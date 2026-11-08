/*
 * ui_display.c
 *
 * Front-panel display rendering for the main loop: image name and
 * scrolling, track/status readout, backlight and 7-segment display modes.
 *
 * Split out of main.c. Written & released by Keir Fraser
 * <keir.xen@gmail.com>.
 *
 * This is free and unencumbered software released into the public domain.
 * See the file COPYING for more details, or visit <http://unlicense.org>.
 */

#include "main.h"

uint32_t display_ticks;
uint8_t display_state;
struct lcd_scroll lcd_scroll;

/* Turn the LCD backlight on, reset the switch-off handler and ticker. */
void lcd_on(void)
{
    if (display_type != DT_LCD_OLED)
        return;
    display_ticks = 0;
    barrier();
    display_state = BACKLIGHT_ON;
    barrier();
    lcd_backlight(ff_cfg.display_off_secs != 0);
}

void lcd_scroll_init(uint16_t pause, uint16_t rate)
{
    int diff = lcd_scroll.off - lcd_scroll.end;
    lcd_scroll.pause = pause;
    lcd_scroll.rate = rate;
    lcd_scroll.end = max_t(
        int, strnlen(cfg.slot.name, sizeof(cfg.slot.name)) - lcd_columns, 0);
    if (lcd_scroll.end && !lcd_scroll.pause)
        lcd_scroll.end += lcd_columns;
    if (lcd_scroll.off > lcd_scroll.end)
        lcd_scroll.off = lcd_scroll.pause || !lcd_scroll.end
            ? 0 : lcd_scroll.end + diff;
}
void lcd_scroll_name(void)
{
    static struct track_info ti;
    char msg[lcd_columns+1];

    if ((lcd_scroll.ticks > 0) || (lcd_scroll.end == 0))
        return;

    floppy_get_track(&ti);
    if (ti.in_da_mode) {
        /* Display controlled by src/image/da.c */
        return;
    }

    lcd_scroll.ticks = time_ms(lcd_scroll.rate);
    if (lcd_scroll.pause != 0) {
        if (++lcd_scroll.off > lcd_scroll.end)
            lcd_scroll.off = 0;
        snprintf(msg, sizeof(msg), "%s", cfg.slot.name + lcd_scroll.off);
        if ((lcd_scroll.off == 0)
            || (lcd_scroll.off == lcd_scroll.end))
            lcd_scroll.ticks = time_ms(lcd_scroll.pause);
    } else {
        const unsigned int scroll_gap = 4;
        lcd_scroll.off++;
        if (lcd_scroll.off <= lcd_scroll.end) {
            snprintf(msg, sizeof(msg), "%s%*s%s",
                     cfg.slot.name + lcd_scroll.off,
                     scroll_gap, "", cfg.slot.name);
        } else {
            snprintf(msg, sizeof(msg), "%*s%s",
                     scroll_gap - (lcd_scroll.off - lcd_scroll.end), "",
                     cfg.slot.name);
            if ((lcd_scroll.off - lcd_scroll.end) == scroll_gap)
                lcd_scroll.off = 0;
        }
    }
    lcd_write(0, 0, -1, msg);
}

/* Write slot info to display. */
void display_write_slot(bool_t nav_mode)
{
    const struct image_type *type;
    char msg[lcd_columns+1], typename[4] = "";
    unsigned int i;

    if (display_type != DT_LCD_OLED) {
        if (display_type == DT_LED_7SEG)
            led_7seg_write_decimal(cfg.slot_nr);
        return;
    }

    if (nav_mode && !cfg_scroll_reset) {
        lcd_scroll_init(0, ff_cfg.nav_scroll_rate);
        if (lcd_scroll.end == 0) {
            snprintf(msg, sizeof(msg), "%s", cfg.slot.name);
            lcd_write(0, 0, -1, msg);
        } else {
            lcd_scroll.off--;
            lcd_scroll.ticks = 0;
            lcd_scroll_name();
        }
    } else {
        snprintf(msg, sizeof(msg), "%s", cfg.slot.name);
        lcd_write(0, 0, -1, msg);
    }

    if (slot_type("v9t9")) {
        snprintf(typename, sizeof(typename), "T99");
    } else if (!(cfg.slot.attributes & AM_DIR)) {
        for (type = &image_type[0]; type->handler != NULL; type++)
            if (slot_type(type->ext))
                break;
        if (type->handler != NULL) {
            snprintf(typename, sizeof(typename), "%s", type->ext);
            for (i = 0; i < sizeof(typename); i++)
                typename[i] = toupper(typename[i]);
        }
    }

    snprintf(msg, sizeof(msg), "%03u/%03u%*s%3s D:%u",
             cfg.slot_nr, cfg.max_slot_nr,
             (lcd_columns > 16) ? 3 : 1, "",
             typename, cfg.depth);

    if (cfg.hxc_mode) {
        /* HxC mode: Exclude depth from the info message. */
        char *p = strrchr(msg, 'D');
        if (p)
            *p = '\0';
    }

    lcd_write(0, 1, -1, msg);
    lcd_on();
}

