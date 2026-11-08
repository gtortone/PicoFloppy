/*
 * pico2/board.c
 *
 * Raspberry Pi Pico 2 (RP2350) board-specific setup and management.
 *
 * Pin map (physical GPIO numbers):
 *   0/1    FF OSD: Amiga keyboard KB_DATA, KB_CLOCK
 *   2-14   Floppy bus, one GPIO per signal line, in bus-pin order:
 *            bus  2  8 12 16 18 20 22 24 26 28 30 32 34
 *          i.e. DSKCHG, INDEX, SEL0, MOTOR, DIR, STEP, WDATA, WGATE,
 *          TRK0, WRPROT, RDATA, SIDE, RDY
 *   15     FF OSD: video CSYNC input
 *   16-19  SD card on SPI0: MISO, CS, SCK, MOSI
 *   20/21  Display I2C0: SDA, SCL
 *   22     Button: Select
 *   25     On-board LED: lit while the drive is selected (accessed)
 *   26/27  Buttons: Right/Left, or rotary encoder A/B
 *   28     FF OSD: video output (via ~270 ohm to one of R, G, B)
 *
 * The debug console is on the Pico 2's own USB socket. There is no speaker
 * and no JC jumper: their pins went to the on-screen display (pico2/osd.c).
 *
 * This is free and unencumbered software released into the public domain.
 * See the file COPYING for more details, or visit <http://unlicense.org>.
 */

#define pin_button_sel   22
#define pin_button_right 26
#define pin_button_left  27
#define pin_led          PICO_DEFAULT_LED_PIN /* Pico 2 on-board LED (25) */

unsigned int board_get_buttons(void)
{
    /* Select=1, Right=2, Left=4 (matching the Gotek encoding). */
    uint32_t in = gpio_get_all();
    unsigned int x = ((in >> pin_button_sel) & 1)
        | (((in >> pin_button_right) & 1) << 1)
        | (((in >> pin_button_left) & 1) << 2);
    return ~x & 7;
}

unsigned int board_get_rotary(void)
{
    uint32_t in = gpio_get_all();
    return ((in >> pin_button_right) & 1) | (((in >> pin_button_left) & 1) << 1);
}

uint32_t board_rotary_exti_mask;
void board_setup_rotary_exti(void)
{
    gpio_irq_enable(pin_button_right, GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE);
    gpio_irq_enable(pin_button_left, GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE);
    board_rotary_exti_mask = m(pin_button_right) | m(pin_button_left);
}

/* Drive-activity indicator: the Pico 2's own LED, mirroring the front-panel
 * LED of a real floppy drive (lit while the drive is selected). Called from
 * the SELA interrupt handler, so it must stay cheap. */
void board_set_activity_led(bool_t on)
{
    gpio_write_pin(gpioa, pin_led, on);
}

/* No JC jumper (its pin is the OSD video output): the interface default
 * is Shugart, and the Amiga HD-ID output on JC is not available. */
void board_jc_set_mode(unsigned int mode)
{
}

bool_t board_jc_strapped(void)
{
    return FALSE;
}

void board_init(void)
{
    /* GPIO 0-15 and 28 (OSD, floppy bus), 16-21 (SD card, display) are
     * configured by their own drivers. Pull up the buttons. */
    gpio_configure_pin(gpioa, pin_button_sel, GPI_pull_up);
    gpio_configure_pin(gpioa, pin_button_right, GPI_pull_up);
    gpio_configure_pin(gpioa, pin_button_left, GPI_pull_up);

    /* On-board LED: drive-activity indicator, initially off. */
    gpio_configure_pin(gpioa, pin_led, GPO_pushpull(_2MHz, LOW));

    /* The Pico 2's own USB socket: debug console, and the 1200-baud
     * BOOTSEL gesture. */
    usb_cdc_init(USB_IRQ_PRI);
}
