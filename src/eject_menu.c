/*
 * eject_menu.c
 *
 * Eject menu and folder menu: image copy/paste/delete, volume
 * space, error display.
 *
 * Split out of main.c. Written & released by Keir Fraser
 * <keir.xen@gmail.com>.
 *
 * This is free and unencumbered software released into the public domain.
 * See the file COPYING for more details, or visit <http://unlicense.org>.
 */

#include "main.h"

void noinline volume_space(void)
{
    char msg[25];
    unsigned int free = (fatfs.free_clst*fatfs.csize+1953/2)/1953 + 100/2;
    unsigned int total = (fatfs.n_fatent*fatfs.csize+1953/2)/1953 + 100/2;
    if (fatfs.free_clst < fatfs.n_fatent-2) {
        snprintf(msg, sizeof(msg), "Free:%u.%u/%u.%uG",
                 free/1000, (free%1000)/100,
                 total/1000, (total%1000)/100);
    } else {
        snprintf(msg, sizeof(msg), "Volume: %u.%uG",
                 total/1000, (total%1000)/100);
    }
    lcd_write(0, 2, -1, msg);

}

/* Wait 50ms for 2-button press. */
uint8_t wait_twobutton_press(uint8_t b)
{
    unsigned int wait;

    for (wait = 0; wait < 50; wait++) {
        if ((buttons & (B_LEFT|B_RIGHT)) == (B_LEFT|B_RIGHT))
            b = B_SELECT;
        if (b & B_SELECT)
            break;
        delay_ms(1);
    }

    return b;
}

static uint8_t menu_wait_button(bool_t twobutton_eject, const char *led_msg)
{
    unsigned int wait = 0;
    uint8_t b;

    /* Wait for any button to be pressed. */
    while ((b = buttons) == 0) {
        /* Bail if USB disconnects. */
        assert_volume_connected();
        /* Update the display. */
        delay_ms(1);
        switch (display_type) {
        case DT_LED_7SEG:
            /* Alternate the 7-segment display. */
            if ((++wait % 1000) == 0) {
                switch (wait / 1000) {
                case 1:
                    led_7seg_write_decimal(cfg.slot_nr);
                    break;
                default:
                    led_7seg_write_string(led_msg);
                    wait = 0;
                    break;
                }
            }
            break;
        case DT_LCD_OLED:
            /* Continue to scroll long filename. */
            lcd_scroll.ticks -= time_ms(1);
            lcd_scroll_name();
            break;
        }
    }

    return twobutton_eject ? wait_twobutton_press(b) : b;
}

uint8_t noinline display_error(FRESULT fres, uint8_t b)
{
    bool_t twobutton_eject =
        (ff_cfg.twobutton_action & TWOBUTTON_mask) == TWOBUTTON_eject;
    char msg[17];

    display_mode = DM_menu;

    switch (display_type) {
    case DT_LED_7SEG:
        snprintf(msg, sizeof(msg), "%c%02u",
                 (fres >= 30) ? 'E' : 'F', fres);
        led_7seg_write_string(msg);
        break;
    case DT_LCD_OLED:
        snprintf(msg, sizeof(msg), "*%s*%02u*",
                 (fres >= 30) ? "ERR" : "FAT", fres);
        lcd_write(wp_column+1, 1, -1, "");
        lcd_write((lcd_columns > 16) ? 10 : 8, 1, 0, msg);
        lcd_on();
        break;
    }

    /* Wait for buttons to be released. */
    while (buttons != 0)
        continue;

    /* Wait for any button to be pressed. */
    b = menu_wait_button(twobutton_eject, msg);

    while (b & B_SELECT) {
        b = buttons;
        if (twobutton_eject && b) {
            /* Wait for 2-button release. */
            b = B_SELECT;
        }
    }

    display_mode = DM_normal;
    return b;
}

static bool_t confirm(const char *op)
{
    char msg[17];
    uint8_t b;

    snprintf(msg, sizeof(msg), "Confirm %s?", op);
    lcd_write(0, 1, -1, msg);

    while (buttons)
        continue;

    while ((b = menu_wait_button(TRUE, "")) == 0)
        continue;

    return (b == B_SELECT);
}

static void image_copy(void)
{
    cfg.clipboard = cfg.slot;
}

