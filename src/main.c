/*
 * main.c
 *
 * System initialisation and the floppy main loop. Navigation, FF.CFG,
 * front panel and menus are in nav.c, ff_cfg_read.c, ui_display.c,
 * ui_buttons.c, eject_menu.c and main_menu.c (shared state: main.h).
 *
 * Written & released by Keir Fraser <keir.xen@gmail.com>
 *
 * This is free and unencumbered software released into the public domain.
 * See the file COPYING for more details, or visit <http://unlicense.org>.
 */

#include "main.h"

FATFS fatfs;
struct fs_scratch *fs;

#if LEVEL == LEVEL_logfile
/* Logfile must be written to config dir. */
#define logfile_flush(_file) do {               \
    fatfs.cdir = cfg.cfg_cdir;                  \
    logfile_flush(_file);                       \
    fatfs.cdir = cfg.cur_cdir;                  \
} while(0)
#endif

static void canary_init(void)
{
    _irq_stackbottom[0] = _thread_stackbottom[0] = 0xdeadbeef;
}

static void canary_check(void)
{
    ASSERT(_irq_stackbottom[0] == 0xdeadbeef);
    ASSERT(_thread_stackbottom[0] == 0xdeadbeef);
}

void assert_volume_connected(void)
{
    if (!volume_connected())
        F_die(FR_DISK_ERR);
}

static int run_floppy(void *_b)
{
    volatile uint8_t *pb = _b;
    time_t t_now, t_prev, t_diff;
    int32_t update_ticks;

    floppy_insert(0, &cfg.slot);

    led_7seg_update_track(TRUE);

    update_ticks = time_ms(20);
    t_prev = time_now();
    while (((*pb = buttons) == 0) && !floppy_handle()) {
        t_now = time_now();
        t_diff = time_diff(t_prev, t_now);
        if ((update_ticks -= t_diff) <= 0) {
            led_7seg_update_track(FALSE);
            lcd_write_track_info(FALSE);
            update_ticks = time_ms(20);
        }
        if (display_type == DT_LCD_OLED) {
            lcd_scroll.ticks -= t_diff;
            lcd_scroll_name();
        }
        canary_check();
        assert_volume_connected();
        t_prev = t_now;
    }

    if (display_type == DT_LED_7SEG)
        display_state = LED_NORMAL;

    return 0;
}

void floppy_arena_setup(void)
{
    arena_init();

    fs = arena_alloc(sizeof(*fs));

    if (cfg.sorted) {
        native_get_slot_map(FALSE);
    } else {
        unsigned int cache_len = arena_avail();
        uint8_t *cache_start = arena_alloc(0);
        volume_cache_init(cache_start, cache_start + cache_len);
    }
}

static void floppy_arena_teardown(void)
{
    fs = NULL;
    volume_cache_destroy();
}

