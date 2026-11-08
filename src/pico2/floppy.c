/*
 * pico2/floppy.c
 *
 * RP2350 (Pico 2)-specific floppy-interface setup, on the pico-sdk.
 *
 * The flux engine runs on PIO0 at clk_sys=144MHz, two PIO cycles per
 * SAMPLECLK (72MHz) tick (programs in pico2/flux.pio):
 *  - the flux_rdata SM generates RDATA: DMA feeds 16-bit flux intervals
 *    from dma_rd->buf into the TX FIFO; the SM emits a fixed-width low
 *    pulse then counts out the remainder of the interval.
 *  - the flux_wdata SM captures WDATA: a free-running 16-bit "timestamp"
 *    counter is sampled at each falling edge and pushed to the RX FIFO,
 *    which DMA drains into dma_wr->buf. This mimics the STM32
 *    input-capture DMA.
 *
 * Bus outputs are driven push-pull at 3.3V while selected, as the Gotek
 * drives them: asserted lines low, everything else actively high.
 * Deselect releases the whole bus with a single SIO OE_CLR write; pins
 * whose bus line is unmapped in the current interface mode stay inputs.
 *
 * This is free and unencumbered software released into the public domain.
 * See the file COPYING for more details, or visit <http://unlicense.org>.
 */

#define O_FALSE 1
#define O_TRUE  0

/*
 * Pin assignment (physical GPIO numbers). GPIOs 2-14 carry the thirteen
 * signal lines of the 34-way floppy bus *in bus-pin order*, so a ribbon
 * header wires straight across with no crossovers:
 *
 *   GPIO   2   3   4   5   6   7   8   9  10  11  12  13  14
 *   Bus    2   8  12  16  18  20  22  24  26  28  30  32  34
 *
 * (Bus pins 16..34 are simply GPIO*2+6; the odd bus pins are ground.)
 */

/* Input pins. */
#define pin_sel0    4 /* Bus 12 */
#define pin_motor   5 /* Bus 16 */
#define pin_dir     6 /* Bus 18 */
#define pin_step    7 /* Bus 20 */
#define pin_wgate   9 /* Bus 24 */
#define pin_side   13 /* Bus 32 */

/* Output pins. */
#define pin_02     2  /* Bus 2: DSKCHG/HDEN */
#define pin_08     3  /* Bus 8: INDEX */
#define pin_26     10 /* Bus 26: TRK0 */
#define pin_28     11 /* Bus 28: WRPROT */
#define pin_34     14 /* Bus 34: RDY */

/* All OE-managed bus outputs (excludes the PIO-driven RDATA pin). */
#define FLOPPY_OUT_MASK (m(pin_02) | m(pin_08) | m(pin_26) \
                         | m(pin_28) | m(pin_34))

#define gpio_data gpioa /* ignored on RP2350 */

#define pin_wdata   8  /* Bus 22: flux_wdata SM input */
#define pin_rdata   12 /* Bus 30: flux_rdata SM output */

/* PIO block, and the state machines and DMA channels of the flux engine
 * (claimed from the SDK at start of day: see flux_hw_claim()). */
#define flux_pio pio0
static uint sm_rdata, sm_wdata;
static uint dma_rdata_ch, dma_wdata_ch;
#define dma_rdata (*dma_channel_hw_addr(dma_rdata_ch))
#define dma_wdata (*dma_channel_hw_addr(dma_wdata_ch))

/* DMA interrupt lines: RDATA on DMA_IRQ_0, WDATA on DMA_IRQ_1. */
#define dma_rdata_irq RP_DMA_IRQ_0
#define dma_wdata_irq RP_DMA_IRQ_1
DEFINE_IRQ(dma_rdata_irq, "IRQ_rdata_dma");
DEFINE_IRQ(dma_wdata_irq, "IRQ_wdata_dma");

/* All floppy-bus GPIO events arrive on the single IO_BANK0 interrupt;
 * lower-priority MOTOR/rotary work is punted to a software IRQ so that
 * timer operations stay serialised at TIMER_IRQ_PRI. */
#define FLOPPY_GPIO_IRQ RP_IO_IRQ_BANK0
#define MOTOR_CHGRST_IRQ SOFTIRQ_2
DEFINE_IRQ(FLOPPY_GPIO_IRQ, "IRQ_io_bank0");
DEFINE_IRQ(MOTOR_CHGRST_IRQ, "IRQ_MOTOR_CHGRST_rotary");

