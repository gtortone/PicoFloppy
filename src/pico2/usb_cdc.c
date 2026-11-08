/*
 * pico2/usb_cdc.c
 *
 * USB CDC-ACM device for the RP2350, on TinyUSB (as shipped with the
 * pico-sdk), providing:
 *  - the debug console, over the Pico 2's own USB connector; and
 *  - the Arduino "touch the port at 1200 baud" gesture: opening and then
 *    closing the port with the line rate set to 1200 reboots the board
 *    into BOOTSEL, so a UF2 can be dropped on it without unplugging.
 *
 * Interrupt structure: TinyUSB's controller ISR (USBCTRL_IRQ) only queues
 * events; a shared handler chained after it pends a spare "task" IRQ, in
 * which tud_task() and the console drain run. Both interrupts sit at the
 * same priority (USB_IRQ_PRI, passed in by the caller), below every
 * floppy-critical interrupt: flux generation is never delayed by USB
 * activity, and all TinyUSB device-stack calls are serialised.
 *
 * Built against the pico-sdk and TinyUSB only (no FlashFloppy headers):
 * the interface is declared in include/mcu/rp2350.h.
 *
 * This is free and unencumbered software released into the public domain.
 * See the file COPYING for more details, or visit <http://unlicense.org>.
 */

#include "tusb.h"
#include "pico/bootrom.h"
#include "hardware/irq.h"
#include "hardware/timer.h"
#include "hardware/structs/nvic.h"

void usb_cdc_init(unsigned int irq_pri);
void usb_cdc_putc(char c);
void usb_cdc_kick(void);
void usb_cdc_flush_sync(void);
void usb_descriptors_init(void); /* usb_descriptors.c */

/* Console transmit ring: filled by usb_cdc_putc() from any context,
 * drained into TinyUSB's CDC FIFO by the task IRQ. Large enough to hold
 * the whole start-up log until a terminal is opened. */
static char txring[4096];
#define TXMASK(x) ((x)&(sizeof(txring)-1))
static volatile unsigned int txcons, txprod;

/* Spare IRQ which runs the device stack (tud_task). */
static unsigned int task_irq;

/* Set once usb_cdc_init() has run: the console calls in from boot. */
static volatile bool usb_ready;

/* 1200-baud gesture seen: reboot once the current request is answered. */
static volatile bool reboot_pending;

static void usb_service(void)
{
    unsigned int c, p, n, avail;

    tud_task();

    /* Move console output into the CDC FIFO, while someone is listening.
     * With no terminal attached the ring simply fills (and then drops new
     * output), so the start of the log is there when one is opened. */
    if (tud_cdc_connected()) {
        while ((c = txcons) != (p = txprod)) {
            avail = tud_cdc_write_available();
            if (avail == 0)
                break;
            n = p - c;
            if (n > (sizeof(txring) - TXMASK(c)))
                n = sizeof(txring) - TXMASK(c); /* contiguous stretch */
            if (n > avail)
                n = avail;
            n = tud_cdc_write(&txring[TXMASK(c)], n);
            txcons = c + n;
        }
        tud_cdc_write_flush();
    }

    if (reboot_pending)
        reset_usb_boot(0, 0);
}

static void usb_task_irq(void)
{
    usb_service();
}

/* Chained after TinyUSB's own USBCTRL_IRQ handler. */
static void usbctrl_irq(void)
{
    irq_set_pending(task_irq);
}

/*
 * TinyUSB callbacks (from tud_task(), hence task IRQ context).
 */

/* The 1200-baud gesture: the host has both selected a 1200-baud line rate
 * and dropped DTR (which is what closing the port does). */
void tud_cdc_line_state_cb(uint8_t itf, bool dtr, bool rts)
{
    cdc_line_coding_t coding;

    (void)rts;
    tud_cdc_n_get_line_coding(itf, &coding);
    if (!dtr && (coding.bit_rate == 1200))
        reboot_pending = true;
}

/* Host->device data is ignored: the console is output-only. */
void tud_cdc_rx_cb(uint8_t itf)
{
    tud_cdc_n_read_flush(itf);
}

/*
 * Public interface
 */

/* Queue one character for the host. Dropped if the ring is full: the
 * console must never block the emulator. */
void usb_cdc_putc(char c)
{
    unsigned int p = txprod;

    if ((p - txcons) >= (sizeof(txring) - 1))
        return;

    txring[TXMASK(p)] = c;
    __compiler_memory_barrier();
    txprod = p + 1;
}

void usb_cdc_kick(void)
{
    if (usb_ready)
        irq_set_pending(task_irq);
}

static bool irq_is_active(unsigned int irq)
{
    return (nvic_hw->iabr[irq / 32] >> (irq % 32)) & 1;
}

/* Drain the transmit ring synchronously (console_sync(), crash dumps),
 * polling the controller. Bounded, so a vanished host cannot wedge us here.
 * Skipped if we have interrupted the USB stack itself, which cannot be
 * re-entered. */
void usb_cdc_flush_sync(void)
{
    static bool gave_up;
    uint32_t t = time_us_32();

    if (!usb_ready || gave_up
        || irq_is_active(USBCTRL_IRQ) || irq_is_active(task_irq))
        return;

    while (tud_cdc_connected()
           && ((txcons != txprod)
               || (tud_cdc_write_available() < CFG_TUD_CDC_TX_BUFSIZE))) {
        if ((time_us_32() - t) > 100000) {
            gave_up = true;
            break;
        }
        tud_int_handler(0);
        usb_service();
    }
}

void usb_cdc_init(unsigned int irq_pri)
{
    usb_descriptors_init();

    /* Task IRQ: a spare (software) interrupt line. */
    task_irq = user_irq_claim_unused(true);
    irq_set_exclusive_handler(task_irq, usb_task_irq);
    irq_set_priority(task_irq, irq_pri << 4);
    irq_set_enabled(task_irq, true);

    /* TinyUSB installs its controller ISR as a shared handler on
     * USBCTRL_IRQ, and enables the interrupt. Ours runs after it. */
    tusb_init();
    irq_add_shared_handler(USBCTRL_IRQ, usbctrl_irq,
                           PICO_SHARED_IRQ_HANDLER_LOWEST_ORDER_PRIORITY);
    irq_set_priority(USBCTRL_IRQ, irq_pri << 4);

    usb_ready = true;
    /* Pick up any console output from before we were ready. */
    irq_set_pending(task_irq);
}
