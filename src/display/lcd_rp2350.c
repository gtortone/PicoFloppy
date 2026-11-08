/*
 * lcd_rp2350.c
 *
 * 1. HD44780 LCD controller via a PCF8574 I2C backpack.
 * 2. SSD1306/SH1106 OLED controllers (128x32 or 128x64), through the u8g2
 *    graphics library (lib/u8g2).
 *
 * RP2350 back end: I2C0 on GPIO20 (SDA) / GPIO21 (SCL), with DMA-fed
 * transmit, via the pico-sdk. The SDK has no asynchronous I2C API, so the
 * transfer engine drives the DesignWare controller through the SDK's
 * register structure (i2c_hw_t).
 *
 * The FF OSD is built in (pico2/osd.c), and needs no I2C: the text is
 * published to it straight from text[], alongside any LCD or OLED. With no
 * I2C display at all, the OSD alone is the display (lcd_init_osd_only()).
 *
 * The display is refreshed continuously, in the background, from the I2C
 * interrupt: each completed frame queues the next one. A frame is a DMA
 * stream of DATA_CMD words, which may hold several I2C transfers (each
 * ending with STOP): one per row for the LCD, a whole screen for the OLED.
 *
 * Adapted from lcd_at32f435.c, written & released by Keir Fraser.
 *
 * This is free and unencumbered software released into the public domain.
 * See the file COPYING for more details, or visit <http://unlicense.org>.
 */

/* PCF8574 pin assignment: D7-D6-D5-D4-BL-EN-RW-RS */
#define _D7 (1u<<7)
#define _D6 (1u<<6)
#define _D5 (1u<<5)
#define _D4 (1u<<4)
#define _BL (1u<<3)
#define _EN (1u<<2)
#define _RW (1u<<1)
#define _RS (1u<<0)

/* HD44780 commands */
#define CMD_ENTRYMODE    0x04
#define CMD_DISPLAYCTL   0x08
#define CMD_DISPLAYSHIFT 0x10
#define CMD_FUNCTIONSET  0x20
#define CMD_SETCGRADDR   0x40
#define CMD_SETDDRADDR   0x80
#define FS_2LINE         0x08

/* u8g2 (lib/u8g2): written for a laxer warning set than FlashFloppy's, and
 * spells out attributes that FlashFloppy defines as macros (intrinsics.h). */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstrict-prototypes"
#pragma GCC diagnostic ignored "-Wredundant-decls"
#pragma GCC diagnostic ignored "-Wundef"
#pragma push_macro("noinline")
#pragma push_macro("packed")
#pragma push_macro("aligned")
#undef noinline
#undef packed
#undef aligned
#include "u8g2.h"
#pragma pop_macro("aligned")
#pragma pop_macro("packed")
#pragma pop_macro("noinline")
#pragma GCC diagnostic pop

/* RP2350 I2C0: SDK instance, and its register block. */
#define i2c_inst i2c0
#define i2c i2c0_hw

#define SDA 20
#define SCL 21

/* I2C event IRQ (STOP detect / abort). */
#define I2C_EVENT_IRQ RP_I2C0_IRQ
DEFINE_IRQ(I2C_EVENT_IRQ, "IRQ_i2c_event");

/* I2C error handling runs from a software interrupt. */
#define I2C_ERROR_IRQ RP_SPARE_IRQ_3
DEFINE_IRQ(I2C_ERROR_IRQ, "IRQ_i2c_error");

/* DMA Tx channel, claimed from the SDK on first use. */
static int dma_tx_ch = -1;

bool_t has_osd; /* set by osd_init() */
uint8_t osd_buttons_tx;
uint8_t osd_buttons_rx;

/* FF OSD: the text is published every 20ms. */
#define OSD_PERIOD time_ms(20)
static struct timer osd_timer;

static uint8_t _bl;
static uint8_t i2c_addr;
static uint8_t i2c_dead;
static uint8_t i2c_row;
static bool_t is_oled_display;
static uint8_t oled_height;

#define OLED_ADDR 0x3c
enum { OLED_unknown, OLED_ssd1306, OLED_sh1106 };
static uint8_t oled_model;
static void oled_init(void);
static unsigned int oled_prep_frame(void);

static void i2c_start(uint8_t a);

static void i2c_tx_tc(void);

/* Count of display-refresh completions. For synchronisation/flush. */
static volatile uint8_t refresh_count;

/* I2C data buffer (LCD rows; OLED synchronous transfers), and the 16-bit
 * DATA_CMD stream that is fed by DMA to the DW I2C controller. Sized for a
 * whole 128x64 OLED frame as u8g2 sends it: per page, three addressing
 * commands of two bytes each, and 128 data bytes in 24-byte transfers
 * each with a control byte (8 x 140 = 1120 words). */
static uint8_t buffer[256] aligned(4);
static uint16_t dma_buffer[1536] aligned(4);

/* Text buffer, rendered into I2C data and placed into buffer[]. */
static char text[4][40];

/* OLED: text[] has changed since the frame buffer was last rendered. */
static volatile bool_t oled_dirty;

/* Columns and rows of text. */
uint8_t lcd_columns, lcd_rows;

