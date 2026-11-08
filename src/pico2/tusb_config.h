/*
 * pico2/tusb_config.h
 *
 * TinyUSB configuration for the RP2350 port: a single CDC-ACM device
 * function (debug console, and the 1200-baud BOOTSEL gesture). See
 * pico2/usb_cdc.c.
 *
 * This is free and unencumbered software released into the public domain.
 * See the file COPYING for more details, or visit <http://unlicense.org>.
 */

#ifndef _PICO2_TUSB_CONFIG_H_
#define _PICO2_TUSB_CONFIG_H_

/* Device mode on the RP2350's only USB port. */
#define CFG_TUSB_RHPORT0_MODE   OPT_MODE_DEVICE
#define CFG_TUD_ENABLED         1

#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS             OPT_OS_PICO
#endif

#define CFG_TUD_ENDPOINT0_SIZE  64

/* Device classes: CDC only. */
#define CFG_TUD_CDC             1
#define CFG_TUD_MSC             0
#define CFG_TUD_HID             0
#define CFG_TUD_MIDI            0
#define CFG_TUD_VENDOR          0

/* CDC FIFOs (TinyUSB's own, behind our console ring) and endpoint size. */
#define CFG_TUD_CDC_RX_BUFSIZE  64
#define CFG_TUD_CDC_TX_BUFSIZE  256
#define CFG_TUD_CDC_EP_BUFSIZE  64

#endif /* _PICO2_TUSB_CONFIG_H_ */
