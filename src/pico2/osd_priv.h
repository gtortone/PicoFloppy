/*
 * pico2/osd_priv.h
 *
 * Declarations shared by the FF OSD modules: osd.c (video engine and core 1
 * main loop), osd_amikbd.c (Amiga keyboard), osd_menu.c (configuration
 * menu), osd_font.c.
 *
 * Everything here except the configuration store runs on core 1, which
 * owns PIO1 and the OSD pins. Core 1 never calls FlashFloppy's time, timer,
 * console or IRQ-priority helpers: they belong to core 0 (time_now() is
 * built on core 0's SysTick). It uses the SDK's microsecond timer instead.
 *
 * Ported from FF OSD (github.com/keirf/flashfloppy-osd), written & released
 * by Keir Fraser <keir.xen@gmail.com>.
 *
 * This is free and unencumbered software released into the public domain.
 * See the file COPYING for more details, or visit <http://unlicense.org>.
 */

/* Pins. The video sync and the keyboard lines are 5V signals on an Amiga:
 * they must be on 5V-tolerant GPIOs, which excludes the ADC pins (26-29).
 * The video output only drives a ~0.7V analogue line, through a resistor. */
#define OSD_PIN_KBDAT   0
#define OSD_PIN_KBCLK   1  /* must be OSD_PIN_KBDAT+1 (osd_amikbd PIO) */
#define OSD_PIN_CSYNC  15
#define OSD_PIN_VIDEO  28

#define osd_pio pio1

/* Core 1 interrupts: sync measurement above keyboard handling. */
#define OSD_SYNC_IRQ   PIO1_IRQ_0
#define OSD_KBD_IRQ    PIO1_IRQ_1
#define OSD_SYNC_PRI   0x00
#define OSD_KBD_PRI    0x40

/* Microsecond time on core 1. */
#define osd_now() time_us_32()
#define osd_since(t) (osd_now() - (t))

/*
 * Configuration (osd.c), kept in its own flash sector.
 */
enum { SYNC_LOW = 0, SYNC_HIGH, SYNC_AUTO, SYNC_MAX };

struct osd_config {
    uint16_t polarity;   /* SYNC_* */
    uint16_t h_off;      /* 1-199, units of 40 clk_sys cycles (0.28us) */
    uint16_t v_off;      /* 2-299, sync pulses from start of frame */
    uint16_t display_2Y; /* double height (each line output twice) */
    uint16_t crc16_ccitt;
};

extern struct osd_config osd_config;
extern const struct osd_config osd_dfl_config;

/* Ask core 0 to write @conf to flash (osd_poll()). */
void osd_config_save(const struct osd_config *conf);

/* Sync polarity in use, and whether auto-detection changed it lately. */
extern volatile uint8_t osd_running_polarity;
void osd_set_polarity(unsigned int polarity);

/* Show a short message for two seconds. Rows are NUL-separated in @s,
 * which ends with an empty string (double NUL). */
void osd_notify(const char *s);

/*
 * Amiga keyboard (osd_amikbd.c).
 */
#define AMI_RETURN 0x44
#define AMI_F(x)   (0x4f+(x))
#define AMI_DEL    0x46
#define AMI_HELP   0x5f
#define AMI_L_CTRL 0x63
#define AMI_L_ALT  0x64
#define AMI_LEFT   0x4f
#define AMI_RIGHT  0x4e
#define AMI_UP     0x4c
#define AMI_W      0x11
#define AMI_A      0x20
#define AMI_S      0x21
#define AMI_D      0x22

/* Keyboard held: hotkeys work without L.Ctrl + L.Alt, and every key press
 * is hidden from the Amiga. */
extern volatile bool_t amikbd_held;

void amikbd_hw_init(void); /* core 0: pins, PIO program and SM */
void amikbd_start(void);   /* core 1: interrupt */

/* Has @keycode been pressed since the last call (or is it still held)?
 * Only while L.Ctrl + L.Alt are held, or the keyboard is held. */
bool_t amikbd_key_pressed(uint8_t keycode);

/*
 * Configuration menu (osd_menu.c).
 */
extern bool_t osd_menu_active;
extern struct osd_display osd_menu_display;

/* @b: B_LEFT/B_RIGHT/B_SELECT, sampled every 5ms. */
void osd_menu_process(uint8_t b, bool_t autosync_changed);

/*
 * Font (osd_font.c): 8x8 pixels, characters 0x20-0x7f, 8 bytes each.
 */
extern const uint8_t osd_font[];