/* Current display mode: Affects row ordering and sizing. */
uint8_t display_mode = DM_banner;
#define menu_mode (display_mode == DM_menu)

/* Occasionally the I2C/DMA engine seems to get stuck. Detect this with
 * a timeout timer and unwedge it by calling the I2C error handler. */
#define DMA_TIMEOUT time_ms(500)
static struct timer timeout_timer;

/* Error diagnosis: what failed, and the controller's abort source, which
 * must be captured before reading IC_CLR_TX_ABRT clears it. */
static const char *i2c_err_what;
static uint32_t i2c_abrt_source;

/* DMA frame in flight: its length in words (0 = none). */
static unsigned int dma_len;

/* Consecutive errors, reset by a completed frame. After a few, back off:
 * a display that keeps failing must not tie up the CPU. */
#define I2C_MAX_RETRIES 3
#define I2C_RETRY_DELAY time_ms(1000)
static uint8_t i2c_errors;
static bool_t i2c_retry_due;
static struct timer retry_timer;

static void timeout_fn(void *unused)
{
    i2c_err_what = "timeout";
    i2c_abrt_source = 0;
    IRQx_set_pending(I2C_ERROR_IRQ);
}

static void retry_fn(void *unused)
{
    i2c_retry_due = TRUE;
    IRQx_set_pending(I2C_ERROR_IRQ);
}

#define I2C_ABORTED I2C_IC_RAW_INTR_STAT_TX_ABRT_BITS

/* Record (and clear) a transmit abort. */
static void i2c_note_abort(const char *what)
{
    i2c_abrt_source = i2c->tx_abrt_source;
    (void)i2c->clr_tx_abrt;
    i2c_err_what = what;
}

/* Print the cause of an error, with the abort reasons by name. */
static void i2c_report_error(void)
{
    static const char * const abrt_names[] = {
        "7B_ADDR_NOACK", "10ADDR1_NOACK", "10ADDR2_NOACK", "TXDATA_NOACK",
        "GCALL_NOACK", "GCALL_READ", "HS_ACKDET", "SBYTE_ACKDET",
        "HS_NORSTRT", "SBYTE_NORSTRT", "10B_RD_NORSTRT", "MASTER_DIS",
        "ARB_LOST", "SLVFLUSH_TXFIFO", "SLV_ARBLOST", "SLVRD_INTX",
        "USER_ABRT"
    };
    uint32_t src = i2c_abrt_source;
    unsigned int i;

    printk("I2C: Error: %s", i2c_err_what ?: "unknown");
    for (i = 0; i < ARRAY_SIZE(abrt_names); i++)
        if (src & (1u << i))
            printk(" %s", abrt_names[i]);
    if (src >> I2C_IC_TX_ABRT_SOURCE_TX_FLUSH_CNT_LSB)
        printk(" (%u flushed)",
               src >> I2C_IC_TX_ABRT_SOURCE_TX_FLUSH_CNT_LSB);
    if (dma_len && (dma_tx_ch >= 0))
        printk(", frame %u/%u words", dma_len
               - dma_channel_hw_addr(dma_tx_ch)->transfer_count, dma_len);
    printk("\n");

    i2c_err_what = NULL;
    i2c_abrt_source = 0;
}

static void i2c_disable(void)
{
    stk_time_t t = stk_now();
    i2c->enable = 0;
    while ((i2c->enable_status & I2C_IC_ENABLE_STATUS_IC_EN_BITS)
           && (stk_diff(t, stk_now()) < stk_ms(5)))
        cpu_relax();
}

/* I2C Error ISR: Reset the peripheral and reinit everything. */
static void IRQ_i2c_error(void)
{
    /* Scheduled retry after a back-off: the cleanup is already done. */
    if (i2c_retry_due) {
        i2c_retry_due = FALSE;
        lcd_init();
        return;
    }

    /* Dump and clear I2C errors. */
    if (i2c->raw_intr_stat & I2C_ABORTED)
        i2c_note_abort(i2c_err_what ?: "abort");
    i2c_report_error();
    (void)i2c->clr_intr;

    /* Clear the DMA channel. */
    if (dma_tx_ch >= 0) {
        dma_channel_abort(dma_tx_ch);
        hw_clear_bits(&dma_channel_hw_addr(dma_tx_ch)->al1_ctrl,
                      DMA_CH0_CTRL_TRIG_EN_BITS);
    }

    /* Clear the I2C peripheral. */
    i2c->intr_mask = 0;
    i2c_disable();

    timer_cancel(&timeout_timer);
    dma_len = 0;

    if (++i2c_errors > I2C_MAX_RETRIES) {
        printk("I2C: Retrying in %ums\n", I2C_RETRY_DELAY / time_ms(1));
        timer_set(&retry_timer, time_now() + I2C_RETRY_DELAY);
        return;
    }

    lcd_init();
}