static int floppy_main(void *unused)
{
    FRESULT fres;
    uint8_t b;
    uint32_t i;

    /* If any buttons are pressed when USB drive is mounted then we start 
     * in ejected state. */
    cfg.ejected = (buttons != 0);

    cfg.sorted = NULL;
    floppy_arena_setup();

    lcd_clear();
    display_mode = DM_normal;

    cfg_init();
    cfg_update(CFG_READ_SLOT_NR);

    /* If we start on a folder, go directly into the image selector. */
    if (cfg.slot.attributes & AM_DIR) {
        volume_space();
        display_write_slot(FALSE);
        b = buttons;
        goto select;
    }

    for (;;) {

        volume_space();

        lcd_scroll.ticks = time_ms(ff_cfg.display_scroll_pause)
            ?: time_ms(ff_cfg.display_scroll_rate);
        lcd_scroll.off = lcd_scroll.end = 0;
        lcd_scroll_init(ff_cfg.display_scroll_pause,
                        ff_cfg.display_scroll_rate);

        /* Make sure slot index is on a valid slot. Find next valid slot if 
         * not (and update config). */
        i = cfg.slot_nr;
        if (!slot_valid(i)) {
            while (!slot_valid(i))
                if (i++ >= cfg.max_slot_nr)
                    i = 0;
            printk("Updated slot %u -> %u\n", cfg.slot_nr, i);
            cfg.slot_nr = i;
            cfg_update(CFG_WRITE_SLOT_NR);
        }

        if (cfg.slot.attributes & AM_DIR) {
            if (cfg.hxc_mode) {
                /* No directory support in HxC selector/indexed modes. */
                F_die(FR_BAD_IMAGE);
            }
            if (!strcmp(fs->fp.fname, "..")) {
                if (cfg.depth == 0)
                    F_die(FR_BAD_IMAGECFG);
                fatfs.cdir = cfg.cur_cdir = cfg.stack[--cfg.depth].cdir;
                cfg.slot_nr = cfg.stack[cfg.depth].slot;
            } else {
                if (cfg.depth == ARRAY_SIZE(cfg.stack))
                    F_die(FR_PATH_TOO_DEEP);
                cfg.stack[cfg.depth].slot = cfg.slot_nr;
                cfg.stack[cfg.depth++].cdir = cfg.cur_cdir;
                F_chdir(fs->fp.fname);
                cfg.cur_cdir = fatfs.cdir;
                cfg.slot_nr = 1;
            }
            cfg.sorted = NULL;
            cfg_update(CFG_READ_SLOT_NR);
            display_write_slot(FALSE);
            b = buttons;
            goto select;
        }

        display_write_slot(FALSE);
        if (display_type == DT_LCD_OLED)
            lcd_write_track_info(TRUE);

        printk("Current slot: %u/%u\n", cfg.slot_nr, cfg.max_slot_nr);
        printk("Name: '%s' Type: %s\n", cfg.slot.name, cfg.slot.type);
        printk("Attr: %02x Clus: %08x Size: %u\n",
               cfg.slot.attributes, cfg.slot.firstCluster, cfg.slot.size);

        logfile_flush(&fs->file);

        if (cfg.ejected) {
            cfg.ejected = FALSE;
            b = B_SELECT;
        } else {
            floppy_arena_teardown();
            speaker_notify_insert(cfg.slot_nr);
            fres = F_call_cancellable(run_floppy, &b);
            floppy_cancel();
            speaker_notify_eject();
            assert_volume_connected();
            if ((b != 0) && (display_type == DT_LCD_OLED)) {
                /* Immediate visual indication of button press. */
                lcd_write(wp_column, 1, 1, "-");
                lcd_on();
            }
            floppy_arena_setup();
            logfile_flush(&fs->file);
            volume_space();
        }

        if (cfg.dirty_slot_name) {
            cfg.dirty_slot_name = FALSE;
            update_slot_by_name();
        }

        if (cfg.dirty_slot_nr) {
            cfg.dirty_slot_nr = FALSE;
            if (!cfg.hxc_mode)
                cfg_update(CFG_KEEP_SLOT_NR); /* get correct fs->fp */
            cfg_update(CFG_WRITE_SLOT_NR);
        }

        if (fres) {
            b = display_error(fres, b);
            fres = FR_OK;
            if (b == 0)
                continue;
        }

        if (b & B_SELECT) {
            b = eject_menu(b);
            if (b == 0)
                continue;
            if (b == 0xff) {
                display_write_slot(FALSE);
                b = 0;
                goto select;
            }
        }

        /* No buttons pressed: we probably just exited D-A mode. */
        if (b == 0) {
            /* If using HXCSDFE.CFG then re-read as it may have changed. */
            if (cfg.hxc_mode && (ff_cfg.nav_mode != NAVMODE_indexed))
                cfg_update(CFG_READ_SLOT_NR);
            continue;
        }

    select:
        lcd_scroll.off = lcd_scroll.end = 0;
        do {
            unsigned int wait_ms;

            /* While buttons are pressed we poll them and update current image
             * accordingly. */
            cfg.ejected = FALSE;
            if (choose_new_image(b) || cfg.ejected)
                break;

            /* Wait a few seconds for further button presses before acting on 
             * the new image selection. */
            if (cfg_scroll_reset)
                lcd_scroll.off = lcd_scroll.end = 0;
            lcd_scroll_init(0, ff_cfg.nav_scroll_rate);
            lcd_scroll.ticks = time_ms(ff_cfg.nav_scroll_pause);
            wait_ms = (cfg.slot.attributes & AM_DIR) ?
                ff_cfg.autoselect_folder_secs : ff_cfg.autoselect_file_secs;
            wait_ms *= 1000;
            if (wait_ms && (display_type == DT_LCD_OLED)) {
                /* Allow time for full name to scroll through. */
                unsigned int scroll_ms = ff_cfg.nav_scroll_pause;
                scroll_ms += lcd_scroll.end * ff_cfg.nav_scroll_rate;
                wait_ms = max(wait_ms, scroll_ms);
            }
            for (i = 0; (wait_ms == 0) || (i < wait_ms); i++) {
                b = buttons;
                if (b != 0)
                    break;
                assert_volume_connected();
                delay_ms(1);
                lcd_scroll.ticks -= time_ms(1);
                lcd_scroll_name();
            }

            /* Wait for select button to be released. */
            for (i = 0; !cfg.ejected && ((b = buttons) & B_SELECT); i++) {
                delay_ms(1);
                if (i >= 1500) { /* 1.5 seconds */
                    if (cfg.slot.attributes & AM_DIR) {
                        /* Display Folder Menu. */
                        folder_menu();
                        /* We go back into the selector, and to do this we 
                         * need to wait for all buttons released. */
                        while (buttons)
                            continue;
                        goto select;
                    } else {
                        cfg.ejected = TRUE;
                    }
                }
            }

        } while (!cfg.ejected && (b != 0));

        /* Write the slot number resulting from the latest round of button 
         * presses back to the config file. */
        cfg_update(CFG_WRITE_SLOT_NR);
    }

    ASSERT(0);
    return 0;
}