static const struct exti_irq exti_irqs[] = {
    { FLOPPY_GPIO_IRQ, FLOPPY_IRQ_SEL_PRI, 0 },
    { MOTOR_CHGRST_IRQ, TIMER_IRQ_PRI, 0 }
};

/* Subset of output pins which are active (O_TRUE). */
static uint32_t gpio_out_active;

/* Output pins we actually drive: set from the interface mode. Unmapped
 * pins stay inputs -- bus pin 2 is DENSEL on a PC cable, driven by the
 * controller, and driving against it would contend. */
static uint32_t gpio_out_driven;

/* Reconfigure the output pins for a new driven set. Caller must have IRQs
 * disabled: this races the SELA handler otherwise. */
static void board_floppy_set_driven(uint32_t mask)
{
    static const uint8_t pins[] = { pin_02, pin_08, pin_26, pin_28, pin_34 };
    unsigned int i;

    gpio_out_driven = mask;
    for (i = 0; i < ARRAY_SIZE(pins); i++) {
        if (m(pins[i]) & mask)
            gpio_configure_pin(gpioa, pins[i], GPO_bus);
        else
            gpio_configure_pin(gpioa, pins[i], GPI_pull_up);
    }
    /* Leave the bus consistent with the current select state. */
    if (drive.sel) {
        gpio_clr_mask(gpio_out_active & mask);
        gpio_set_mask(~gpio_out_active & mask);
        gpio_set_dir_out_masked(mask);
    } else {
        gpio_set_dir_in_masked(mask);
    }
}

/* Is the RDATA read stream active? Read by the SELA handler. */
static volatile uint8_t rdata_active;
#define dma_rd_set_active(x) (rdata_active = (x))

/* Toggle pin 34 on each deselect? (Amiga HD-ID "magic".) */
static bool_t sela_amiga_hd_id;

/* Hand the RDATA pin to the PIO, or back to SIO. Called from the SELA
 * handler, so this is a bare function-select write: the pad itself was set
 * up by gpio_configure_pin() (gpio_set_function() would rewrite it too). */
static always_inline void rdata_set_function(enum gpio_function_rp2350 fn)
{
    io_bank0_hw->io[pin_rdata].ctrl = fn << IO_BANK0_GPIO0_CTRL_FUNCSEL_LSB;
}

/* Latched flag: MOTOR pin changed (processed at TIMER_IRQ_PRI). */
static volatile uint8_t motor_pin_event;

uint32_t motor_chgrst_exti_mask;

/* PIO programs, assembled by pioasm from pico2/flux.pio. */
#include "flux.pio.h"
static uint rdata_prog_offset, wdata_prog_offset;

/* Fixed per-interval overhead of the RDATA PIO program, in SAMPLECLK
 * ticks. Producer entries represent (value + FLUX_LEAD) ticks; the STM32
 * timer encoding is (value + 1). flux_adjust() converts. */
#define FLUX_LEAD 30
#define DMA_RD_TICKS(x) ((uint32_t)(x) + FLUX_LEAD)

static void flux_adjust(uint16_t *buf, unsigned int nr)
{
    /* Rebias flux intervals from the generic (x+1)-ticks encoding to
     * this PIO program's (x+FLUX_LEAD)-ticks encoding. */
    while (nr--) {
        uint16_t x = *buf;
        *buf++ = (x > (FLUX_LEAD-1)) ? x - (FLUX_LEAD-1) : 0;
    }
}

/* DMA engine positions, as sample counts into the rings (analogous to
 * the STM32 "SIZE - CNDTR" calculation). */
#define dma_rdata_pos()                                                 \
    ((uint16_t)((dma_rdata.read_addr - (uint32_t)dma_rd->buf) / 2)      \
     & (ARRAY_SIZE(dma_rd->buf) - 1))
#define dma_wdata_pos()                                                 \
    ((uint16_t)((dma_wdata.write_addr - (uint32_t)dma_wr->buf) / 2)     \
     & (ARRAY_SIZE(dma_wr->buf) - 1))

/* Ticks remaining in the currently-playing sample: not cheaply observable
 * on PIO (the SM scratch registers are not readable by the CPU). The only
 * consumer tolerates a one-sample error in index alignment. */