static void IRQ_i2c_event(void)
{
    const uint32_t expected = I2C_IC_INTR_MASK_M_STOP_DET_BITS
        | I2C_IC_INTR_MASK_M_TX_ABRT_BITS;
    uint32_t stat = i2c->intr_stat;

    /* Only STOP_DET and TX_ABRT are ever enabled. Mask anything else rather
     * than return with it still asserted, which would re-enter this
     * handler forever and starve every lower-priority interrupt. */
    if (stat & ~expected)
        i2c->intr_mask &= expected;

    if (stat & I2C_IC_INTR_STAT_R_TX_ABRT_BITS) {
        i2c_note_abort("abort during frame");
        IRQx_set_pending(I2C_ERROR_IRQ);
        return;
    }

    if (stat & I2C_IC_INTR_STAT_R_STOP_DET_BITS) {
        (void)i2c->clr_stop_det;
        /* A frame may hold several transfers: it is complete only once DMA
         * has fed the whole stream and the controller has drained it. */
        if ((dma_tx_ch >= 0) && !dma_channel_is_busy(dma_tx_ch)
            && (i2c->status & I2C_IC_STATUS_TFE_BITS)
            && !(i2c->status & I2C_IC_STATUS_MST_ACTIVITY_BITS)) {
            dma_len = 0;
            i2c_errors = 0;
            i2c_tx_tc();
        }
    }
}

/* Start sending the first @nr words of dma_buffer[] by DMA. */
static void dma_kick(unsigned int nr)
{
    dma_channel_config c;

    ASSERT((nr != 0) && (nr <= ARRAY_SIZE(dma_buffer)));

    i2c_start(i2c_addr);

    i2c->dma_tdlr = 8;
    i2c->dma_cr = I2C_IC_DMA_CR_TDMAE_BITS;
    i2c->intr_mask = I2C_IC_INTR_MASK_M_STOP_DET_BITS
        | I2C_IC_INTR_MASK_M_TX_ABRT_BITS;

    if (dma_tx_ch < 0)
        dma_tx_ch = dma_claim_unused_channel(true);
    c = dma_channel_get_default_config(dma_tx_ch);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_16);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, i2c_get_dreq(i2c_inst, true));
    dma_len = nr;
    dma_channel_configure(dma_tx_ch, &c, &i2c->data_cmd, dma_buffer, nr,
                          true);

    /* Set the timeout timer in case the DMA hangs for any reason. */
    timer_set(&timeout_timer, time_now() + DMA_TIMEOUT);
}

/* Send the first @sz bytes of buffer[] as a single I2C transfer. */
static void dma_start(unsigned int sz)
{
    unsigned int i;

    ASSERT((sz != 0) && (sz <= ARRAY_SIZE(buffer)));

    /* Expand into 16-bit DATA_CMD values; STOP rides the final byte. */
    for (i = 0; i < sz; i++)
        dma_buffer[i] = buffer[i];
    dma_buffer[sz-1] |= I2C_IC_DATA_CMD_STOP_BITS;

    dma_kick(sz);
}

/* Emit a 4-bit command to the HD44780 via the DMA buffer. */
static void emit4(uint8_t **p, uint8_t val)
{
    *(*p)++ = val;
    *(*p)++ = val | _EN;
    *(*p)++ = val;
}

/* Emit an 8-bit command to the HD44780 via the DMA buffer. */
static void emit8(uint8_t **p, uint8_t val, uint8_t signals)
{
    signals |= _bl;
    emit4(p, (val & 0xf0) | signals);
    emit4(p, (val << 4) | signals);
}

/* Snapshot text buffer into the command buffer. */
static unsigned int lcd_prep_buffer(void)
{
    const static uint8_t row_offs[] = { 0x00, 0x40, 0x14, 0x54 };
    uint16_t order;
    char *p;
    uint8_t *q = buffer;
    unsigned int i, row;

    if (i2c_row >= lcd_rows) {
        i2c_row = 0;
        refresh_count++;
    }

    order = (lcd_rows == 2) ? 0x7710 : 0x2103;
    if ((ff_cfg.display_order != DORD_default) && (display_mode == DM_normal))
        order = ff_cfg.display_order;

    row = (order >> (i2c_row * DORD_shift)) & DORD_row;
    p = (_bl && row < ARRAY_SIZE(text)) ? text[row] : NULL;

    emit8(&q, CMD_SETDDRADDR | row_offs[i2c_row], 0);
    for (i = 0; i < lcd_columns; i++)
        emit8(&q, p ? *p++ : ' ', _RS);

    i2c_row++;

    return q - buffer;
}

static void i2c_tx_tc(void)
{
    /* Prepare the next frame and start sending it. */
    if (is_oled_display)
        dma_kick(oled_prep_frame());
    else
        dma_start(lcd_prep_buffer());
}

/* Prepare the controller for a DMA frame to address @a. */
static void i2c_start(uint8_t a)
{
    i2c_disable();
    /* Discard events latched by earlier transfers (a NACKed transfer ends
     * with both TX_ABRT and STOP_DET). */
    (void)i2c->clr_intr;
    i2c->tar = a;
    i2c->enable = 1;
}

/*
 * Synchronous (polled) transfers, used to probe the bus and to initialise
 * the displays: the pico-sdk's blocking I2C calls, bounded by a timeout.
 */

#define I2C_SYNC_TIMEOUT_US 10000

static bool_t i2c_sync_result(int rc, unsigned int nr)
{
    if (rc == (int)nr)
        return TRUE;
    if (rc == PICO_ERROR_TIMEOUT) {
        /* I2C bus seems to be locked up. */
        i2c_dead = TRUE;
        i2c_err_what = "timeout (polled)";
    } else {
        i2c_err_what = "NACK (polled)";
    }
    return FALSE;
}

