/*
 * ff_cfg_read.c
 *
 * FF.CFG parsing, and applying changed options at run time.
 *
 * Split out of main.c. Written & released by Keir Fraser
 * <keir.xen@gmail.com>.
 *
 * This is free and unencumbered software released into the public domain.
 * See the file COPYING for more details, or visit <http://unlicense.org>.
 */

#include "main.h"

/* Parse pinNN= config value. */
static uint8_t parse_pin_str(const char *s)
{
    uint8_t pin = 0;
    if (*s == 'n') {
        pin = PIN_invert;
        s++;
    }
    pin ^= !strcmp(s, "low") ? PIN_low
        : !strcmp(s, "high") ? PIN_high
        : !strcmp(s, "c") ? (PIN_invert | PIN_nc)
        : !strcmp(s, "rdy") ? PIN_rdy
        : !strcmp(s, "dens") ? PIN_dens
        : !strcmp(s, "chg") ? PIN_chg
        : PIN_auto;
    return pin;
}

static uint16_t parse_display_order(const char *p)
{
    int sh = 0;
    uint16_t order = 0;

    if (!strcmp(p, "default"))
        return DORD_default;

    while (p != NULL) {
        order |= ((p[0]-'0')&7) << sh;
        if (p[1] == 'd')
            order |= DORD_double << sh;
        sh += DORD_shift;
        if ((p = strchr(p, ',')) == NULL)
            break;
        p++;
    }

    if (sh < 16)
        order |= 0x7777 << sh;

    return order;
}

