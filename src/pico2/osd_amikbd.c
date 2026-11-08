/*
 * pico2/osd_amikbd.c
 *
 * FF OSD: watch the Amiga keyboard serial line, and remember key presses.
 * Runs on core 1.
 *
 * The keyboard sends each key code as 8 bits on KB_DATA, each sampled by
 * the Amiga's CIA on a rising edge of KB_CLOCK; the CIA then acknowledges
 * by pulling KB_DATA low (handshake). The osd_amikbd PIO program pushes
 * the data level at every falling clock edge, and the interrupt below
 * assembles the bytes.
 *
 * Key presses with L.Ctrl + L.Alt held (or with the keyboard held) are
 * ours: they are hidden from the Amiga by pulling KB_DATA low during the
 * last bit (the up/down flag), so that the CIA sees a key release.
 *
 * Ported from FF OSD's amiga.c, written & released by Keir Fraser
 * <keir.xen@gmail.com>.
 *
 * This is free and unencumbered software released into the public domain.
 * See the file COPYING for more details, or visit <http://unlicense.org>.
 */

#include "osd_priv.h"
#include "osd.pio.h"

#define OSD_KBD_CLKDIV 4 /* osd_amikbd at 36MHz: 32-cycle glitch filter */

static unsigned int sm_kbd, off_kbd;

/* Per key: bit 0 = held down; bit 1 = pressed since last looked at. */
static uint8_t keymap[0x68];

volatile bool_t amikbd_held;

/* KB_DATA is open drain: low = output enable (level is preset low). */
#define dat_pull_low() gpio_set_dir(OSD_PIN_KBDAT, GPIO_OUT)
#define dat_release()  gpio_set_dir(OSD_PIN_KBDAT, GPIO_IN)

static void handshake(void)
{
    uint32_t t = osd_now();

    /* Wait for rising edge. */
    while ((osd_since(t) < 40) && !gpio_get(OSD_PIN_KBCLK))
        continue;

    /* Allow time for CIA to clock the bit. */
    busy_wait_us_32(5);

    /* Force handshake. 100us is plenty long enough. */
    dat_pull_low();
    busy_wait_us_32(100);
    dat_release();
}

static bool_t modifiers_held(void)
{
    return (keymap[AMI_L_CTRL] & 1) && (keymap[AMI_L_ALT] & 1);
}

static void kbd_bit(int bit)
{
    static uint8_t keycode, bitpos;
    static uint32_t timestamp;
    static int resync;
    uint32_t t = osd_now();

    /* Sync to keycode start by observing delay in comms. */
    if (((t - timestamp) > 1000) && (bitpos != 0)) {
        bitpos = 0;
        if (resync++) {
            /* Two OOS in a row! The keyboard has lost sync: Handshake. */
            handshake();
            return;
        }
    } else {
        resync = 0;
    }

    timestamp = t;
    bitpos = (bitpos + 1) & 7;
    keycode = (keycode << 1) | bit;

    /* Bail if we have not yet received a full byte. */
    if (bitpos != 0)
        return;

    /* Forcibly filter out key presses modified by L.Ctrl + L.Alt: We force
     * them to be viewed as key-release events by blatting KBDAT. */
    if (bit && (modifiers_held() || amikbd_held))
        dat_pull_low();

    /* Decode the keycode and update the keymap. */
    keycode = ~(keycode >> 1) & 0x7f;
    if (keycode < sizeof(keymap)) {
        if (bit) {
            if ((keycode == AMI_RETURN) && modifiers_held())
                amikbd_held ^= 1;
            keymap[keycode] = 3;
        } else {
            keymap[keycode] &= 2;
        }
    }

    keycode |= !bit << 7;
    if (keycode == 0xfd) {
        /* 0xFD: "Initiate power-up key stream".
         * Clear the keymap as all pressed keys are being re-sent. */
        memset(keymap, 0, sizeof(keymap));
    }

    /* Acknowledge the byte (some games and demos have no keyboard handler). */
    handshake();
}

static void isr_kbd(void)
{
    while (!pio_sm_is_rx_fifo_empty(osd_pio, sm_kbd))
        kbd_bit(pio_sm_get(osd_pio, sm_kbd) & 1);
}

bool_t amikbd_key_pressed(uint8_t keycode)
{
    uint8_t state;
    /* Grab keypress state and clear the sticky bit. */
    do {
        state = keymap[keycode];
    } while ((state & 2)
             && (cmpxchg(&keymap[keycode], state, state & 1) != state));
    /* Only return key presses while the modifier keys are held. */
    return (modifiers_held() || amikbd_held) ? !!state : FALSE;
}

void amikbd_hw_init(void)
{
    pio_sm_config c;

    /* KB_DATA, KB_CLOCK: inputs, without internal pulls (the Amiga pulls
     * them up to 5V). KB_DATA is driven low only for the handshake. */
    gpio_configure_pin(gpioa, OSD_PIN_KBDAT, GPI_floating);
    gpio_configure_pin(gpioa, OSD_PIN_KBCLK, GPI_floating);
    gpio_put(OSD_PIN_KBDAT, 0);

    off_kbd = pio_add_program(osd_pio, &osd_amikbd_program);
    sm_kbd = pio_claim_unused_sm(osd_pio, true);

    /* in_base = KB_DATA (and KB_CLOCK next), jmp_pin = KB_CLOCK. Each push
     * holds one sample in bit 0. */
    c = osd_amikbd_program_get_default_config(off_kbd);
    sm_config_set_in_pins(&c, OSD_PIN_KBDAT);
    sm_config_set_jmp_pin(&c, OSD_PIN_KBCLK);
    sm_config_set_in_shift(&c, false, false, 32);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);
    sm_config_set_clkdiv_int_frac8(&c, OSD_KBD_CLKDIV, 0);
    pio_sm_init(osd_pio, sm_kbd, off_kbd, &c);
}

void amikbd_start(void)
{
    irq_set_exclusive_handler(OSD_KBD_IRQ, isr_kbd);
    irq_set_priority(OSD_KBD_IRQ, OSD_KBD_PRI);
    pio_set_irqn_source_enabled(
        osd_pio, 1, pio_get_rx_fifo_not_empty_interrupt_source(sm_kbd),
        true);
    irq_set_enabled(OSD_KBD_IRQ, true);
    pio_sm_set_enabled(osd_pio, sm_kbd, true);
}