static void image_paste(const char *subfolder)
{
    time_t t;
    uint32_t cur_cdir = fatfs.cdir;
    int i, baselen, todo, idx, max_idx = -1;
    char *p, *q;
    FIL *nfil;
    bool_t use_basename = FALSE;
    const struct slot *slot = &cfg.clipboard;

    if (!slot->size || !confirm("Paste"))
        return;

    if (subfolder)
        F_chdir(subfolder);

    /* Is the source filename of the form '*_000'? */
    strcpy(fs->buf, slot->name);
    if ((p = q = strrchr(fs->buf, '_')) != NULL) {
        for (i = 0; i < 3; i++) {
            char c = *++p;
            if ((c < '0') || (c > '9'))
                break;
        }
        p = ((i == 3) && (p[1] == '\0')) ? q : NULL;
    }

    if (p == NULL) {
        /* Source filename is not of the form '*_000'. Does it exist at the 
         * destinaton? */
        FRESULT fres;
        p = fs->buf + strlen(fs->buf);
        snprintf(p, sizeof(fs->buf)-(p-fs->buf), ".%s", slot->type);
        fres = f_stat(fs->buf, &fs->fp);
        /* If it doesn't exist, we use the same exact name for the clone. */
        use_basename = (fres == FR_NO_FILE);
    }

    if (use_basename) {

        /* Clone the original source filename. */
        t = time_now();
        lcd_write(0, 1, -1, "Pasting...");

    } else {

        /* Search for next available three-digit clone identifier. */
        baselen = p - fs->buf;
        snprintf(p, sizeof(fs->buf) - baselen, "_*.%s", slot->type);

        for (F_findfirst(&fs->dp, &fs->fp, "", fs->buf);
             fs->fp.fname[0] != '\0';
             F_findnext(&fs->dp, &fs->fp)) {
            p = fs->fp.fname + baselen + 1;
            /* Parse 3-digit index number. */
            idx = 0;
            for (i = 0; i < 3; i++) {
                if ((*p < '0') || (*p > '9'))
                    break;
                idx *= 10;
                idx += *p++ - '0';
            }
            /* Expect a 3-digit number. */
            if ((i != 3) || (*p != '.'))
                continue;
            max_idx = max_t(int, max_idx, idx);
        }
        F_closedir(&fs->dp);

        /* Is there already a '*_999' file? */
        t = time_now();
        if (max_idx >= 999) {
            lcd_write(0, 1, -1, "No spare slots!");
            goto out;
        }

        snprintf(fs->buf, sizeof(fs->buf), "Pasting (%03u)...", max_idx+1);
        lcd_write(0, 1, -1, fs->buf);

        strcpy(fs->buf, slot->name);
        snprintf(fs->buf + baselen, sizeof(fs->buf) - baselen, "_%03u.%s",
                 max_idx+1, slot->type);

    }

    volume_cache_destroy();
    fatfs_from_slot(&fs->file, slot, FA_READ);
    nfil = arena_alloc(sizeof(*nfil));
    F_open(nfil, fs->buf, FA_CREATE_NEW|FA_WRITE);
    todo = f_size(&fs->file); 
    while (todo != 0) {
        int nr = min_t(int, todo, arena_avail());
        void *p = arena_alloc(0);
        F_read(&fs->file, p, nr, NULL);
        F_write(nfil, p, nr, NULL);
        todo -= nr;
    }
    F_close(nfil);
    fatfs.cdir = cfg.cur_cdir;
    floppy_arena_setup();
    if (!cfg.sorted)
        cfg_update(CFG_READ_SLOT_NR);

out:
    fatfs.cdir = cur_cdir;
    delay_from(t, time_ms(2000));
}

static bool_t image_delete(void)
{
    FRESULT fres;
    time_t t = time_now();
    bool_t ok = FALSE;

    if (!confirm("Delete"))
        return FALSE;

    lcd_write(0, 1, -1, "Deleting...");

    /* Kill the clipboard if we're deleting the copy source. */
    if (cfg.slot.firstCluster == cfg.clipboard.firstCluster)
        memset(&cfg.clipboard, 0, sizeof(cfg.clipboard));

    snprintf(fs->buf, sizeof(fs->buf), "%s.%s",
             cfg.slot.name, cfg.slot.type);
    fres = f_unlink(fs->buf);
    if (fres != FR_OK) {
        snprintf(fs->buf, sizeof(fs->buf), "Failed (%d)", fres);
        t = time_now();
    }

    cfg.slot_nr = min_t(uint16_t, cfg.slot_nr, cfg.max_slot_nr-1);
    cfg.sorted = NULL;
    cfg_update(CFG_READ_SLOT_NR);
    ok = TRUE;

    delay_from(t, time_ms(2000));
    return ok;
}

enum {
    EJM_header = 0,
    EJM_wrprot,
    EJM_copy,
    EJM_paste,
    EJM_delete,
    EJM_exit_to_selector,
    EJM_exit_reinsert,
    EJM_nr
};