void read_ff_cfg(void)
{
    enum {
#define x(n,o,v) FFCFG_##o,
#include "ff_cfg_defaults.h"
#undef x
        FFCFG_nr
    };

    const static struct opt ff_cfg_opts[FFCFG_nr+1] = {
#define x(n,o,v) [FFCFG_##o] = { #n },
#include "ff_cfg_defaults.h"
#undef x
    };

    FRESULT fr;
    int option;
    struct opts opts = {
        .file = &fs->file,
        .opts = ff_cfg_opts,
        .arg = fs->buf,
        .argmax = sizeof(fs->buf)-1
    };

    fatfs.cdir = cfg.cfg_cdir;
    fr = F_try_open(&fs->file, "FF.CFG", FA_READ);
    if (fr)
        return;

    while ((option = get_next_opt(&opts)) != -1) {

        switch (option) {

            /* DRIVE EMULATION */

        case FFCFG_interface:
            ff_cfg.interface =
                !strcmp(opts.arg, "shugart") ? FINTF_SHUGART
                : !strcmp(opts.arg, "ibmpc") ? FINTF_IBMPC
                : !strcmp(opts.arg, "ibmpc-hdout") ? FINTF_IBMPC_HDOUT
                : !strcmp(opts.arg, "jppc") ? FINTF_JPPC
                : !strcmp(opts.arg, "jppc-hdout") ? FINTF_JPPC_HDOUT
                : !strcmp(opts.arg, "akai-s950") ? FINTF_JPPC_HDOUT
                : !strcmp(opts.arg, "amiga") ? FINTF_AMIGA
                : FINTF_JC;
            break;

        case FFCFG_host:
            ff_cfg.host =
                !strcmp(opts.arg, "acorn") ? HOST_acorn
                : !strcmp(opts.arg, "akai") ? HOST_akai
                : !strcmp(opts.arg, "casio") ? HOST_casio
                : !strcmp(opts.arg, "dec") ? HOST_dec
                : !strcmp(opts.arg, "ensoniq") ? HOST_ensoniq
                : !strcmp(opts.arg, "fluke") ? HOST_fluke
                : !strcmp(opts.arg, "gem") ? HOST_gem
                : !strcmp(opts.arg, "ibm-3174") ? HOST_ibm_3174
                : !strcmp(opts.arg, "memotech") ? HOST_memotech
                : !strcmp(opts.arg, "msx") ? HOST_msx
                : !strcmp(opts.arg, "nascom") ? HOST_nascom
                : !strcmp(opts.arg, "pc98") ? HOST_pc98
                : !strcmp(opts.arg, "pc-dos") ? HOST_pc_dos
                : !strcmp(opts.arg, "tandy-coco") ? HOST_tandy_coco
                : !strcmp(opts.arg, "ti99") ? HOST_ti99
                : !strcmp(opts.arg, "uknc") ? HOST_uknc
                : HOST_unspecified;
            break;

        case FFCFG_pin02:
            ff_cfg.pin02 = parse_pin_str(opts.arg);
            break;

        case FFCFG_pin34:
            ff_cfg.pin34 = parse_pin_str(opts.arg);
            break;

        case FFCFG_write_protect:
            ff_cfg.write_protect = !strcmp(opts.arg, "yes");
            break;

        case FFCFG_max_cyl:
            ff_cfg.max_cyl = strtol(opts.arg, NULL, 10);
            break;

        case FFCFG_side_select_glitch_filter:
            ff_cfg.side_select_glitch_filter = strtol(opts.arg, NULL, 10);
            break;

        case FFCFG_track_change:
            ff_cfg.track_change =
                !strcmp(opts.arg, "realtime") ? TRKCHG_realtime
                : TRKCHG_instant;
            break;

        case FFCFG_write_drain:
            ff_cfg.write_drain =
                !strcmp(opts.arg, "realtime") ? WDRAIN_realtime
                : !strcmp(opts.arg, "eot") ? WDRAIN_eot
                : WDRAIN_instant;
            break;

        case FFCFG_index_suppression:
            ff_cfg.index_suppression = !strcmp(opts.arg, "yes");
            break;

        case FFCFG_head_settle_ms:
            ff_cfg.head_settle_ms = strtol(opts.arg, NULL, 10);
            break;

        case FFCFG_motor_delay:
            ff_cfg.motor_delay =
                !strcmp(opts.arg, "ignore") ? MOTOR_ignore
                : (strtol(opts.arg, NULL, 10) + 9) / 10;
            break;

        case FFCFG_chgrst:
            ff_cfg.chgrst =
                !strncmp(opts.arg, "delay-", 6)
                  ? CHGRST_delay(strtol(opts.arg+6, NULL, 10))
                : !strcmp(opts.arg, "pa14") ? CHGRST_pa14
                : CHGRST_step;
            break;

            /* STARTUP / INITIALISATION */

        case FFCFG_ejected_on_startup:
            ff_cfg.ejected_on_startup = !strcmp(opts.arg, "yes");
            break;

        case FFCFG_image_on_startup:
            ff_cfg.image_on_startup =
                !strcmp(opts.arg, "static") ? IMGS_static
                : !strcmp(opts.arg, "last") ? IMGS_last : IMGS_init;
            break;

        case FFCFG_display_probe_ms:
            ff_cfg.display_probe_ms = strtol(opts.arg, NULL, 10);
            break;

            /* IMAGE NAVIGATION */

        case FFCFG_autoselect_file_secs:
            ff_cfg.autoselect_file_secs = strtol(opts.arg, NULL, 10);
            break;

        case FFCFG_autoselect_folder_secs:
            ff_cfg.autoselect_folder_secs = strtol(opts.arg, NULL, 10);
            break;

        case FFCFG_folder_sort:
            ff_cfg.folder_sort =
                !strcmp(opts.arg, "never") ? SORT_never
                : !strcmp(opts.arg, "small") ? SORT_small
                : SORT_always;
            break;

        case FFCFG_sort_priority:
            ff_cfg.sort_priority =
                !strcmp(opts.arg, "none") ? SORTPRI_none
                : !strcmp(opts.arg, "files") ? SORTPRI_files
                : SORTPRI_folders;
            break;

        case FFCFG_nav_mode:
            ff_cfg.nav_mode =
                !strcmp(opts.arg, "native") ? NAVMODE_native
                : !strcmp(opts.arg, "indexed") ? NAVMODE_indexed
                : NAVMODE_default;
            break;

        case FFCFG_nav_loop:
            ff_cfg.nav_loop = !strcmp(opts.arg, "yes");
            break;

        case FFCFG_twobutton_action: {
            char *p, *q;
            ff_cfg.twobutton_action = TWOBUTTON_zero;
            for (p = opts.arg; *p != '\0'; p = q) {
                for (q = p; *q && *q != ','; q++)
                    continue;
                if (*q == ',')
                    *q++ = '\0';
                if (!strcmp(p, "reverse")) {
                    ff_cfg.twobutton_action |= TWOBUTTON_reverse;
                } else {
                    ff_cfg.twobutton_action &= TWOBUTTON_reverse;
                    ff_cfg.twobutton_action |=
                        !strcmp(p, "rotary") ? TWOBUTTON_rotary
                        : !strcmp(p, "rotary-fast") ? TWOBUTTON_rotary_fast
                        : !strcmp(p, "eject") ? TWOBUTTON_eject
                        : !strcmp(p, "htu") ? TWOBUTTON_htu
                        : TWOBUTTON_zero;
                }
            }
            break;
        }

        case FFCFG_rotary: {
            char *p, *q;
            ff_cfg.rotary = ROT_full;
            for (p = opts.arg; *p != '\0'; p = q) {
                for (q = p; *q && *q != ','; q++)
                    continue;
                if (*q == ',')
                    *q++ = '\0';
                if (!strcmp(p, "reverse")) {
                    ff_cfg.rotary |= ROT_reverse;
                } else if (!strcmp(p, "v2")) {
                    ff_cfg.rotary |= ROT_v2;
                } else {
                    ff_cfg.rotary &= ~ROT_typemask;
                    ff_cfg.rotary |=
                        !strcmp(p, "quarter") ? ROT_quarter
                        : !strcmp(p, "half") ? ROT_half
                        : !strcmp(p, "trackball") ? ROT_trackball
                        : !strcmp(p, "buttons") ? ROT_buttons
                        : !strcmp(p, "none") ? ROT_none
                        : ROT_full;
                }
            }
            break;
        }

        case FFCFG_indexed_prefix:
            memset(ff_cfg.indexed_prefix, 0,
                   sizeof(ff_cfg.indexed_prefix));
            snprintf(ff_cfg.indexed_prefix,
                     sizeof(ff_cfg.indexed_prefix),
                     "%s", opts.arg);
            break;

            /* DISPLAY */

        case FFCFG_display_type: {
            char *p, *q, *r;
            ff_cfg.display_type = DISPLAY_auto;
            for (p = opts.arg; *p != '\0'; p = q) {
                for (q = p; *q && *q != '-'; q++)
                    continue;
                if (*q == '-')
                    *q++ = '\0';
                if (!strcmp(p, "lcd")) {
                    ff_cfg.display_type = DISPLAY_lcd;
                } else if (!strcmp(p, "oled")) {
                    ff_cfg.display_type = DISPLAY_oled;
                } else if ((r = strchr(p, 'x')) != NULL) {
                    unsigned int w, h;
                    *r++ = '\0';
                    w = strtol(p, NULL, 10);
                    h = strtol(r, NULL, 10);
                    if (ff_cfg.display_type & DISPLAY_oled) {
                        if (h == 64)
                            ff_cfg.display_type |= DISPLAY_oled_64;
                    } else if (ff_cfg.display_type & DISPLAY_lcd) {
                        ff_cfg.display_type |= DISPLAY_lcd_columns(w);
                        ff_cfg.display_type |= DISPLAY_lcd_rows(h);
                    }
                } else if (ff_cfg.display_type & DISPLAY_oled) {
                    if (!strcmp(p, "rotate")) {
                        ff_cfg.display_type |= DISPLAY_rotate;
                    } else if (!strncmp(p, "narrow", 6)) {
                        ff_cfg.display_type |=
                            (p[6] == 'e') ? DISPLAY_narrower : DISPLAY_narrow;
                    } else if (!strcmp(p, "inverse")) {
                        ff_cfg.display_type |= DISPLAY_inverse;
                    } else if (!strcmp(p, "ztech")) {
                        ff_cfg.display_type |= DISPLAY_ztech;
                    } else if (!strcmp(p, "slow")) {
                        ff_cfg.display_type |= DISPLAY_slow;
                    } else if (!strcmp(p, "hflip")) {
                        ff_cfg.display_type |= DISPLAY_hflip;
                    }
                }
            }
            break;
        }

        case FFCFG_oled_font:
            ff_cfg.oled_font =
                !strcmp(opts.arg, "6x13") ? FONT_6x13
                : FONT_8x16;
            break;

        case FFCFG_oled_contrast:
            ff_cfg.oled_contrast = strtol(opts.arg, NULL, 10);
            break;

        case FFCFG_display_order:
            ff_cfg.display_order = parse_display_order(opts.arg);
            break;

        case FFCFG_osd_display_order:
            ff_cfg.osd_display_order = parse_display_order(opts.arg);
            break;

        case FFCFG_osd_columns:
            ff_cfg.osd_columns = strtol(opts.arg, NULL, 10);
            break;

        case FFCFG_display_off_secs:
            ff_cfg.display_off_secs = strtol(opts.arg, NULL, 10);
            cfg.ffcfg_has_display_off_secs = TRUE;
            break;

        case FFCFG_display_on_activity:
            ff_cfg.display_on_activity =
                !strcmp(opts.arg, "no") ? DISPON_no
                : !strcmp(opts.arg, "sel") ? DISPON_sel
                : DISPON_yes;
            break;

        case FFCFG_display_scroll_rate:
            ff_cfg.display_scroll_rate = strtol(opts.arg, NULL, 10);
            if (ff_cfg.display_scroll_rate < 100)
                ff_cfg.display_scroll_rate = 100;
            cfg.ffcfg_has_display_scroll_rate = TRUE;
            break;

        case FFCFG_display_scroll_pause:
            ff_cfg.display_scroll_pause = strtol(opts.arg, NULL, 10);
            break;

        case FFCFG_nav_scroll_rate:
            ff_cfg.nav_scroll_rate = strtol(opts.arg, NULL, 10);
            break;

        case FFCFG_nav_scroll_pause:
            ff_cfg.nav_scroll_pause = strtol(opts.arg, NULL, 10);
            break;

            /* HFE OVERRIDES */

        case FFCFG_hfe_step:
            ff_cfg.hfe_step = strtol(opts.arg, NULL, 10);
            break;

        case FFCFG_hfe_rpm:
            ff_cfg.hfe_rpm = strtol(opts.arg, NULL, 10);
            break;

            /* MISCELLANEOUS */

        case FFCFG_step_volume: {
            int volume = strtol(opts.arg, NULL, 10);
            if (volume <= 0) volume = 0;
            if (volume >= 20) volume = 20;
            ff_cfg.step_volume = volume;
            cfg.ffcfg_has_step_volume = TRUE;
            break;
        }

        case FFCFG_notify_volume: {
            char *p, *q;
            ff_cfg.notify_volume = 0;
            for (p = opts.arg; *p != '\0'; p = q) {
                for (q = p; *q && *q != ','; q++)
                    continue;
                if (*q == ',')
                    *q++ = '\0';
                if (!strcmp(p, "slotnr")) {
                    ff_cfg.notify_volume |= NOTIFY_slotnr;
                } else {
                    ff_cfg.notify_volume &= ~NOTIFY_volume_mask;
                    ff_cfg.notify_volume |= strtol(p, NULL, 10)
                        & NOTIFY_volume_mask;
                }
            }
            break;
        }

        case FFCFG_da_report_version:
            memset(ff_cfg.da_report_version, 0,
                   sizeof(ff_cfg.da_report_version));
            snprintf(ff_cfg.da_report_version,
                     sizeof(ff_cfg.da_report_version),
                     "%s", opts.arg);
            break;

        case FFCFG_extend_image:
            ff_cfg.extend_image = !strcmp(opts.arg, "yes");
            break;

        }
    }

    F_close(&fs->file);

    flash_ff_cfg_update(fs->buf);
}