static bool_t i2c_sync_write_txn(uint8_t addr, uint8_t *cmds, unsigned int nr)
{
    return i2c_sync_result(
        i2c_write_timeout_us(i2c_inst, addr, cmds, nr, false,
                             I2C_SYNC_TIMEOUT_US), nr);
}

static bool_t i2c_sync_read_txn(uint8_t addr, uint8_t *rsp, unsigned int nr)
{
    return i2c_sync_result(
        i2c_read_timeout_us(i2c_inst, addr, rsp, nr, false,
                            I2C_SYNC_TIMEOUT_US), nr);
}

/* Write a 4-bit nibble over D7-D4 (4-bit bus), strobing EN. */
static void write4(uint8_t val)
{
    uint8_t b[3] = { val, val | _EN, val };
    (void)i2c_sync_write_txn(i2c_addr, b, sizeof(b));
}

/* Check whether an I2C device is responding at given address. */
static bool_t i2c_probe(uint8_t a)
{
    uint8_t zero = 0;
    bool_t ok = i2c_sync_write_txn(a, &zero, 1);
    i2c_err_what = NULL; /* absence is not an error */
    return ok;
}

#if LEVEL == LEVEL_debug
/* List every device that answers on the bus (debug builds). */
static void i2c_scan(void)
{
    uint8_t a;

    printk("I2C: Devices at:");
    for (a = 0x08; (a < 0x78) && !i2c_dead; a++)
        if (i2c_probe(a))
            printk(" %02x", a);
    printk("%s\n", i2c_dead ? " (bus locked up?)" : "");
}
#endif

/* Check given inclusive range of addresses for a responding I2C device. */
static uint8_t i2c_probe_range(uint8_t s, uint8_t e)
{
    uint8_t a;
    for (a = s; (a <= e) && !i2c_dead; a++)
        if (i2c_probe(a))
            return a;
    return 0;
}

/* Publish text[] to the OSD, ordered and sized as FlashFloppy does for the
 * FF OSD I2C protocol. Also, with no I2C display, this is the refresh. */
static void osd_timer_fn(void *unused)
{
    struct osd_display d;
    uint16_t order = menu_mode ? 0x7903 : 0x7183;
    unsigned int row, r, rows, heights;
    int i;

    if ((ff_cfg.osd_display_order != DORD_default)
        && (display_mode == DM_normal))
        order = ff_cfg.osd_display_order;

    heights = rows = 0;
    for (i = 3; i >= 0; i--) {
        /* Iterate over rows, bottom to top. */
        row = order >> (i * DORD_shift);
        /* Skip all trailing empty rows. */
        if ((rows == 0) && ((row & DORD_row) == 7))
            continue;
        /* Count this row and check if it is double height. */
        rows++;
        heights <<= 1;
        if (row & DORD_double)
            heights |= 1;
    }

    memset(&d, ' ', sizeof(d));
    d.on = !!_bl;
    d.rows = rows;
    d.cols = lcd_columns;
    d.heights = heights;
    for (row = 0; row < rows; row++) {
        r = (order >> (row * DORD_shift)) & DORD_row;
        if (r < ARRAY_SIZE(text))
            memcpy(d.text[row], text[r], lcd_columns);
    }
    osd_publish(&d);

    if (i2c_addr == 0)
        refresh_count++;

    osd_poll();

    timer_set(&osd_timer, time_now() + OSD_PERIOD);
}

static void osd_timer_start(void)
{
    static bool_t started;
    if (!has_osd || started)
        return;
    started = TRUE;
    timer_init(&osd_timer, osd_timer_fn, NULL);
    timer_set(&osd_timer, time_now());
}

void lcd_clear(void)
{
    memset(text, ' ', sizeof(text));
    oled_dirty = TRUE;
}

void lcd_write(int col, int row, int min, const char *str)
{
    char c, *p;
    uint32_t oldpri;

    if (min < 0)
        min = lcd_columns;

    p = &text[row][col];

    /* Prevent the text[] getting rendered while we're updating it. */
    oldpri = IRQ_save(I2C_IRQ_PRI);

    while ((c = *str++) && (col++ < lcd_columns)) {
        *p++ = c;
        min--;
    }
    while ((min-- > 0) && (col++ < lcd_columns))
        *p++ = ' ';

    oled_dirty = TRUE;

    IRQ_restore(oldpri);
}

void lcd_backlight(bool_t on)
{
    /* Will be picked up the next time text[] is rendered. */
    _bl = on ? _BL : 0;
}

void lcd_sync(void)
{
    uint8_t c = refresh_count;
    while ((uint8_t)(refresh_count - c) < 2)
        cpu_relax();
}

/* (Re)initialise the I2C controller in master mode at the given bus speed
 * (100kHz or 400kHz). i2c_init() resets the block and derives the SCL and
 * SDA-hold timings from clk_sys. Interrupts are left masked. */