static int construct_eject_menu(uint8_t *menu)
{
    int i, j;
    for (i = j = 0; i < EJM_nr; i++) {
        if ((i == EJM_paste) && !cfg.clipboard.size)
            continue;
        if ((i >= EJM_copy) && (i <= EJM_delete) && cfg.hxc_mode)
            continue;
        menu[j++] = i;
    }
    return j-1;
}

uint8_t noinline eject_menu(uint8_t b)
{
    const static char *menu_s[] = {
        [EJM_header] = "**Eject Menu**",
        [EJM_copy]   = "Copy",
        [EJM_paste]  = "Paste",
        [EJM_delete] = "Delete",
        [EJM_exit_to_selector] = "Exit to Selector",
        [EJM_exit_reinsert] = "Exit & Re-Insert",
    };

    char msg[17];
    uint8_t menu[EJM_nr];
    unsigned int wait;
    int sel = 0, sel_max;
    bool_t twobutton_eject =
        ((ff_cfg.twobutton_action & TWOBUTTON_mask) == TWOBUTTON_eject)
        || (display_type == DT_LCD_OLED); /* or two buttons can't exit menu */

    display_mode = DM_menu;

    ima_mark_ejected(TRUE);

    sel_max = construct_eject_menu(menu);

    for (;;) {

        switch (display_type) {
        case DT_LED_7SEG:
            led_7seg_write_string("EJE");
            break;
        case DT_LCD_OLED:
            switch (menu[sel]) {
            case EJM_wrprot:
                snprintf(msg, sizeof(msg), "Write Prot.: O%s",
                         (cfg.slot.attributes & AM_RDO) ? "N" : "FF");
                lcd_write(0, 1, -1, msg);
                break;
            default:
                lcd_write(0, 1, -1, menu_s[menu[sel]]);
                break;
            }
            lcd_on();
            break;
        }

        /* Wait for buttons to be released. */
        wait = 0;
        while (buttons != 0) {
            delay_ms(1);
            if ((display_type != DT_LCD_OLED) && (wait++ >= 2000)) {
            toggle_wp:
                wait = 0;
                cfg.slot.attributes ^= AM_RDO;
                if (volume_readonly()) {
                    /* Read-only filesystem: force AM_RDO always. */
                    cfg.slot.attributes |= AM_RDO;
                }
                if (display_type == DT_LED_7SEG)
                    led_7seg_write_string((cfg.slot.attributes & AM_RDO)
                                          ? "RDO" : "RIT");
            }
        }

        /* Wait for any button to be pressed. */
        b = menu_wait_button(twobutton_eject, "EJE");

        if (b & B_LEFT)
            sel--;
        if (b & B_RIGHT)
            sel++;
        if ((sel != 0) && (display_type != DT_LCD_OLED))
            goto out;

        if (sel < 0)
            sel = sel_max;
        if (sel > sel_max)
            sel = 0;

        if (b & B_SELECT) {
            /* Wait for eject button to be released. */
            wait = 0;
            while (b & B_SELECT) {
                b = buttons;
                if (twobutton_eject && b) {
                    /* Wait for 2-button release. */
                    b = B_SELECT;
                }
                delay_ms(1);
                if ((display_type != DT_LCD_OLED) && (wait++ >= 2000))
                    goto toggle_wp;
            }
            /* LED display: No menu, we exit straight out. */
            if (display_type != DT_LCD_OLED)
                goto out;
            switch (menu[sel]) {
            case EJM_wrprot: /* Toggle W.Protect */
                cfg.slot.attributes ^= AM_RDO;
                if (volume_readonly()) {
                    /* Read-only filesystem: force AM_RDO always. */
                    cfg.slot.attributes |= AM_RDO;
                }
                break;
            case EJM_copy:
                image_copy();
                sel_max = construct_eject_menu(menu);
                break;
            case EJM_paste:
                image_paste(NULL);
                break;
            case EJM_delete:
                if (!image_delete())
                    break;
                b = 0xff; /* selector */
                goto out;
            case EJM_exit_to_selector:
                display_write_slot(TRUE);
                b = 0xff; /* selector */
                goto out;
            case EJM_header:
            case EJM_exit_reinsert:
                b = 0;
                goto out;
            }
        }
 
    }

out:
    ima_mark_ejected(FALSE);
    display_mode = DM_normal;
    return b;
}

void folder_menu(void)
{
    if (display_type != DT_LCD_OLED)
        return;

    /* HACK: image_paste() clobbers fs->fp.fname so we make use of
     * cfg.slot.name instead. */
    snprintf(cfg.slot.name, sizeof(cfg.slot.name),
             "%s", fs->fp.fname);
    image_paste(cfg.slot.name);

    /* Now we must refresh the display */
    cfg_update(CFG_KEEP_SLOT_NR);
    display_write_slot(TRUE);
}
