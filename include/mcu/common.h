/*
 * common.h
 * 
 * Generic MCU interface: Cortex-M core (SysTick, NVIC), clocks, GPIO and
 * flash, as used by the FlashFloppy core. Implemented for the RP2350 in
 * mcu/rp2350.h and mcu_rp2350.c.
 * 
 * Written & released by Keir Fraser <keir.xen@gmail.com>
 * 
 * This is free and unencumbered software released into the public domain.
 * See the file COPYING for more details, or visit <http://unlicense.org>.
 */

/* C pointer types */
#define STK volatile struct stk * const
#define SCB volatile struct scb * const
#define NVIC volatile struct nvic * const
#define GPIO volatile struct gpio * const
#define SPI volatile struct spi * const

/* System */
void mcu_init(void);
void system_reset(void) __attribute__((noreturn));

/* Clocks */
#define SYSCLK     (SYSCLK_MHZ * 1000000)
#define sysclk_ns(x) (((x) * SYSCLK_MHZ) / 1000)
#define sysclk_us(x) ((x) * SYSCLK_MHZ)
#define sysclk_ms(x) ((x) * SYSCLK_MHZ * 1000)
#define sysclk_stk(x) ((x) * (SYSCLK_MHZ / STK_MHZ))

/* SysTick Timer */
/* SysTick counts the 1MHz tick generator (TICKS::PROC0). */
#define STK_MHZ    1
void delay_ticks(unsigned int ticks);
void delay_ns(unsigned int ns);
void delay_us(unsigned int us);
void delay_ms(unsigned int ms);

typedef uint32_t stk_time_t;
#define stk_now() (stk->val)
#define stk_diff(x,y) (((x)-(y)) & STK_MASK) /* d = y - x */
#define stk_add(x,d)  (((x)-(d)) & STK_MASK) /* y = x + d */
#define stk_sub(x,d)  (((x)+(d)) & STK_MASK) /* y = x - d */
#define stk_timesince(x) stk_diff(x,stk_now())

#define stk_us(x) ((x) * STK_MHZ)
#define stk_ms(x) stk_us((x) * 1000)
#define stk_sysclk(x) ((x) / (SYSCLK_MHZ / STK_MHZ))

/* NVIC */
#define IRQx_enable(x) do {                     \
    barrier();                                  \
    nvic->iser[(x)>>5] = 1u<<((x)&31);          \
} while (0)
#define IRQx_disable(x) do {                    \
    nvic->icer[(x)>>5] = 1u<<((x)&31);          \
    cpu_sync();                                 \
} while (0)
#define IRQx_is_enabled(x) ((nvic->iser[(x)>>5]>>((x)&31))&1)
#define IRQx_set_pending(x) (nvic->ispr[(x)>>5] = 1u<<((x)&31))
#define IRQx_clear_pending(x) (nvic->icpr[(x)>>5] = 1u<<((x)&31))
#define IRQx_is_pending(x) ((nvic->ispr[(x)>>5]>>((x)&31))&1)
#define IRQx_set_prio(x,y) (nvic->ipr[x] = (y) << 4)
#define IRQx_get_prio(x) (nvic->ipr[x] >> 4)

/* GPIO */
struct gpio;
void gpio_configure_pin(GPIO gpio, unsigned int pin, unsigned int mode);

/* FPEC */
void fpec_init(void);
void fpec_page_erase(uint32_t flash_address);
void fpec_write(const void *data, unsigned int size, uint32_t flash_address);

#define FLASH_PAGE_SIZE FLASH_SECTOR_SIZE /* QSPI flash erase unit (4kB) */
extern unsigned int ram_kb;