void process_ff_cfg_opts(const struct ff_cfg *old)
{
    /* chgrst, motor-delay: Reset EXTI handlers. */
    if ((ff_cfg.motor_delay != old->motor_delay)
        || (ff_cfg.chgrst != old->chgrst)) {
        gpio_irq_disable_mask(motor_chgrst_exti_mask);
        motor_chgrst_exti_mask = 0;
    }

    /* rotary, chgrst, motor-delay: Inform the rotary-encoder subsystem. 
     * It is harmless to reset rotary EXTI handlers unconditionally. */
    set_rotary_exti();

    /* interface, pin02, pin34, chgrst, motor-delay: Inform the floppy
     * subsystem.  */
    if ((ff_cfg.interface != old->interface)
        || (ff_cfg.pin02 != old->pin02)
        || (ff_cfg.pin34 != old->pin34)
        || (ff_cfg.motor_delay != old->motor_delay)
        || (ff_cfg.chgrst != old->chgrst)) {
        floppy_set_fintf_mode();
        motor_chgrst_setup_exti();
    }

    /* max-cyl: Inform the floppy subsystem. */
    if (ff_cfg.max_cyl != old->max_cyl)
        floppy_set_max_cyl();

    /* ejected-on-startup: Set the ejected state appropriately. */
    if (ff_cfg.ejected_on_startup)
        cfg.ejected = TRUE;

    /* oled-font, display-type: Reinitialise the display subsystem. */
    if ((ff_cfg.oled_font != old->oled_font)
        || (ff_cfg.oled_contrast != old->oled_contrast)
        || (ff_cfg.display_type != old->display_type))
        system_reset(); /* hit it with a hammer */
}