static void i2c_configure(bool_t fast)
{
    /* i2c_init() resets the controller, after which its interrupt mask has
     * its default value, with TX_EMPTY (among others) enabled; and an
     * empty TX FIFO asserts it at once. Keep the interrupt off in the NVIC
     * until the mask is cleared. */
    bool_t irq_on = IRQx_is_enabled(I2C_EVENT_IRQ);
    IRQx_disable(I2C_EVENT_IRQ);
    i2c_init(i2c_inst, fast ? 400*1000 : 100*1000);
    i2c->intr_mask = 0;
    (void)i2c->clr_intr;
    IRQx_clear_pending(I2C_EVENT_IRQ);
    if (irq_on)
        IRQx_enable(I2C_EVENT_IRQ);
}

bool_t lcd_init(void)
{
    uint8_t a, *p;
    bool_t reinit = (i2c_addr != 0);

    i2c_dead = FALSE;
    i2c_row = 0;
    osd_buttons_rx = 0;

    /* Check we have a clear I2C bus. Both clock and data must be high. If SDA
     * is stuck low then slave may be stuck in an ACK cycle. We can try to
     * unwedge the slave in that case and drive it into the STOP condition. */
    gpio_configure_pin(gpiob, SCL, GPO_opendrain(_2MHz, HIGH));
    gpio_configure_pin(gpiob, SDA, GPO_opendrain(_2MHz, HIGH));
    delay_us(10);
    if (gpio_read_pin(gpiob, SCL) && !gpio_read_pin(gpiob, SDA)) {
        printk("I2C: SDA held by slave? Fixing... ");
        /* We will hold SDA low (as slave is) and also drive SCL low to end
         * the current ACK cycle. */
        gpio_write_pin(gpiob, SDA, FALSE);
        gpio_write_pin(gpiob, SCL, FALSE);
        delay_us(10);
        /* Slave should no longer be driving SDA low (but we still are).
         * Now prepare for the STOP condition by setting SCL high. */
        gpio_write_pin(gpiob, SCL, TRUE);
        delay_us(10);
        /* Enter the STOP condition by setting SDA high while SCL is high. */
        gpio_write_pin(gpiob, SDA, TRUE);
        delay_us(10);
        printk("%s\n",
               !gpio_read_pin(gpiob, SCL) || !gpio_read_pin(gpiob, SDA)
               ? "Still held" : "Done");
    }

    /* Check the bus is not floating (or still stuck!). We shouldn't be able to
     * pull the lines low with our internal weak pull-downs. */
    if (!reinit) {
        bool_t scl, sda;
        gpio_configure_pin(gpiob, SCL, GPI_pull_down);
        gpio_configure_pin(gpiob, SDA, GPI_pull_down);
        delay_us(10);
        scl = gpio_read_pin(gpiob, SCL);
        sda = gpio_read_pin(gpiob, SDA);
        if (!scl || !sda) {
            printk("I2C: Invalid bus SCL=%u SDA=%u\n", scl, sda);
            goto fail;
        }
    }

    /* Hand the pins to the I2C controller (with pull-ups: external
     * pull-ups are still recommended for speed). */
    gpio_configure_pin(gpiob, SCL, _GPM_FUNC(GPIO_FUNC_I2C) | _GPM_PU);
    gpio_configure_pin(gpiob, SDA, _GPM_FUNC(GPIO_FUNC_I2C) | _GPM_PU);

    /* Standard Mode (100kHz) */
    i2c_configure(FALSE);

    if (!reinit) {

#if LEVEL == LEVEL_debug
        i2c_scan();
        delay_ms(50);
#endif

        /* Probe the bus for an LCD or OLED display. */
        a = i2c_probe_range(0x20, 0x27) ?: i2c_probe_range(0x38, 0x3f);
        if (a == 0) {
            printk("I2C: %s\n",
                   i2c_dead ? "Bus locked up?" : "No device found");
            goto fail;
        }

        is_oled_display = (ff_cfg.display_type & DISPLAY_oled) ? TRUE
            : (ff_cfg.display_type & DISPLAY_lcd) ? FALSE
            : ((a&~1) == OLED_ADDR);

        if (is_oled_display) {
            oled_height = (ff_cfg.display_type & DISPLAY_oled_64) ? 64 : 32;
            lcd_columns = (ff_cfg.oled_font == FONT_8x16) ? 16
                : (ff_cfg.display_type & DISPLAY_narrower) ? 16
                : (ff_cfg.display_type & DISPLAY_narrow) ? 18 : 21;
            lcd_rows = 4;
        } else {
            lcd_columns = (ff_cfg.display_type >> _DISPLAY_lcd_columns) & 63;
            lcd_rows = (ff_cfg.display_type >> _DISPLAY_lcd_rows) & 7;
        }

        printk("I2C: %s found at 0x%02x\n",
               is_oled_display ? "OLED" : "LCD", a);
        i2c_addr = a;

        lcd_columns = max_t(uint8_t, lcd_columns, 16);
        lcd_columns = min_t(uint8_t, lcd_columns, 40);
        lcd_rows = max_t(uint8_t, lcd_rows, 2);
        lcd_rows = min_t(uint8_t, lcd_rows, 4);

        lcd_clear();

    }

    /* Enable the Event IRQ. */
    IRQx_set_prio(I2C_EVENT_IRQ, I2C_IRQ_PRI);
    IRQx_clear_pending(I2C_EVENT_IRQ);
    IRQx_enable(I2C_EVENT_IRQ);

    /* Enable the Error IRQ. */
    IRQx_set_prio(I2C_ERROR_IRQ, I2C_IRQ_PRI);
    IRQx_clear_pending(I2C_ERROR_IRQ);
    IRQx_enable(I2C_ERROR_IRQ);

    /* Timeout handler for if I2C transmission borks. */
    timer_init(&timeout_timer, timeout_fn, NULL);
    timer_set(&timeout_timer, time_now() + DMA_TIMEOUT);
    if (!reinit)
        timer_init(&retry_timer, retry_fn, NULL);

    osd_timer_start();

    if (is_oled_display) {
        oled_init();
        return TRUE;
    }

    /* Initialise 4-bit interface, as in the datasheet. Do this synchronously
     * and with the required delays. */
    write4(3 << 4);
    delay_us(4100);
    write4(3 << 4);
    delay_us(100);
    write4(3 << 4);
    write4(2 << 4);

    /* More initialisation from the datasheet. Send by DMA. */
    p = buffer;
    emit8(&p, CMD_FUNCTIONSET | FS_2LINE, 0);
    emit8(&p, CMD_DISPLAYCTL, 0);
    emit8(&p, CMD_ENTRYMODE | 2, 0);
    emit8(&p, CMD_DISPLAYCTL | 4, 0); /* display on */
    dma_start(p - buffer);

    if (!reinit)
        lcd_backlight(TRUE);

    return TRUE;

fail:
    if (reinit)
        return FALSE;
    IRQx_disable(I2C_EVENT_IRQ);
    IRQx_disable(I2C_ERROR_IRQ);
    i2c_deinit(i2c_inst);
    gpio_configure_pin(gpiob, SCL, GPI_pull_up);
    gpio_configure_pin(gpiob, SDA, GPI_pull_up);
    return FALSE;
}

