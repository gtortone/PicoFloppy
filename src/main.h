/*
 * main.h
 *
 * State and functions shared by the main-loop modules: main.c (start-up
 * and the floppy main loop), nav.c (image navigation), ff_cfg_read.c
 * (FF.CFG), ui_display.c and ui_buttons.c (front panel), eject_menu.c and
 * main_menu.c (menus). Private to those files.
 *
 * Written & released by Keir Fraser <keir.xen@gmail.com>
 *
 * This is free and unencumbered software released into the public domain.
 * See the file COPYING for more details, or visit <http://unlicense.org>.
 */

/*
 * Storage (main.c)
 */

extern FATFS fatfs;

/* Scratch file/directory state, allocated from the arena while a volume
 * is mounted (floppy_arena_setup()). */
struct fs_scratch {
    FIL file;
    DIR dp;
    FILINFO fp;
    char buf[512];
};
extern struct fs_scratch *fs;

void assert_volume_connected(void);
void floppy_arena_setup(void);

/*
 * Navigation state (nav.c)
 */

struct native_dirent {
    uint32_t dir_sect;
    uint16_t dir_off;
    uint8_t attr;
    char name[0];
};

struct nav_cfg {
    uint16_t slot_nr, max_slot_nr;
    uint8_t slot_map[1000/8];
    struct short_slot autoboot;
    struct short_slot hxcsdfe;
    struct short_slot imgcfg;
    struct slot slot, clipboard;
    uint32_t cfg_cdir, cur_cdir;
    struct native_dirent **sorted;
    struct {
        uint32_t cdir;
        uint16_t slot;
    } stack[20];
    uint8_t depth;
    uint8_t dirty_slot_nr:1;
    uint8_t dirty_slot_name:1;
    uint8_t hxc_mode:1;
    uint8_t ejected:1;
    uint8_t ima_ej_flag:1; /* "\\EJ" flag in IMAGE_A.CFG? */
    /* FF.CFG values which override HXCSDFE.CFG. */
    uint8_t ffcfg_has_step_volume:1;
    uint8_t ffcfg_has_display_off_secs:1;
    uint8_t ffcfg_has_display_scroll_rate:1;
};
extern struct nav_cfg cfg;

/* If TRUE, reset to start of filename when selecting a new image.
 * If FALSE, try to maintain scroll offset when browsing through images. */
#define cfg_scroll_reset TRUE

/* cfg_update() slot modes. */
#define CFG_KEEP_SLOT_NR  0 /* Do not re-read slot number from config */
#define CFG_READ_SLOT_NR  1 /* Read slot number afresh from config */
#define CFG_WRITE_SLOT_NR 2 /* Write new slot number to config */

bool_t slot_valid(unsigned int i);
bool_t slot_type(const char *str);
void update_slot_by_name(void);
void cfg_init(void);
void cfg_update(uint8_t slot_mode);
void native_get_slot_map(bool_t sorted_only);
void ima_mark_ejected(bool_t ej);
bool_t choose_new_image(uint8_t init_b);

/*
 * FF.CFG (ff_cfg_read.c)
 */

void read_ff_cfg(void);
void process_ff_cfg_opts(const struct ff_cfg *old);

/*
 * Front-panel display (ui_display.c)
 */

extern uint32_t display_ticks;
extern uint8_t display_state;
enum { BACKLIGHT_OFF, BACKLIGHT_SWITCHING_ON, BACKLIGHT_ON };
enum { LED_NORMAL, LED_TRACK, LED_TRACK_QUIESCENT,
       LED_BUTTON_HELD, LED_BUTTON_RELEASED };

/* Column of the write-protect/status indicator on an LCD/OLED. */
#define wp_column ((lcd_columns > 16) ? 8 : 7)

/* Scroll long filename. */
struct lcd_scroll {
    uint16_t off, end, pause, rate;
    int32_t ticks;
};
extern struct lcd_scroll lcd_scroll;

void lcd_on(void);
void lcd_scroll_init(uint16_t pause, uint16_t rate);
void lcd_scroll_name(void);
void display_write_slot(bool_t nav_mode);
void lcd_write_track_info(bool_t force);
void led_7seg_update_track(bool_t force);
uint8_t lcd_handle_backlight(uint8_t b);
uint8_t led_handle_display(uint8_t b);

/*
 * Front-panel buttons (ui_buttons.c)
 */

#define BUTTON_SCAN_HZ 500
#define BUTTON_SCAN_MS (1000/BUTTON_SCAN_HZ)

extern struct timer button_timer;
extern volatile uint8_t buttons, velocity;
extern uint8_t rotary;

void set_rotary_exti(void);
void button_timer_fn(void *unused);

/*
 * Menus (eject_menu.c, main_menu.c)
 */

void volume_space(void);
uint8_t wait_twobutton_press(uint8_t b);
uint8_t display_error(FRESULT fres, uint8_t b);
uint8_t eject_menu(uint8_t b);
void folder_menu(void);
void factory_reset(void);
void main_menu(void);
