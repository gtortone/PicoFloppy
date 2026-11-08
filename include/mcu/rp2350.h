/*
 * rp2350.h
 *
 * RP2350 (Raspberry Pi Pico 2) MCU environment, on top of the pico-sdk.
 *
 * The SDK headers themselves are pulled in by mcu/rp2350_sdk.h, ahead of
 * all FlashFloppy headers (see decls.h). This file adapts FlashFloppy's
 * generic MCU interface (mcu/common.h: GPIO modes, IRQ numbering, clocks)
 * to the SDK.
 *
 * This is free and unencumbered software released into the public domain.
 * See the file COPYING for more details, or visit <http://unlicense.org>.
 */

/* SPI handle type of the generic interface (mcu/common.h, spi.h): opaque;
 * it carries an SDK spi_inst_t. */
struct spi;

/* C-accessible Cortex-M33 core registers (SysTick, SCB, NVIC): see
 * mcu/common_regs.h. */
static STK stk = (struct stk *)STK_BASE;
static SCB scb = (struct scb *)SCB_BASE;
static NVIC nvic = (struct nvic *)NVIC_BASE;

/* Clocks: clk_sys is configured by the SDK runtime from SYS_CLK_HZ (see
 * CMakeLists.txt). 144MHz, so that a 2-cycles-per-tick PIO program yields
 * exactly the 72MHz SAMPLECLK used by the floppy flux engine. */
#define SYSCLK_MHZ 144
#define APB1_MHZ SYSCLK_MHZ /* clk_peri == clk_sys */

/* Interrupt numbers used with DEFINE_IRQ(). It pastes the number into the
 * handler's name (isr_irq<n>), so it must be a literal: in C the SDK has
 * these only as enum constants, hence the copies (checked just below). */
#define RP_TIMER0_IRQ_0  0
#define RP_DMA_IRQ_0    10
#define RP_DMA_IRQ_1    11
#define RP_IO_IRQ_BANK0 21
#define RP_I2C0_IRQ     36
#define RP_SPARE_IRQ_0  46
#define RP_SPARE_IRQ_1  47
#define RP_SPARE_IRQ_2  48
#define RP_SPARE_IRQ_3  49
_Static_assert((RP_TIMER0_IRQ_0 == TIMER0_IRQ_0) && (RP_DMA_IRQ_0 == DMA_IRQ_0)
               && (RP_DMA_IRQ_1 == DMA_IRQ_1)
               && (RP_IO_IRQ_BANK0 == IO_IRQ_BANK0)
               && (RP_I2C0_IRQ == I2C0_IRQ)
               && (RP_SPARE_IRQ_0 == SPARE_IRQ_0)
               && (RP_SPARE_IRQ_1 == SPARE_IRQ_1)
               && (RP_SPARE_IRQ_2 == SPARE_IRQ_2)
               && (RP_SPARE_IRQ_3 == SPARE_IRQ_3),
               "RP2350 IRQ numbers disagree with the pico-sdk");

/* Software IRQs, borrowed from the RP2350 spare interrupt lines. */
#define SOFTIRQ_0 RP_SPARE_IRQ_0
#define SOFTIRQ_1 RP_SPARE_IRQ_1
#define SOFTIRQ_2 RP_SPARE_IRQ_2

/* Exception handlers are placed into the SDK's vector table by name (see
 * also __DEFINE_IRQ in intrinsics.h). SVCall drives cancel_call(). */
#define EXC_sv_call isr_svcall

/* Generic pin levels */
#define LOW  0
#define HIGH 1

/* GPIO API. RP2350 has a single GPIO bank; the gpioa/gpiob/gpioc handles
 * accepted by these functions are ignored, and pin numbers are physical
 * GPIO numbers 0-29. Pins configured as open-drain (emulated: drive low
 * vs high-impedance) are tracked in gpio_od_mask. */
struct gpio;
static GPIO gpioa = (struct gpio *)1;
static GPIO gpiob = (struct gpio *)2;
static GPIO gpioc = (struct gpio *)3;

extern uint32_t gpio_od_mask;

/* Mode encoding for gpio_configure_pin():
 *  [4:0] function select (SDK enum gpio_function_rp2350), [5] pull-up,
 *  [6] pull-down, [7] output enable, [8] output level, [9] open-drain
 *  emulation, [10] high (12mA) pad drive */
#define _GPM_FUNC(x) ((x)&0x1f)
#define _GPM_PU  (1u<<5)
#define _GPM_PD  (1u<<6)
#define _GPM_OE  (1u<<7)
#define _GPM_OUT (1u<<8)
#define _GPM_OD  (1u<<9)
#define _GPM_HIDRIVE (1u<<10)

#define GPI_floating  _GPM_FUNC(GPIO_FUNC_SIO)
#define GPI_pull_up   (_GPM_FUNC(GPIO_FUNC_SIO) | _GPM_PU)
#define GPI_pull_down (_GPM_FUNC(GPIO_FUNC_SIO) | _GPM_PD)
/* Output speed is ignored on RP2350: define the usual tokens to 0. */
#define _2MHz  0
#define _10MHz 0
#define _50MHz 0
#define GPO_pushpull(speed,level)                                       \
    (_GPM_FUNC(GPIO_FUNC_SIO) | _GPM_OE | ((level) ? _GPM_OUT : 0))
#define GPO_opendrain(speed,level)                                      \
    (_GPM_FUNC(GPIO_FUNC_SIO) | _GPM_OD | ((level) ? 0 : _GPM_OE))
/* "Alternate function" output: FlashFloppy uses this solely for the
 * PIO-driven RDATA pin. */
#define _AFO_pushpull(speed,level) _GPM_FUNC(GPIO_FUNC_PIO0)
#define AFO_pushpull(speed) _GPM_FUNC(GPIO_FUNC_PIO0)
#define AFI(pupd) GPI_floating

void gpio_write_pin_(unsigned int pin, unsigned int level);
void gpio_write_pins_(uint32_t mask, unsigned int level);
#define gpio_write_pin(gpio, pin, level) gpio_write_pin_(pin, level)
#define gpio_write_pins(gpio, mask, level) gpio_write_pins_(mask, level)
#define gpio_read_pin(gpio, pin) gpio_get(pin)

/* GPIO interrupts (analogue of STM32 EXTI). All are delivered through the
 * single IO_IRQ_BANK0 vector; see pico2/floppy.c for the dispatcher.
 * The SDK clears any latched edge before enabling. */
#define gpio_irq_enable(pin, events) gpio_set_irq_enabled(pin, events, true)
void gpio_irq_disable_mask(uint32_t pin_mask);

/* On-board LED, used as the drive-activity indicator (pico2/board.c). */
void board_set_activity_led(bool_t on);

/* Reboot into the bootrom's UF2 loader (BOOTSEL mode). */
void rp2350_enter_bootsel(void) __attribute__((noreturn));

/* USB CDC-ACM device: debug console, and the Arduino-style "open the port
 * at 1200 baud to enter BOOTSEL" gesture. See pico2/usb_cdc.c. */
void usb_cdc_init(unsigned int irq_pri);
void usb_cdc_putc(char c);
void usb_cdc_kick(void);
void usb_cdc_flush_sync(void);
