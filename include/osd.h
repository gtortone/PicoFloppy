/*
 * osd.h
 *
 * FF OSD: on-screen display and Amiga keyboard controller, built into the
 * RP2350 firmware (pico2/osd*.c). It replaces the separate FF OSD board
 * and its I2C link: the second CPU core generates the display, and text
 * and button presses are exchanged through memory.
 *
 * This is free and unencumbered software released into the public domain.
 * See the file COPYING for more details, or visit <http://unlicense.org>.
 */

/* Text to show on screen, as published by the display driver. Row r is
 * double height iff bit r of @heights is set. */
struct osd_display {
    uint8_t on, rows, cols, heights;
    char text[4][40];
};

/* Core 0: start the OSD (second core) at boot. Sets has_osd. */
void osd_init(void);

/* Core 0: publish new display contents. Safe from interrupt context. */
void osd_publish(const struct osd_display *d);

/* Core 0: housekeeping, called periodically (saves the OSD configuration
 * to flash when the OSD menu asks for it). */
void osd_poll(void);
