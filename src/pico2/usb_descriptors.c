/*
 * pico2/usb_descriptors.c
 *
 * TinyUSB descriptors for the RP2350 port's CDC-ACM console device.
 *
 * Built against the pico-sdk and TinyUSB only (no FlashFloppy headers).
 *
 * This is free and unencumbered software released into the public domain.
 * See the file COPYING for more details, or visit <http://unlicense.org>.
 */

#include <string.h>

#include "tusb.h"
#include "pico/unique_id.h"

#define USBD_VID 0x2e8a /* Raspberry Pi */
#define USBD_PID 0x0009 /* Pico SDK CDC (RP2350), as pico_stdio_usb uses */

enum { STR_LANGID = 0, STR_MANUFACTURER, STR_PRODUCT, STR_SERIAL };

enum { ITF_NUM_CDC = 0, ITF_NUM_CDC_DATA, ITF_NUM_TOTAL };

#define EP_CDC_NOTIF  0x81
#define EP_CDC_OUT    0x02
#define EP_CDC_IN     0x82

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN)

static const tusb_desc_device_t desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    /* Interface Association Descriptor (IAD) for CDC */
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = USBD_VID,
    .idProduct          = USBD_PID,
    .bcdDevice          = 0x0100,
    .iManufacturer      = STR_MANUFACTURER,
    .iProduct           = STR_PRODUCT,
    .iSerialNumber      = STR_SERIAL,
    .bNumConfigurations = 1
};

static const uint8_t desc_configuration[] = {
    /* Bus powered, 100mA. */
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x80, 100),
    /* CDC: notification EP (8 bytes), data EPs (64 bytes). */
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, 0, EP_CDC_NOTIF, 8,
                       EP_CDC_OUT, EP_CDC_IN, CFG_TUD_CDC_EP_BUFSIZE)
};

static const char * const desc_strings[] = {
    [STR_MANUFACTURER] = "PicoFloppy",
    [STR_PRODUCT]      = "PicoFloppy Console",
};

/* Serial number: the board's unique ID, so that each Pico 2 gets a stable
 * device node on the host (e.g. /dev/serial/by-id/...). Filled in by
 * usb_descriptors_init() from thread context. */
static char serial_str[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];

void usb_descriptors_init(void);
void usb_descriptors_init(void)
{
    pico_get_unique_board_id_string(serial_str, sizeof(serial_str));
}

uint8_t const *tud_descriptor_device_cb(void)
{
    return (uint8_t const *)&desc_device;
}

uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return desc_configuration;
}

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    static uint16_t desc_str[32 + 1];
    const char *s;
    unsigned int i, len;

    (void)langid;

    if (index == STR_LANGID) {
        desc_str[1] = 0x0409; /* English (US) */
        len = 1;
    } else {
        if (index == STR_SERIAL)
            s = serial_str;
        else if ((index < TU_ARRAY_SIZE(desc_strings))
                 && (desc_strings[index] != NULL))
            s = desc_strings[index];
        else
            return NULL;
        len = strlen(s);
        if (len > 32)
            len = 32;
        for (i = 0; i < len; i++)
            desc_str[1 + i] = (uint8_t)s[i];
    }

    /* First halfword: total length in bytes, and descriptor type. */
    desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * len + 2));
    return desc_str;
}