#define rdata_ticks_remaining() 0

#define flux_dma_ack_rdata() dma_irqn_acknowledge_channel(0, dma_rdata_ch)
#define flux_dma_ack_wdata() dma_irqn_acknowledge_channel(1, dma_wdata_ch)

/* Self-retriggering ring transfers: an IRQ every half-ring, like the
 * STM32 half/full-transfer interrupts. */
#define dma_rdata_count \
    dma_encode_transfer_count_with_self_trigger(ARRAY_SIZE(dma_rd->buf) / 2)
#define dma_wdata_count \
    dma_encode_transfer_count_with_self_trigger(ARRAY_SIZE(dma_wr->buf) / 2)

static bool_t rdata_dma_en, wdata_dma_en;

/* Stop a state machine and restart it at @pc, optionally emptying its
 * FIFOs. */
static void flux_sm_reset(uint sm, uint pc, bool_t drain)
{
    pio_sm_set_enabled(flux_pio, sm, false);
    if (drain)
        pio_sm_clear_fifos(flux_pio, sm);
    pio_sm_restart(flux_pio, sm);
    pio_sm_clkdiv_restart(flux_pio, sm);
    pio_sm_exec(flux_pio, sm, pio_encode_jmp(pc));
}

/* Reset the RDATA generator with its pin idle high and the output driver
 * on (push-pull). */
static void rdata_sm_reset(void)
{
    flux_sm_reset(sm_rdata, rdata_prog_offset, TRUE);
    pio_sm_exec(flux_pio, sm_rdata, pio_encode_set(pio_pins, 1));
    pio_sm_exec(flux_pio, sm_rdata, pio_encode_set(pio_pindirs, 1));
}

/*
 * Stream start/stop. Stopping a stream halts its DMA channel with
 * dma_channel_abort(), which returns once any in-flight transfer has
 * retired: the channel is then idle.
 *
 * A stream must never be restarted with its channel still live, because
 * flux_sm_reset() clears the state machine's FIFO: emptying a FIFO
 * underneath a DREQ-paced channel leaves the channel's outstanding-request
 * counter out of step with the FIFO, and it then sits EN=1/BUSY=1 with a
 * full transfer count and never moves another byte. Aborting here also
 * freezes the channel's position, so the dma_*_pos() reading that the
 * caller snapshots next is exact rather than up to a FIFO-depth ahead.
 *
 * Called from IRQ context: the wait is a few cycles (one 16-bit transfer
 * at most).
 */
static void rdata_hw_start(void)
{
    rdata_sm_reset();
    /* Re-arm the channel. It resumes at its current read_addr, which is the
     * position dma_rd->cons was snapped to when the stream stopped. */
    dma_channel_set_transfer_count(dma_rdata_ch, dma_rdata_count, false);
    rdata_dma_en = TRUE;
    dma_channel_start(dma_rdata_ch);
    pio_sm_set_enabled(flux_pio, sm_rdata, true);
}

static void rdata_hw_stop(void)
{
    pio_sm_set_enabled(flux_pio, sm_rdata, false);
    if (rdata_dma_en) {
        rdata_dma_en = FALSE;
        dma_channel_abort(dma_rdata_ch);
        flux_dma_ack_rdata();
    }
}

static void wdata_hw_start(void)
{
    flux_sm_reset(sm_wdata, wdata_prog_offset, TRUE);
    /* Re-arm the channel; it continues writing where it left off, which is
     * the ring position recorded as the previous write's dma_end. */
    dma_channel_set_transfer_count(dma_wdata_ch, dma_wdata_count, false);
    wdata_dma_en = TRUE;
    dma_channel_start(dma_wdata_ch);
    pio_sm_set_enabled(flux_pio, sm_wdata, true);
}

static void wdata_hw_stop(void)
{
    unsigned int i;
    pio_sm_set_enabled(flux_pio, sm_wdata, false);
    /* Let DMA drain the RX FIFO of any tail-end samples (fast, bounded). */
    for (i = 0; i < 1000; i++) {
        if (pio_sm_is_rx_fifo_empty(flux_pio, sm_wdata))
            break;
        cpu_relax();
    }
    if (wdata_dma_en) {
        wdata_dma_en = FALSE;
        dma_channel_abort(dma_wdata_ch);
        flux_dma_ack_wdata();
    }
}