/* Write track number to LCD. */
void lcd_write_track_info(bool_t force)
{
    static struct track_info lcd_ti;
    struct track_info ti;
    char msg[17];

    if (display_type != DT_LCD_OLED)
        return;

    floppy_get_track(&ti);

    if (lcd_columns <= 16)
        ti.cyl = min_t(uint8_t, ti.cyl, 99);
    ASSERT(ti.side <= 1);

    if (force || (ti.cyl != lcd_ti.cyl)
        || ((ti.side != lcd_ti.side) && ti.sel)
        || (ti.writing != lcd_ti.writing)) {
        snprintf(msg, sizeof(msg), "%c T:%02u.%u",
                 (cfg.slot.attributes & AM_RDO) ? '*' : ti.writing ? 'W' : ' ',
                 ti.cyl, ti.side);
        lcd_write(wp_column, 1, -1, msg);
        if (ff_cfg.display_on_activity != DISPON_no)
            lcd_on();
        lcd_ti = ti;
    } else if ((ff_cfg.display_on_activity == DISPON_sel) && ti.sel) {
        lcd_on();
    }
}

void led_7seg_update_track(bool_t force)
{
    static struct track_info led_ti;
    static bool_t showing_track;
    static uint8_t active_countdown;

    bool_t changed;
    struct track_info ti;
    char msg[4];

    if (display_type != DT_LED_7SEG)
        return;

    floppy_get_track(&ti);
    changed = (ti.cyl != led_ti.cyl) || ((ti.side != led_ti.side) && ti.sel)
        || (ti.writing != led_ti.writing);

    if (force) {
        /* First call afer mounting new image: forcibly show track nr. */
        display_state = LED_TRACK;
        showing_track = FALSE;
        changed = TRUE;
    }

    if (ti.in_da_mode) {
        /* Display controlled by src/image/da.c */
        display_state = LED_NORMAL;
    }

    if (changed) {
        /* We will show new track nr unless overridden by a button press. */
        if (display_state == LED_TRACK_QUIESCENT)
            display_state = LED_TRACK;
        active_countdown = 50*4;
        led_ti = ti;
    } else if (active_countdown != 0) {
        /* Count down towards reverting to showing image nr. */
        active_countdown--;
    }

    if ((display_state != LED_TRACK) || (active_countdown == 0)) {
        if (showing_track)
            display_write_slot(FALSE);
        showing_track = FALSE;
        active_countdown = 0;
        if (display_state == LED_TRACK)
            display_state = LED_TRACK_QUIESCENT;
        return;
    }

    if (!showing_track || changed) {
        const static char status[] = { 'k', 'm', 'v', 'w' };
        snprintf(msg, sizeof(msg), "%2u%c", ti.cyl,
                 status[ti.side|(ti.writing<<1)]);
        led_7seg_write_string(msg);
        showing_track = TRUE;
    }
}

/* Handle switching the LCD backlight. */
uint8_t lcd_handle_backlight(uint8_t b)
{
    if ((ff_cfg.display_off_secs == 0)
        || (ff_cfg.display_off_secs == 0xff))
        return b;

    switch (display_state) {
    case BACKLIGHT_OFF:
        if (!b)
            break;
        /* First button press turns on the backlight. Nothing more. */
        b = 0;
        display_state = BACKLIGHT_SWITCHING_ON;
        lcd_backlight(TRUE);
        break;
    case BACKLIGHT_SWITCHING_ON:
        /* We sit in this state until the button is released. */
        if (!b)
            display_state = BACKLIGHT_ON;
        b = 0;
        display_ticks = 0;
        break;
    case BACKLIGHT_ON:
        /* After a period with no button activity we turn the backlight off. */
        if (b)
            display_ticks = 0;
        if (display_ticks++ >= BUTTON_SCAN_HZ*ff_cfg.display_off_secs) {
            lcd_backlight(FALSE);
            display_state = BACKLIGHT_OFF;
        }
        break;
    }

    return b;
}

uint8_t led_handle_display(uint8_t b)
{
    switch (display_state) {
    case LED_TRACK:
        if (!b)
            break;
        /* First button press switches to image number. Nothing more. */
        b = 0;
        display_state = LED_BUTTON_HELD;
        break;
    case LED_BUTTON_HELD:
        /* We sit in this state until the button is released. */
        if (!b)
            display_state = LED_BUTTON_RELEASED;
        b = 0;
        display_ticks = 0;
        break;
    case LED_BUTTON_RELEASED:
        /* After a period with no button activity we return to track number. */
        if (display_ticks++ >= BUTTON_SCAN_HZ*3)
            display_state = LED_TRACK;
        break;
    }

    return b;
}
