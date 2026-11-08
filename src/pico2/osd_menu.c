/*
 * pico2/osd_menu.c
 *
 * FF OSD configuration menu, shown on screen. Runs on core 1.
 *
 * Opened with L.Ctrl + L.Alt + Help, or from FlashFloppy's main menu
 * ("Configure FF OSD"), which then passes the front-panel buttons on.
 * Left/Right change the value, Select (Up arrow) moves to the next item.
 *
 * Ported from FF OSD's config.c, written & released by Keir Fraser
 * <keir.xen@gmail.com>. The settings that only apply to FF OSD hardware
 * (output pin and enable, pixel timing, LCD emulation) are gone.
 *
 * This is free and unencumbered software released into the public domain.
 * See the file COPYING for more details, or visit <http://unlicense.org>.
 */

#include "osd_priv.h"

bool_t osd_menu_active;

struct osd_display osd_menu_display = {
    .cols = 16, .rows = 2, .on = TRUE,
};

static enum {
    C_idle = 0,
    C_banner,
    C_polarity,
    C_disp2Y,
    C_h_off,
    C_v_off,
    C_save,
    C_max
} menu_state;

const static char *polarity_pretty[] = { "Low", "High", "Auto" };

static void cnf_prt(int row, const char *format, ...)
{
    char *r = osd_menu_display.text[row];
    va_list ap;

    memset(r, 0, sizeof(osd_menu_display.text[0]));
    va_start(ap, format);
    (void)vsnprintf(r, 20, format, ap);
    va_end(ap);
}

static struct repeat {
    int repeat;
    uint32_t prev;
} left, right;

/* Auto-repeat a held Left/Right: after 500ms, then every 100ms. */
static uint8_t button_repeat(uint8_t pb, uint8_t b, uint8_t m,
                             struct repeat *r)
{
    if (pb & m) {
        /* Is this button held down? */
        if (b & m) {
            uint32_t delta = r->repeat ? 100000 : 500000;
            if (osd_since(r->prev) > delta) {
                /* Repeat this button now. */
                r->repeat++;
            } else {
                /* Not ready to repeat this button. */
                b &= ~m;
            }
        } else {
            /* Button not pressed. Reset repeat count. */
            r->repeat = 0;
        }
    }
    if (b & m) {
        /* Remember when we actioned this button press/repeat. */
        r->prev = osd_now();
    }
    return b;
}

void osd_menu_process(uint8_t b, bool_t autosync_changed)
{
    static enum { C_SAVE = 0, C_USE, C_DISCARD, C_RESET, C_NC_MAX }
        new_config;
    static struct osd_config old_config;
    static uint8_t pb;
    bool_t changed = FALSE;
    uint8_t _b;

    _b = b;
    b &= b ^ (pb & B_SELECT); /* Select acts on press only */
    b = button_repeat(pb, b, B_LEFT, &left);
    b = button_repeat(pb, b, B_RIGHT, &right);
    pb = _b;

    if (b & B_SELECT) {
        if (++menu_state >= C_max) {
            menu_state = C_idle;
            switch (new_config) {
            case C_SAVE:
                osd_config_save(&osd_config);
                break;
            case C_USE:
                break;
            case C_DISCARD:
                osd_config = old_config;
                break;
            case C_RESET:
                osd_config = osd_dfl_config;
                osd_config_save(&osd_config);
                break;
            case C_NC_MAX:
                break;
            }
            if (osd_config.polarity != SYNC_AUTO)
                osd_set_polarity(osd_config.polarity);
        }
        osd_menu_active = (menu_state != C_idle);
        changed = TRUE;
    }

    switch (menu_state) {
    default:
        break;
    case C_banner:
        if (changed) {
            cnf_prt(0, "PicoFloppy OSD");
            cnf_prt(1, "Flash Config");
            old_config = osd_config;
        }
        break;
    case C_polarity:
        if (changed)
            cnf_prt(0, "Sync Polarity:");
        if (b & B_LEFT) {
            if (osd_config.polarity > 0)
                --osd_config.polarity;
            else
                osd_config.polarity = SYNC_MAX-1;
        }
        if (b & B_RIGHT) {
            if (++osd_config.polarity >= SYNC_MAX)
                osd_config.polarity = 0;
        }
        if ((b & (B_LEFT|B_RIGHT)) && (osd_config.polarity != SYNC_AUTO))
            osd_set_polarity(osd_config.polarity);
        if (b || changed || autosync_changed) {
            if (osd_config.polarity == SYNC_AUTO)
                cnf_prt(1, "%s (%s)", polarity_pretty[SYNC_AUTO],
                        polarity_pretty[osd_running_polarity]);
            else
                cnf_prt(1, "%s", polarity_pretty[osd_config.polarity]);
        }
        break;
    case C_disp2Y:
        if (changed)
            cnf_prt(0, "Display Height:");
        if (b & (B_LEFT|B_RIGHT))
            osd_config.display_2Y ^= 1;
        if (b || changed)
            cnf_prt(1, "%s", osd_config.display_2Y ? "Double" : "Normal");
        break;
    case C_h_off:
        if (changed)
            cnf_prt(0, "H.Off (1-199):");
        if (b & B_LEFT)
            osd_config.h_off = max_t(uint16_t, osd_config.h_off-1, 1);
        if (b & B_RIGHT)
            osd_config.h_off = min_t(uint16_t, osd_config.h_off+1, 199);
        if (b || changed)
            cnf_prt(1, "%u", osd_config.h_off);
        break;
    case C_v_off:
        if (changed)
            cnf_prt(0, "V.Off (2-299):");
        if (b & B_LEFT)
            osd_config.v_off = max_t(uint16_t, osd_config.v_off-1, 2);
        if (b & B_RIGHT)
            osd_config.v_off = min_t(uint16_t, osd_config.v_off+1, 299);
        if (b || changed)
            cnf_prt(1, "%u", osd_config.v_off);
        break;
    case C_save: {
        const static char *str[] = { "Save", "Use", "Discard",
                                     "Factory Reset" };
        if (changed) {
            cnf_prt(0, "Save New Config?");
            new_config = C_SAVE;
        }
        if (b & B_LEFT) {
            if (new_config > 0)
                --new_config;
            else
                new_config = C_NC_MAX-1;
        }
        if (b & B_RIGHT)
            if (++new_config >= C_NC_MAX)
                new_config = 0;
        if (b || changed)
            cnf_prt(1, "%s", str[new_config]);
        break;
    }
    }
}