/* Stop both streams and disable their channels (on image unmount). */
static void flux_dma_disable(void)
{
    pio_sm_set_enabled(flux_pio, sm_rdata, false);
    pio_sm_set_enabled(flux_pio, sm_wdata, false);
    dma_channel_abort(dma_rdata_ch);
    dma_channel_abort(dma_wdata_ch);
    hw_clear_bits(&dma_rdata.al1_ctrl, DMA_CH0_CTRL_TRIG_EN_BITS);
    hw_clear_bits(&dma_wdata.al1_ctrl, DMA_CH0_CTRL_TRIG_EN_BITS);
    rdata_dma_en = wdata_dma_en = FALSE;
}

/* Claim the PIO state machines and DMA channels, and load the programs.
 * Called once, from board_floppy_init(): they are kept for the lifetime of
 * the firmware, so every later reference to them is valid. */
static void flux_hw_claim(void)
{
    rdata_prog_offset = pio_add_program(flux_pio, &flux_rdata_program);
    wdata_prog_offset = pio_add_program(flux_pio, &flux_wdata_program);
    sm_rdata = pio_claim_unused_sm(flux_pio, true);
    sm_wdata = pio_claim_unused_sm(flux_pio, true);
    dma_rdata_ch = dma_claim_unused_channel(true);
    dma_wdata_ch = dma_claim_unused_channel(true);
}

/* Initialise PIO and DMA for RDATA/WDATA (called at image mount). */
static void timer_dma_hw_init(void)
{
    pio_sm_config c;
    dma_channel_config dc;

    /* Ring buffers must be aligned for the DMA address wrap. */
    ASSERT(!((uint32_t)dma_rd->buf & (sizeof(dma_rd->buf)-1)));
    ASSERT(!((uint32_t)dma_wr->buf & (sizeof(dma_wr->buf)-1)));
    BUILD_BUG_ON(sizeof(dma_rd->buf) != (1u << 11));

    /* Enable DMA interrupts. */
    flux_dma_ack_rdata();
    flux_dma_ack_wdata();
    IRQx_set_prio(dma_rdata_irq, RDATA_IRQ_PRI);
    IRQx_set_prio(dma_wdata_irq, WDATA_IRQ_PRI);
    IRQx_enable(dma_rdata_irq);
    IRQx_enable(dma_wdata_irq);
    dma_irqn_set_channel_enabled(0, dma_rdata_ch, true);
    dma_irqn_set_channel_enabled(1, dma_wdata_ch, true);

    /* RDATA generator: undivided clock, 16-bit autopull (shift right),
     * joined TX FIFO, SET drives the RDATA pin. */
    c = flux_rdata_program_get_default_config(rdata_prog_offset);
    sm_config_set_clkdiv_int_frac8(&c, 1, 0);
    sm_config_set_out_shift(&c, true, true, 16);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
    sm_config_set_set_pins(&c, pin_rdata, 1);
    pio_sm_init(flux_pio, sm_rdata, rdata_prog_offset, &c);
    /* Idle high with the output driver on: push-pull. */
    rdata_sm_reset();

    /* WDATA capture: undivided clock, joined RX FIFO, JMP PIN = WDATA. */
    c = flux_wdata_program_get_default_config(wdata_prog_offset);
    sm_config_set_clkdiv_int_frac8(&c, 1, 0);
    sm_config_set_in_pins(&c, pin_wdata);
    sm_config_set_jmp_pin(&c, pin_wdata);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);
    pio_sm_init(flux_pio, sm_wdata, wdata_prog_offset, &c);

    /* RDATA DMA: dma_rd->buf ring -> TX FIFO, 2048-byte read ring. Left
     * disabled until rdata_hw_start(): the ring is still empty. */
    dc = dma_channel_get_default_config(dma_rdata_ch);
    channel_config_set_transfer_data_size(&dc, DMA_SIZE_16);
    channel_config_set_read_increment(&dc, true);
    channel_config_set_write_increment(&dc, false);
    channel_config_set_ring(&dc, false, 11);
    channel_config_set_dreq(&dc, pio_get_dreq(flux_pio, sm_rdata, true));
    dma_channel_configure(dma_rdata_ch, &dc, &flux_pio->txf[sm_rdata],
                          dma_rd->buf, dma_rdata_count, false);

    /* WDATA DMA: RX FIFO -> dma_wr->buf ring, 2048-byte write ring. */
    dc = dma_channel_get_default_config(dma_wdata_ch);
    channel_config_set_transfer_data_size(&dc, DMA_SIZE_16);
    channel_config_set_read_increment(&dc, false);
    channel_config_set_write_increment(&dc, true);
    channel_config_set_ring(&dc, true, 11);
    channel_config_set_dreq(&dc, pio_get_dreq(flux_pio, sm_wdata, false));
    dma_channel_configure(dma_wdata_ch, &dc, dma_wr->buf,
                          &flux_pio->rxf[sm_wdata], dma_wdata_count, false);

    rdata_dma_en = wdata_dma_en = FALSE;
}