static void noinline banner(void)
{
    char msg[2][25];

    switch (display_type) {

    case DT_LED_7SEG:
#define sep_ch "r"
        led_7seg_write_string(
#if LEVEL == LEVEL_logfile
            "LOG"
#else
            (led_7seg_nr_digits() == 3) ? "F"sep_ch"F" : "FF"
#endif
            );
#undef sep_ch
        break;

    case DT_LCD_OLED:
        lcd_clear();
        display_mode = DM_banner; /* double height row 0 */
        lcd_write(0, 0, 0, "PicoFloppy");
        snprintf(msg[0], sizeof(msg[0]), "%s%s", fw_ver,
#if LEVEL == LEVEL_logfile
                 " Log"
#else
                 ""
#endif
            );
        snprintf(msg[1], sizeof(msg[1]), "%9s %dkB", msg[0], ram_kb);
        lcd_write(0, 1, 0, msg[1]);
        lcd_on();
        break;

    }
}

static void check_buttons(void)
{
    unsigned int i;
    uint8_t b = buttons;

    /* Need both LEFT and RIGHT pressed, or SELECT alone. */
    if ((b != (B_LEFT|B_RIGHT)) && (b != B_SELECT))
        return;

    /* Buttons must be continuously pressed for three seconds. */
    for (i = 0; (i < 3000) && (buttons == b); i++)
        delay_ms(1);

    if (buttons == b)
        factory_reset();
    else
        main_menu();

    banner();
}