/* No LCD or OLED on the I2C bus: the FF OSD alone is the display, sized as
 * FF.CFG's osd-columns (and the LCD row count of display-type). */
bool_t lcd_init_osd_only(void)
{
    if (!has_osd)
        return FALSE;

    gpio_configure_pin(gpiob, SCL, GPI_pull_up);
    gpio_configure_pin(gpiob, SDA, GPI_pull_up);

    i2c_addr = 0;
    is_oled_display = FALSE;
    lcd_columns = ff_cfg.osd_columns;
    lcd_rows = (ff_cfg.display_type >> _DISPLAY_lcd_rows) & 7;
    lcd_columns = max_t(uint8_t, lcd_columns, 16);
    lcd_columns = min_t(uint8_t, lcd_columns, 40);
    lcd_rows = max_t(uint8_t, lcd_rows, 2);
    lcd_rows = min_t(uint8_t, lcd_rows, 4);

    lcd_clear();
    lcd_backlight(TRUE);
    osd_timer_start();

    return TRUE;
}

/*
 * OLED, via u8g2.
 *
 * u8g2 renders the text rows into its frame buffer (oled_fb) using the
 * FlashFloppy fonts (oled_fonts.c). Its byte-level I2C callback runs in one
 * of two modes:
 *  - synchronous, during initialisation: each transfer is sent at once,
 *    polled, and its success checked;
 *  - queued, for frame refresh: each transfer is appended to dma_buffer[]
 *    (STOP on its last byte), and the whole frame goes out by DMA.
 * After initialisation all u8g2 calls are made from the I2C interrupt.
 */

static u8g2_t u8g2;
static uint8_t oled_fb[128*64/8];

static bool_t u8x8_queued;   /* byte callback mode (see above) */
static unsigned int dma_nr;  /* queued mode: words in dma_buffer[] */
static unsigned int xfer_start, xfer_len;
static bool_t u8x8_failed;   /* synchronous mode: a transfer failed */

static uint8_t oled_drawn_mode; /* display_mode of the rendered frame */
static uint8_t oled_bl;         /* backlight state sent to the display */

/* u8g2 fonts generated from the BDF files in fonts/ (oled_fonts.c). Each
 * glyph sits in a 16-pixel text band, @baseline pixels from its top, in a
 * @w-pixel cell starting @x0 pixels from the left edge. */
extern const uint8_t u8g2_font_ff_6x13[];
extern const uint8_t u8g2_font_ff_8x16[];
static const struct oled_font {
    const uint8_t *font;
    uint8_t x0, w, baseline;
} oled_fonts[] = {
    { u8g2_font_ff_6x13, 1, 6, 11 },
    { u8g2_font_ff_8x16, 0, 8, 12 },
};
static const struct oled_font *oled_cur_font(void)
{
    return &oled_fonts[ff_cfg.oled_font == FONT_8x16];
}