bool_t floppy_ribbon_is_reversed(void)
{
    time_t t_start = time_now();

    /* If the ribbon is reversed then most/all inputs are grounded.
     * Check four inputs which are supposed only to pulse. */
    while (!(gpio_get_all()
             & (m(pin_sel0) | m(pin_step) | m(pin_wdata) | m(pin_wgate)))) {
        /* If all four inputs are LOW for a full second, conclude that
         * the ribbon is reversed. */
        if (time_since(t_start) > time_ms(1000))
            return TRUE;
    }

    return FALSE;
}

static void board_floppy_init(void)
{
    flux_hw_claim();

    /* Inputs, in bus-pin order. */
    gpio_configure_pin(gpioa, pin_sel0,  GPI_bus);
    gpio_configure_pin(gpiob, pin_motor, GPI_pull_down);
    gpio_configure_pin(gpiob, pin_dir,   GPI_bus);
    gpio_configure_pin(gpioa, pin_step,  GPI_bus);
    gpio_configure_pin(gpio_data, pin_wdata, GPI_bus);
    gpio_configure_pin(gpiob, pin_wgate, GPI_bus);
    gpio_configure_pin(gpiob, pin_side,  GPI_bus);

    /* RDATA: SIO function, output latch low, high-impedance. */
    gpio_configure_pin(gpio_data, pin_rdata, GPO_rdata);

    /* GPIO edge interrupts. STEP is sampled on its trailing (rising)
     * edge; the other inputs act on both edges. */
    gpio_irq_enable(pin_sel0,  GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE);
    gpio_irq_enable(pin_step,  GPIO_IRQ_EDGE_RISE);
    gpio_irq_enable(pin_wgate, GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE);
    gpio_irq_enable(pin_side,  GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE);
}

/* Update the SELA handler. Used for switching in the Amiga HD-ID "magic".
 * Must be called with interrupts disabled. */
static void update_SELA_irq(bool_t amiga_hd_id)
{
    sela_amiga_hd_id = amiga_hd_id;
}

/* Apply the select/deselect state of the bus. Called on SELA edges and
 * once at start of day to prime the state. */
static void IRQ_SELA_changed(void)
{
    struct drive *drv = &drive;
    bool_t sel = !(gpio_get_all() & m(pin_sel0));

    if (sel == drv->sel)
        return;
    drv->sel = sel;

    /* Light the on-board LED while we are the selected drive. */
    board_set_activity_led(sel);

    if (sel) {
        /* SELA is asserted (this drive is selected). Drive the bus:
         * asserted lines low, the rest actively high. Levels first, then
         * output-enable. RDATA idles high via SIO until the PIO takes it. */
        gpio_clr_mask(gpio_out_active & gpio_out_driven);
        gpio_set_mask(~gpio_out_active & gpio_out_driven);
        gpio_set_dir_out_masked(gpio_out_driven | m(pin_rdata));
        /* Hand RDATA to the PIO if the read stream is active. */
        if (rdata_active)
            rdata_set_function(GPIO_FUNC_PIO0);
    } else {
        /* SELA is deasserted (this drive is not selected).
         * Relinquish the bus by disabling all our outputs. */
        gpio_set_dir_in_masked(gpio_out_driven | m(pin_rdata));
        rdata_set_function(GPIO_FUNC_SIO);
        /* Amiga HD-ID: toggle pin 34 for next time we take the bus. */
        if (sela_amiga_hd_id)
            gpio_out_active ^= m(pin_34);
    }
}

static bool_t drive_is_writing(void)
{
    if (!dma_wr)
        return FALSE;
    switch (dma_wr->state) {
    case DMA_starting:
    case DMA_active:
        return TRUE;
    }
    return FALSE;
}