static void maybe_show_version(void)
{
    uint8_t b, nb;
    const char *p, *np;
    char msg[3];
    int len;

    /* LCD/OLED already displays version info in idle state. */
    if (display_type != DT_LED_7SEG)
        return;

    /* Check if right button is pressed and released. */
    if ((b = buttons) != B_RIGHT)
        return;
    while ((nb = buttons) == b)
        continue;
    if (nb)
        return;

    led_7seg_write_string("CPU");
    delay_ms(1000);

    led_7seg_write_decimal(SYSCLK_MHZ);
    delay_ms(1000);

    led_7seg_write_string("RAN");
    delay_ms(1000);

    led_7seg_write_decimal(ram_kb);
    delay_ms(1000);

    led_7seg_write_string("UER");
    delay_ms(1000);

    /* Iterate through the dotted sections of the version number. */
    for (p = fw_ver; p != NULL; p = np ? np+1 : NULL) {
        np = strchr(p, '.');
        memset(msg, ' ', sizeof(msg));
        len = min_t(int, np ? np - p : strnlen(p, sizeof(msg)), sizeof(msg));
        memcpy(&msg[sizeof(msg) - len], p, len);
        led_7seg_write_string(msg);
        delay_ms(1000);
    }

    banner();
}

static void handle_errors(FRESULT fres)
{
    char msg[17];

    if (volume_connected() && (fres != FR_OK)) {
        printk("**Error %u\n", fres);
        snprintf(msg, sizeof(msg),
                 (display_type == DT_LED_7SEG)
                 ? ((fres >= 30) ? "E%02u" : "F%02u")
                 : ((fres >= 30) ? "*ERROR* %02u" : "*FATFS* %02u"),
                 fres);
    } else {
        /* No error. Do nothing. */
        return;
    }

    switch (display_type) {
    case DT_LED_7SEG:
        led_7seg_write_string(msg);
        break;
    case DT_LCD_OLED:
        display_mode = DM_menu; /* double-height row 1 */
        lcd_write(0, 0, -1, "***************");
        lcd_write(0, 1, -1, msg);
        lcd_on();
        break;
    }

    /* Wait for buttons to be released, pressed and released again. */
    while (buttons)
        continue;
    while (!buttons && volume_connected())
        continue;
    while (buttons)
        continue;
}

int main(void)
{
    FRESULT fres;

    /* DATA and BSS are set up by the pico-sdk runtime (crt0). */
    canary_init();
    mcu_init();
    time_init();
    console_init();
    board_init();
    console_crash_on_input();
    delay_ms(200); /* 5v settle */

    printk("\n** PicoFloppy %s\n", fw_ver);
    printk("** Based on FlashFloppy by Keir Fraser <keir.xen@gmail.com>\n");
    printk("** github:keirf/flashfloppy\n\n");

    printk("Build: %s %s\n", build_date, build_time);
    printk("Board: Raspberry Pi Pico 2 (RP2350)\n");

    speaker_init();

    flash_ff_cfg_read();

    osd_init();

    floppy_init();

    display_init();

    while (floppy_ribbon_is_reversed()) {
        printk("** Error: Ribbon cable upside down?\n");
        switch (display_type) {
        case DT_LED_7SEG:
            led_7seg_write_string("RIB");
            break;
        case DT_LCD_OLED:
            lcd_write(0, 0, -1, "Ribbon Cable May");
            lcd_write(0, 1, -1, "Be Upside Down? ");
            lcd_on();
            break;
        }
    }

    rotary = board_get_rotary();
    set_rotary_exti();
    timer_init(&button_timer, button_timer_fn, NULL);
    timer_set(&button_timer, time_now());

    for (;;) {

        banner();

        arena_init();
        while (f_mount(&fatfs, "", 1) != FR_OK) {
            maybe_show_version();
            check_buttons();
        }

        fres = F_call_cancellable(floppy_main, NULL);
        osd_buttons_tx = 0;
        floppy_cancel();
        floppy_arena_teardown();

        handle_errors(fres);
    }

    return 0;
}
