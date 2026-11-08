/*
 * main_menu.c
 *
 * Main menu: MCU info, factory reset, firmware update (BOOTSEL), FF OSD
 * configuration.
 *
 * Split out of main.c. Written & released by Keir Fraser
 * <keir.xen@gmail.com>.
 *
 * This is free and unencumbered software released into the public domain.
 * See the file COPYING for more details, or visit <http://unlicense.org>.
 */

#include "main.h"

static bool_t main_menu_confirm(const char *op)
{
    char msg[17];
    uint8_t b;

    snprintf(msg, sizeof(msg), "Confirm %s?", op);
    lcd_write(0, 1, -1, msg);

    while ((b = buttons) == 0)
        continue;

    return wait_twobutton_press(b) == B_SELECT;
}

static void noinline mcu_info(void)
{
    const static char mcu[] = "RP2350";
    char msg[20];
    snprintf(msg, sizeof(msg), "%uMHz, %ukB", SYSCLK_MHZ, ram_kb);
    lcd_write(0, 0, -1, mcu);
    lcd_write(0, 1, -1, msg);
    while (!buttons)
        continue;
}

void factory_reset(void)
{
    /* Inform user that factory reset is about to occur. */
    switch (display_type) {
    case DT_LED_7SEG:
        led_7seg_write_string("RST");
        break;
    case DT_LCD_OLED:
        lcd_clear();
        display_mode = DM_banner; /* double height row 0 */
        lcd_write(0, 0, 0, "Reset Flash");
        lcd_write(0, 1, 0, "Configuration");
        lcd_on();
        break;
    }

    /* Wait for buttons to be released... */
    while (buttons)
        continue;

    /* ...and then do the Flash erase. */
    flash_ff_cfg_erase();

    /* Linger so user sees it is done. */
    delay_ms(2000);

    /* Reset so that changes take effect. */
    system_reset();
}

static void update_firmware(void)
{

    /* No FlashFloppy bootloader: the bootrom's UF2 loader takes its place. */
    rp2350_enter_bootsel();
}

static void ff_osd_configure(void)
{
    if (!has_osd || !main_menu_confirm("OSD Cnf"))
        return;

    lcd_write(0, 1, -1, "Exit: Power Off");

    for (;;) {
        time_t t = time_now();
        uint8_t b = buttons;
        if (b == (B_LEFT|B_RIGHT)) {
            osd_buttons_tx = B_SELECT;
            /* Wait for two-button release. */
            while ((time_diff(t, time_now()) < time_ms(1000)) && buttons)
                continue;
        } else if (b & (B_LEFT|B_RIGHT)) {
            /* Wait 50ms for a two-button press. */
            while ((b == buttons) && (time_diff(t, time_now()) < time_ms(50)))
                continue;
            osd_buttons_tx = (buttons == (B_LEFT|B_RIGHT)) ? B_SELECT : b;
        } else {
            osd_buttons_tx = b;
        }
        /* Hold button-held state for a while to make sure it gets 
         * transferred to the OSD. */
        if (osd_buttons_tx)
            delay_ms(100);
    }
}

void main_menu(void)
{
    const static char *menu[] = {
        "MCU Info",
        "Factory Reset",
        "Update Firmware",
        "Configure FF OSD",
        "Exit",
    };

    int sel = 0;
    uint8_t b;

    /* Not available to 7-segment display. */
    if (display_type != DT_LCD_OLED)
        return;

    display_mode = DM_menu;

    for (;;) {

        if (sel < 0)
            sel += ARRAY_SIZE(menu);
        if (sel >= ARRAY_SIZE(menu))
            sel -= ARRAY_SIZE(menu);

        lcd_write(0, 0, -1, "**Main Menu**");
        lcd_write(0, 1, -1, menu[sel]);
        lcd_on();

        /* Wait for buttons to be released. */
        while (buttons != 0)
            continue;

        /* Wait for any button to be pressed. */
        while ((b = buttons) == 0)
            continue;
        b = wait_twobutton_press(b);

        if (b & B_LEFT)
            sel--;
        if (b & B_RIGHT)
            sel++;

        if (b & B_SELECT) {
            /* Wait for buttons to be released. */
            while (buttons)
                continue;
            switch (sel) {
            case 0: /* MCU Info */
                mcu_info();
                break;
            case 1: /* Factory Reset */
                if (main_menu_confirm("Reset"))
                    factory_reset();
                break;
            case 2: /* Update Firmware */
                if (main_menu_confirm("Update"))
                    update_firmware();
                break;
            case 3: /* Configure FF OSD */
                ff_osd_configure();
                break;
            case 4: /* Exit */
                goto out;
            }
        }
 
    }

out:
    display_mode = DM_normal;
}