static uint8_t u8x8_byte_ff(
    u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr)
{
    const uint8_t *p = arg_ptr;

    switch (msg) {
    case U8X8_MSG_BYTE_START_TRANSFER:
        xfer_start = dma_nr;
        xfer_len = 0;
        break;
    case U8X8_MSG_BYTE_SEND:
        while (arg_int--) {
            if (u8x8_queued) {
                if (dma_nr < ARRAY_SIZE(dma_buffer))
                    dma_buffer[dma_nr++] = *p;
            } else if (xfer_len < sizeof(buffer)) {
                buffer[xfer_len++] = *p;
            }
            p++;
        }
        break;
    case U8X8_MSG_BYTE_END_TRANSFER:
        if (u8x8_queued) {
            if (dma_nr > xfer_start)
                dma_buffer[dma_nr-1] |= I2C_IC_DATA_CMD_STOP_BITS;
        } else if (!u8x8_failed && xfer_len) {
            u8x8_failed = !i2c_sync_write_txn(i2c_addr, buffer, xfer_len);
        }
        break;
    }

    return 1;
}

/* No GPIOs: I2C only, and the displays have no reset line. Millisecond
 * delays are requested only around the reset pulse u8g2 would generate on
 * that line (300ms in all, run from the I2C interrupt on a re-init), so
 * they are skipped; the drivers used here have no other waits. */
static uint8_t u8x8_gpio_and_delay_ff(
    u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr)
{
    if (msg == U8X8_MSG_DELAY_10MICRO)
        delay_us(arg_int * 10);
    return 1;
}

/* ZHONGJY_TECH 2.23" 128x32 display based on the SSD1305 controller: as
 * an SSD1306 128x32, but with alternate COM pin mapping and offset four
 * columns to the right. */
static uint8_t u8x8_d_ztech_128x32(
    u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr)
{
    static const uint8_t com_pins_seq[] = {
        U8X8_START_TRANSFER(),
        U8X8_CA(0xda, 0x12),     /* alternate com pins config */
        U8X8_END_TRANSFER(),
        U8X8_END()
    };
    uint8_t rc = u8x8_d_ssd1306_128x32_univision(
        u8x8, msg, arg_int, arg_ptr);

    switch (msg) {
    case U8X8_MSG_DISPLAY_INIT:
        u8x8_cad_SendSequence(u8x8, com_pins_seq);
        break;
    case U8X8_MSG_DISPLAY_SETUP_MEMORY:
    case U8X8_MSG_DISPLAY_SET_FLIP_MODE:
        u8x8->x_offset = 4;
        break;
    }

    return rc;
}

/* SH1106 128x32: as u8g2's driver, but with the panel at column 0, where
 * FlashFloppy has always put it (u8g2 assumes column 2). */
static uint8_t u8x8_d_sh1106_128x32(
    u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr)
{
    uint8_t rc = u8x8_d_sh1106_128x32_visionox(u8x8, msg, arg_int, arg_ptr);

    switch (msg) {
    case U8X8_MSG_DISPLAY_SETUP_MEMORY:
    case U8X8_MSG_DISPLAY_SET_FLIP_MODE:
        u8x8->x_offset = 0;
        break;
    }

    return rc;
}

/* Expand the 16-pixel text band at @page (two 8-pixel pages) to double
 * height, over four pages. */
static void oled_double_height(unsigned int page)
{
    static const uint8_t tbl[] = {
        0x00, 0x03, 0x0c, 0x0f, 0x30, 0x33, 0x3c, 0x3f,
        0xc0, 0xc3, 0xcc, 0xcf, 0xf0, 0xf3, 0xfc, 0xff
    };
    const unsigned int npages = oled_height / 8;
    uint8_t *p = &oled_fb[page * 128];
    unsigned int x;

    for (x = 0; x < 128; x++) {
        uint8_t lo = p[x], hi = p[128 + x];
        if (page + 3 < npages)
            p[384 + x] = tbl[hi >> 4];
        if (page + 2 < npages)
            p[256 + x] = tbl[hi & 15];
        p[128 + x] = tbl[lo >> 4];
        p[x] = tbl[lo & 15];
    }
}

/* Draw text row @row into the 16-pixel text band at pixel row @y. */
static void oled_draw_row(const char *row, int y)
{
    const struct oled_font *f = oled_cur_font();
    unsigned int i, x = f->x0;
    uint8_t c;

    for (i = 0; i < lcd_columns; i++) {
        c = *row++;
        if ((c < 0x20) || (c > 0x7e))
            c = '.';
        u8g2_DrawGlyph(&u8g2, x, y + f->baseline, c);
        x += f->w;
    }
}

/* Render text[] into the frame buffer. The display order lists the text
 * row shown in each 16-pixel band, top down; a double-height row takes two
 * bands. */
static void oled_render(void)
{
    const unsigned int nbands = oled_height / 16;
    unsigned int band, row;
    bool_t large;
    uint16_t order;

    order = (oled_height == 32) ? 0x7710 : menu_mode ? 0x7903 : 0x7183;
    if ((ff_cfg.display_order != DORD_default) && (display_mode == DM_normal))
        order = ff_cfg.display_order;

    u8g2_ClearBuffer(&u8g2);
    for (band = 0; band < nbands; band += large ? 2 : 1) {
        large = !!(order & DORD_double);
        row = order & DORD_row;
        if (row < lcd_rows) {
            oled_draw_row(text[row], band * 16);
            if (large)
                oled_double_height(band * 2);
        }
        order >>= DORD_shift;
    }
}

/* Queue the next frame into dma_buffer[]; return its length in words.
 * Called from the I2C interrupt, which serialises it with lcd_write(). */
