/*
 * console.c
 *
 * printf-style interface to the debug console.
 *
 * On the RP2350 the console is the USB CDC-ACM device on the Pico 2's own
 * USB socket (pico2/usb_cdc.c). UART0's pins (GPIO 0/1) carry the FF OSD
 * Amiga keyboard lines instead.
 *
 * Written & released by Keir Fraser <keir.xen@gmail.com>
 *
 * This is free and unencumbered software released into the public domain.
 * See the file COPYING for more details, or visit <http://unlicense.org>.
 */

/* Normally flush to USB is asynchronously executed in a low-pri IRQ. */
#define CONSOLE_SOFTIRQ SOFTIRQ_1
DEFINE_IRQ(CONSOLE_SOFTIRQ, "SOFTIRQ_console");

/* The console can be set into synchronous mode in which case IRQ is disabled
 * and the USB controller is polled to drain the output. */
static bool_t sync_console;

static void SOFTIRQ_console(void)
{
    usb_cdc_kick();
}

static void kick_tx(void)
{
    if (sync_console)
        usb_cdc_flush_sync();
    else
        IRQx_set_pending(CONSOLE_SOFTIRQ);
}

int vprintk(const char *format, va_list ap)
{
    static char str[128];
    char *p, c;
    int n;

    IRQ_global_disable();

    n = vsnprintf(str, sizeof(str), format, ap);

    p = str;
    while ((c = *p++) != '\0') {
        switch (c) {
        case '\r': /* CR: ignore as we generate our own CR/LF */
            break;
        case '\n': /* LF: convert to CR/LF (usual terminal behaviour) */
            usb_cdc_putc('\r');
            /* fall through */
        default:
            usb_cdc_putc(c);
            break;
        }
    }

    kick_tx();

    if (!sync_console)
        IRQ_global_enable();

    return n;
}

int printk(const char *format, ...)
{
    va_list ap;
    int n;

    va_start(ap, format);
    n = vprintk(format, ap);
    va_end(ap);

    return n;
}

void console_sync(void)
{
    if (sync_console)
        return;

    IRQ_global_disable();
    sync_console = TRUE;
    kick_tx();

    /* Leave IRQs globally disabled. */
}

void console_init(void)
{
    IRQx_set_prio(CONSOLE_SOFTIRQ, CONSOLE_IRQ_PRI);
    IRQx_enable(CONSOLE_SOFTIRQ);
}

/* Debug helper: on the STM32 builds, serial input after this call causes a
 * crash dump of the stuck context. The USB console is output-only, so this
 * is a no-op here. */
void console_crash_on_input(void)
{
}