/*
 * Shortest gap between STEP pulses we will believe, in microseconds.
 *
 * An unbuffered ribbon rings hard enough to put thousands of spurious edges
 * on this line, in bursts tens of microseconds apart. Most are rejected
 * anyway because a step is already in progress, but one that lands after
 * the previous step has settled is indistinguishable from a real one, and
 * costs a cylinder: the host then reads a track it did not ask for and the
 * sector headers no longer match, which presents as an unreadable disk
 * rather than as a seek fault.
 *
 * The floor on real hardware is the NEC765's 1ms minimum step rate, and
 * Amiga/Atari hosts step at 3ms, so half a millisecond discards ringing
 * with a wide margin and cannot swallow a genuine step. Measured from the
 * last edge of any kind, not the last accepted one, so a burst holds itself
 * off until the line goes quiet.
 */
#define STEP_GLITCH_FILTER_US 500

static time_t step_prev_time;

static void IRQ_STEP_changed(void)
{
    struct drive *drv = &drive;
    uint32_t in = gpio_get_all();
    time_t now = time_now();
    bool_t glitch = time_diff(step_prev_time, now)
        < (int32_t)time_us(STEP_GLITCH_FILTER_US);

    step_prev_time = now;
    if (glitch)
        return;

    /* Bail if drive not selected. */
    if (in & m(pin_sel0))
        return;

    /* Deassert DSKCHG if a disk is inserted. */
    if ((drv->outp & m(outp_dskchg)) && drv->inserted
        && (ff_cfg.chgrst == CHGRST_step))
        drive_change_output(drv, outp_dskchg, FALSE);

    /* Do we accept this STEP command? */
    if ((drv->step.state & STEP_active) /* Already mid-step? */
        || drive_is_writing())   /* Write in progress? */
        return;

    /* Latch the step direction and check bounds (0 <= cyl <= 255). */
    drv->step.inward = !(in & m(pin_dir));
    if (drv->cyl == (drv->step.inward ? ff_cfg.max_cyl : 0))
        return;

    /* Valid step request for this drive: start the step operation. */
    drv->step.start = time_now();
    drv->step.state = STEP_started;
    if (drv->outp & m(outp_trk0))
        drive_change_output(drv, outp_trk0, FALSE);
    if (dma_rd != NULL) {
        rdata_stop();
        if (!ff_cfg.index_suppression
                && ff_cfg.track_change != TRKCHG_realtime) {
            /* Opportunistically insert an INDEX pulse ahead of seek op. */
            drive_change_output(drv, outp_index, TRUE);
            index.fake_fired = TRUE;
        }
    }
    IRQx_set_pending(FLOPPY_SOFTIRQ);
}

static void IRQ_SIDE_changed(void)
{
    stk_time_t t = stk_now();
    unsigned int filter = stk_us(ff_cfg.side_select_glitch_filter);
    struct drive *drv = &drive;
    uint8_t hd;

    do {
        /* Has SIDE actually changed? */
        hd = !(gpio_get_all() & m(pin_side));
        if (hd == drv->head)
            return;

        /* If configured to do so, wait a few microseconds to ensure this isn't
         * a glitch (eg. signal is mistaken for the archaic Fault-Reset line by
         * old CP/M loaders, and pulsed LOW when starting a read). */
    } while (stk_diff(t, stk_now()) < filter);

    drv->head = hd;
    if ((dma_rd != NULL) && (drv->image->nr_sides == 2))
        rdata_stop();
}

static void IRQ_WGATE(void)
{
    struct drive *drv = &drive;
    uint32_t in = gpio_get_all();

    /* If WRPROT line is asserted then we ignore WGATE. */
    if (drv->outp & m(outp_wrprot))
        return;

    if ((in & m(pin_wgate))          /* WGATE off? */
        || (in & m(pin_sel0))) {     /* Not selected? */
        wdata_stop();
    } else {
        rdata_stop();
        wdata_start();
    }
}

/* Test a floppy-bus pin's latched edges in the INTS words read by
 * IRQ_io_bank0(): four status bits per GPIO, GPIOs 0-7 in ints0 and
 * GPIOs 8-15 in ints1. Pin numbers are compile-time constants. (The SDK's
 * gpio_get_irq_event_mask() would re-read INTS once per pin: too slow for
 * this latency-critical path.) */