static unsigned int oled_prep_frame(void)
{
    if (oled_dirty || (display_mode != oled_drawn_mode)) {
        oled_dirty = FALSE;
        oled_drawn_mode = display_mode;
        oled_render();
    }

    dma_nr = 0;

    /* Display on/off according to backlight setting. */
    if (_bl != oled_bl) {
        oled_bl = _bl;
        u8g2_SetPowerSave(&u8g2, !_bl);
    }

    u8g2_SendBuffer(&u8g2);
    refresh_count++;

    return dma_nr;
}

static bool_t oled_probe_model(void)
{
    uint8_t cmd1[] = { 0x80, 0x00, /* Column 0 */
                       0xc0 };     /* Read one data */
    uint8_t cmd2[] = { 0x80, 0x00, /* Column 0 */
                       0xc0, 0x00 }; /* Write one data */
    uint8_t rsp[2];
    int i;
    uint8_t x, px = 0;
    uint8_t *rand = (uint8_t *)emit8;

    for (i = 0; i < 3; i++) {
        /* 1st Write stage. */
        if (!i2c_sync_write_txn(i2c_addr, cmd1, sizeof(cmd1)))
            goto fail;
        /* Read stage. */
        if (!i2c_sync_read_txn(i2c_addr, rsp, sizeof(rsp)))
            goto fail;
        x = rsp[1];
        /* 2nd Write stage. */
        cmd2[3] = x ^ rand[i]; /* XOR the write with "randomness" */
        if (!i2c_sync_write_txn(i2c_addr, cmd2, sizeof(cmd2)))
            goto fail;
        /* Check we read what we wrote on previous iteration. */
        if (i && (x != px))
            break;
        /* Remember what we wrote, for next iteration. */
        px = cmd2[3];
    }

    oled_model = (i == 3) ? OLED_sh1106 : OLED_ssd1306;
    printk("OLED: %s\n", (oled_model == OLED_sh1106) ? "SH1106" : "SSD1306");
    return TRUE;

fail:
    return FALSE;
}

static void oled_init_fast_mode(void)
{
    /* Fast Mode (400kHz). */
    i2c_configure(TRUE);
}

/* Select the u8g2 display driver for the detected controller and the
 * configured geometry. */
static u8x8_msg_cb oled_driver(void)
{
    if (ff_cfg.display_type & DISPLAY_ztech)
        return u8x8_d_ztech_128x32;
    if (oled_model == OLED_sh1106)
        return (oled_height == 64) ? u8x8_d_sh1106_128x64_noname
            : u8x8_d_sh1106_128x32;
    return (oled_height == 64) ? u8x8_d_ssd1306_128x64_noname
        : u8x8_d_ssd1306_128x32_univision;
}

static void oled_init(void)
{
    u8x8_t *u8x8 = u8g2_GetU8x8(&u8g2);

    if (!(ff_cfg.display_type & DISPLAY_slow))
        oled_init_fast_mode();

    if ((oled_model == OLED_unknown) && !oled_probe_model()) {
        i2c_err_what = i2c_err_what ?: "OLED probe";
        goto fail;
    }


    /* Horizontal flip is done in software; 180-degree rotation by the
     * controller (u8g2 "flip mode"). Both together flip vertically. */
    u8x8_Setup(u8x8, oled_driver(), u8x8_cad_ssd13xx_fast_i2c,
               u8x8_byte_ff, u8x8_gpio_and_delay_ff);
    u8g2_SetupBuffer(&u8g2, oled_fb, oled_height / 8,
                     u8g2_ll_hvline_vertical_top_lsb,
                     (ff_cfg.display_type & DISPLAY_hflip)
                     ? U8G2_MIRROR : U8G2_R0);
    u8g2_SetI2CAddress(&u8g2, i2c_addr << 1);

    /* Initialise the controller, synchronously. */
    u8x8_queued = FALSE;
    u8x8_failed = FALSE;
    u8g2_InitDisplay(&u8g2);
    u8g2_SetFlipMode(&u8g2, !!(ff_cfg.display_type & DISPLAY_rotate));
    u8g2_SetContrast(&u8g2, ff_cfg.oled_contrast);
    if (ff_cfg.display_type & DISPLAY_inverse) {
        u8x8_cad_StartTransfer(u8x8);
        u8x8_cad_SendCmd(u8x8, 0xa7); /* inverse video */
        u8x8_cad_EndTransfer(u8x8);
    }
    if (u8x8_failed) {
        i2c_err_what = i2c_err_what ?: "OLED init";
        goto fail;
    }

    u8g2_SetFont(&u8g2, oled_cur_font()->font);
    u8g2_SetFontMode(&u8g2, 1); /* transparent: glyphs only set pixels */
    u8g2_SetDrawColor(&u8g2, 1);

    /* Render and send the first frame; the I2C interrupt takes it from
     * there. The display leaves initialisation powered down: oled_bl
     * records that, so the first frame switches it on. */
    u8x8_queued = TRUE;
    oled_bl = FALSE;
    oled_dirty = TRUE;
    i2c_tx_tc();
    return;

fail:
    IRQx_set_pending(I2C_ERROR_IRQ);
}
