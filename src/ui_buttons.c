/*
 * ui_buttons.c
 *
 * Front-panel buttons and rotary encoder: sampling, debounce and
 * decoding, from a periodic timer.
 *
 * Split out of main.c. Written & released by Keir Fraser
 * <keir.xen@gmail.com>.
 *
 * This is free and unencumbered software released into the public domain.
 * See the file COPYING for more details, or visit <http://unlicense.org>.
 */

#include "main.h"

static uint8_t v2_read_rotary(uint8_t rotary)
{
    /* Rotary encoder outputs a Gray code, counting clockwise: 00-01-11-10. */
    const uint32_t rotary_transitions[] = {
        [ROT_none]    = 0x00000000, /* No encoder */
        [ROT_full]    = 0x20000100, /* 4 transitions (full cycle) per detent */
        [ROT_half]    = 0x24000018, /* 2 transitions (half cycle) per detent */
        [ROT_quarter] = 0x24428118  /* 1 transition (quarter cyc) per detent */
    };
    return (rotary_transitions[ff_cfg.rotary & 3] >> (rotary << 1)) & 3;
}

static uint8_t read_rotary(uint8_t rotary)
{
    /* Rotary encoder outputs a Gray code, counting clockwise: 00-01-11-10. */
    const uint32_t rotary_transitions = 0x24428118;

    /* Number of back-to-back transitions we see per detent on various 
     * types of rotary encoder. */
    const uint8_t rotary_transitions_per_detent[] = {
        [ROT_full] = 4, [ROT_half] = 2, [ROT_quarter] = 1
    };

    /* p_t(x) returns the previous valid transition in same direction. 
     * eg. p_t(0b0111) == 0b0001 */
#define p_t(x) (((x)>>2)|((((x)^3)&3)<<2))

    /* Bitmask of which valid 4-bit transition codes we have seen in each 
     * direction (CW and CCW). */
    static uint16_t t_seen[2];

    uint16_t ts;
    uint8_t rb;

    /* Check if we have seen a valid CW or CCW state transition. */
    rb = (rotary_transitions >> (rotary << 1)) & 3;
    if (likely(!rb))
        return 0; /* Nope */

    /* Have we seen the /previous/ transition in this direction? If not, any 
     * previously-recorded transitions are not in a contiguous step-wise
     * sequence, and should be discarded as switch bounce. */
    ts = t_seen[rb-1];
    if (!(ts & (1<<p_t(rotary))))
        ts = 0; /* Clear any existing bounce transitions. */

    /* Record this transition and check if we have seen enough to get 
     * us from one detent to another. */
    ts |= (1<<rotary);
    if ((popcount(ts) < rotary_transitions_per_detent[ff_cfg.rotary & 3])
        || (((ff_cfg.rotary & 3) == ROT_full) && ((rotary & 3) != 3))) {
        /* Not enough transitions yet: Remember where we are for next time. */
        t_seen[rb-1] = ts;
        return 0;
    }

    /* This is a valid movement between detents. Clear transition state 
     * and return the movement to the caller. */
    t_seen[0] = t_seen[1] = 0;
    return rb;
#undef p_t
}

struct timer button_timer;
volatile uint8_t buttons, velocity;
uint8_t rotary;
static uint8_t rb;

void IRQ_rotary(void)
{
    if ((ff_cfg.rotary & ROT_typemask) != ROT_full)
        return;
    rotary = ((rotary << 2) | board_get_rotary()) & 15;
    rb = read_rotary(rotary) ?: rb;
}

void set_rotary_exti(void)
{
    gpio_irq_disable_mask(board_rotary_exti_mask);
    board_rotary_exti_mask = 0;
    if ((ff_cfg.rotary & ROT_typemask) == ROT_full)
        board_setup_rotary_exti();
}

void button_timer_fn(void *unused)
{
    const uint8_t rotary_reverse[4] = {
        [B_LEFT] = B_RIGHT, [B_RIGHT] = B_LEFT
    };

    static uint16_t cur_time, prev_time;
    static uint32_t _b[3]; /* 0 = left, 1 = right, 2 = select */
    uint8_t x, b = osd_buttons_rx;
    bool_t twobutton_rotary =
        (ff_cfg.twobutton_action & TWOBUTTON_mask) == TWOBUTTON_rotary;
    int i, twobutton_reverse = !!(ff_cfg.twobutton_action & TWOBUTTON_reverse);

    cur_time++;
    if ((uint16_t)(cur_time - prev_time) > 0x7fff)
        prev_time = cur_time - 0x7fff;
    velocity = 0;

    /* We debounce the switches by waiting for them to be pressed continuously 
     * for 32 consecutive sample periods (32 * 2ms == 64ms) */
    x = ~board_get_buttons();
    for (i = 0; i < 3; i++) {
        _b[i] <<= 1;
        _b[i] |= x & 1;
        x >>= 1;
    }

    if (_b[twobutton_reverse] == 0)
        b |= twobutton_rotary ? B_LEFT|B_RIGHT : B_LEFT;

    if (_b[!twobutton_reverse] == 0)
        b |= twobutton_rotary ? B_SELECT : B_RIGHT;

    if (_b[2] == 0)
        b |= B_SELECT;

    rotary = ((rotary << 2) | board_get_rotary()) & 15;
    switch (ff_cfg.rotary & ROT_typemask) {

    case ROT_trackball: {
        static uint16_t count, thresh, dir;
        rb = rotary_reverse[(rotary ^ (rotary >> 2)) & 3];
        if (rb == 0) {
            /* Idle: Increase threshold, decay the counter. */
            thresh = min_t(int, thresh + BUTTON_SCAN_MS, 360);
            count = max_t(int, count - BUTTON_SCAN_MS, 0);
        } else if (rb != dir) {
            /* Change of direction: Put the brakes on. */
            dir = rb;
            count = rb = 0;
            thresh = 360;
        } else {
            /* Step in same direction: Increase count, decay the threshold. */
            count += 160;
            thresh = max_t(int, 0, thresh - 40);
            if (count >= thresh) {
                /* Count exceeds threshold: register a press. */
                count = 0;
            } else {
                /* Don't register a press yet. */
                rb = 0;
            }
        }
        break;
    }

    case ROT_buttons:
        rb = rotary_reverse[rotary & 3];
        break;

    case ROT_none:
        break;

    default: /* rotary encoder */ {
        rb = (ff_cfg.rotary & ROT_v2) ? v2_read_rotary(rotary)
            : (read_rotary(rotary) ?: rb);
        if (rb) {
            uint16_t delta = cur_time - prev_time;
            velocity = (BUTTON_SCAN_HZ/10)/(delta?:1);
            velocity = range_t(int, velocity, 0, 20);
            prev_time = cur_time;
        }
        break;
    }

    }
    if (ff_cfg.rotary & ROT_reverse)
        rb = rotary_reverse[rb];
    b |= rb;
    rb = 0;

    switch (display_type) {
    case DT_LCD_OLED:
        b = lcd_handle_backlight(b);
        break;
    case DT_LED_7SEG:
        b = led_handle_display(b);
        break;
    }

    /* Latch final button state and reset the timer. */
    buttons = b;
    timer_set(&button_timer, button_timer.deadline + time_ms(BUTTON_SCAN_MS));
}