#define pin_ints(pin) \
    (((pin) < 8 ? ints0 : ints1) & (0xfu << (((pin)&7)*4)))

/* Single dispatcher for all floppy-bus GPIO edges. Runs at
 * FLOPPY_IRQ_SEL_PRI; only handlers which perform no timer operations may
 * be called from here. MOTOR (and rotary) events are latched and punted
 * to MOTOR_CHGRST_IRQ at TIMER_IRQ_PRI. */
static void IRQ_io_bank0(void)
{
    io_bank0_irq_ctrl_hw_t *irq_ctrl = &io_bank0_hw->proc0_irq_ctrl;
    uint32_t ints0 = irq_ctrl->ints[0]; /* GPIOs 0-7 */
    uint32_t ints1 = irq_ctrl->ints[1]; /* GPIOs 8-15 */
    uint32_t ints3 = irq_ctrl->ints[3]; /* GPIOs 24-31 */

    /* Acknowledge the latched edges we are about to process. */
    if (ints0)
        io_bank0_hw->intr[0] = ints0;
    if (ints1)
        io_bank0_hw->intr[1] = ints1;
    if (ints3)
        io_bank0_hw->intr[3] = ints3;

    /* SELA first: it is the most latency-critical. Level-based, so it is
     * also how the primed (software-pended) first interrupt initialises
     * the select state. */
    IRQ_SELA_changed();

    if (pin_ints(pin_wgate))
        IRQ_WGATE();

    if (pin_ints(pin_step))
        IRQ_STEP_changed();

    if (pin_ints(pin_side))
        IRQ_SIDE_changed();

    if (pin_ints(pin_motor)) {
        motor_pin_event = TRUE;
        IRQx_set_pending(MOTOR_CHGRST_IRQ);
    }

    /* Rotary encoder (GPIOs 26-27): punt to low-priority handler. */
    if (ints3)
        IRQx_set_pending(MOTOR_CHGRST_IRQ);
}

static void IRQ_MOTOR(struct drive *drv)
{
    bool_t mtr_asserted = !(gpio_get_all() & m(pin_motor));

    if (drv->amiga_pin34 && (ff_cfg.motor_delay != MOTOR_ignore)) {
        IRQ_global_disable();
        drive_change_pin(drv, pin_34, !mtr_asserted);
    }

    timer_cancel(&drv->motor.timer);
    drv->motor.on = FALSE;

    if (!drv->inserted) {
        /* No disk inserted -- MOTOR OFF */
        drive_change_output(drv, outp_rdy, FALSE);
    } else if (ff_cfg.motor_delay == MOTOR_ignore) {
        /* Motor signal ignored -- MOTOR ON */
        drv->motor.on = TRUE;
        drive_change_output(drv, outp_rdy, TRUE);
    } else if (!mtr_asserted) {
        /* Motor signal off -- MOTOR OFF */
        drive_change_output(drv, outp_rdy, FALSE);
    } else {
        /* Motor signal on -- MOTOR SPINNING UP */
        timer_set(&drv->motor.timer,
                  time_now() + time_ms(ff_cfg.motor_delay * 10));
    }
}

static void IRQ_MOTOR_CHGRST_rotary(void)
{
    struct drive *drv = &drive;
    bool_t changed = drv->motor.changed;
    bool_t motor_event = motor_pin_event;

    drv->motor.changed = FALSE;
    motor_pin_event = FALSE;

    if ((motor_event && (ff_cfg.motor_delay != MOTOR_ignore)) || changed)
        IRQ_MOTOR(drv);

    /* Note: the CHGRST_pa14 input is not supported on this board. */

    if (board_rotary_exti_mask)
        IRQ_rotary();
}

static void motor_chgrst_update_status(struct drive *drv)
{
    drv->motor.changed = TRUE;
    barrier();
    IRQx_set_pending(MOTOR_CHGRST_IRQ);
}

void motor_chgrst_setup_exti(void)
{
    uint32_t m = 0;

    if (ff_cfg.motor_delay != MOTOR_ignore) {
        gpio_irq_enable(pin_motor, GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE);
        m |= m(pin_motor);
    }

    motor_chgrst_exti_mask = m;

    motor_chgrst_update_status(&drive);
}
