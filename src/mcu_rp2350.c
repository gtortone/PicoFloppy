/*
 * mcu_rp2350.c
 *
 * RP2350 (Raspberry Pi Pico 2) system initialisation and GPIO management,
 * on top of the pico-sdk.
 *
 * The SDK runtime has already brought up clocks (clk_sys = 144MHz, see
 * CMakeLists.txt), the 1MHz tick generators and the peripheral resets by
 * the time main() runs. What remains here is FlashFloppy's Cortex setup
 * and its STM32-style GPIO interface.
 *
 * This is free and unencumbered software released into the public domain.
 * See the file COPYING for more details, or visit <http://unlicense.org>.
 */

unsigned int ram_kb = 512;

/* Pins configured for emulated open-drain (assert = drive low, deassert =
 * high impedance). Consulted by gpio_write_pin[s]_(). */
uint32_t gpio_od_mask;

/* The flux engine's timing is derived from clk_sys, which the SDK runtime
 * configures from SYS_CLK_HZ (CMakeLists.txt). */
_Static_assert(SYS_CLK_HZ == SYSCLK_MHZ * 1000000,
               "clk_sys must be 144MHz for the PIO flux engine");

void mcu_init(void)
{
    cortex_init();

    /* Reserve the spare IRQ lines that FlashFloppy uses as software IRQs
     * (SOFTIRQ_0-2, and the I2C display's error handler) against any SDK
     * library that claims one (e.g. USB, pico2/usb_cdc.c). */
    user_irq_claim(SPARE_IRQ_0);
    user_irq_claim(SPARE_IRQ_1);
    user_irq_claim(SPARE_IRQ_2);
    user_irq_claim(SPARE_IRQ_3);

    cpu_sync();
}

void gpio_configure_pin(GPIO gpio, unsigned int pin, unsigned int mode)
{
    uint32_t mask = m(pin);

    if (mode & _GPM_OD)
        gpio_od_mask |= mask;
    else
        gpio_od_mask &= ~mask;

    /* Output level and direction via SIO. */
    if (mode & _GPM_OUT)
        gpio_set_mask(mask);
    else
        gpio_clr_mask(mask);
    gpio_set_dir(pin, !!(mode & _GPM_OE));

    /* Pad: pulls, Schmitt input, and drive strength. Bus outputs ask for
     * the strongest drive: they must sink the host's termination current,
     * and a pad that cannot pull the line below the controller's input
     * threshold reads as a dead line however correct the firmware. */
    gpio_set_pulls(pin, !!(mode & _GPM_PU), !!(mode & _GPM_PD));
    gpio_set_input_hysteresis_enabled(pin, true);
    gpio_set_drive_strength(pin, (mode & _GPM_HIDRIVE)
                            ? GPIO_DRIVE_STRENGTH_12MA
                            : GPIO_DRIVE_STRENGTH_4MA);

    /* Function select. Also enables the input buffer and clears the pad
     * isolation latch that the RP2350 sets out of reset. */
    gpio_set_function(pin, _GPM_FUNC(mode));
}

void gpio_write_pin_(unsigned int pin, unsigned int level)
{
    gpio_write_pins_(m(pin), level);
}

void gpio_write_pins_(uint32_t mask, unsigned int level)
{
    uint32_t od = mask & gpio_od_mask;
    uint32_t pp = mask & ~gpio_od_mask;
    if (level) {
        /* Open-drain emulation: high = float. */
        if (od) gpio_set_dir_in_masked(od);
        if (pp) gpio_set_mask(pp);
    } else {
        /* Open-drain emulation: low = drive low. */
        if (od) gpio_set_dir_out_masked(od);
        if (pp) gpio_clr_mask(pp);
    }
}

/* Disable all GPIO interrupt events for the pins in @pin_mask. */
void gpio_irq_disable_mask(uint32_t pin_mask)
{
    unsigned int pin;
    for (pin = 0; pin_mask != 0; pin++, pin_mask >>= 1) {
        if (pin_mask & 1)
            gpio_set_irq_enabled(pin, GPIO_IRQ_LEVEL_LOW | GPIO_IRQ_LEVEL_HIGH
                                 | GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE,
                                 false);
    }
}

void rp2350_enter_bootsel(void)
{
    /* Let the console drain, then hand over to the bootrom's UF2 loader. */
    console_sync();
    printk("Entering BOOTSEL...\n");
    reset_usb_boot(0, 0);
}
