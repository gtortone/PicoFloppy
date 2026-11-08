/*
 * common_regs.h
 * 
 * Cortex-M core register definitions (SysTick, SCB, NVIC).
 * 
 * Written & released by Keir Fraser <keir.xen@gmail.com>
 * 
 * This is free and unencumbered software released into the public domain.
 * See the file COPYING for more details, or visit <http://unlicense.org>.
 */

/* SysTick timer */
struct stk {
    uint32_t ctrl;     /* 00: Control and status */
    uint32_t load;     /* 04: Reload value */
    uint32_t val;      /* 08: Current value */
    uint32_t calib;    /* 0C: Calibration value */
};

#define STK_CTRL_COUNTFLAG  (1u<<16)
#define STK_CTRL_CLKSOURCE  (1u<< 2)
#define STK_CTRL_TICKINT    (1u<< 1)
#define STK_CTRL_ENABLE     (1u<< 0)

#define STK_MASK            ((1u<<24)-1)

#define STK_BASE 0xe000e010

/* System control block */
struct scb {
    uint32_t cpuid;    /* 00: CPUID base */
    uint32_t icsr;     /* 04: Interrupt control and state */
    uint32_t vtor;     /* 08: Vector table offset */
    uint32_t aircr;    /* 0C: Application interrupt and reset control */
    uint32_t scr;      /* 10: System control */
    uint32_t ccr;      /* 14: Configuration and control */
    uint32_t shpr1;    /* 18: System handler priority reg #1 */
    uint32_t shpr2;    /* 1C: system handler priority reg #2 */
    uint32_t shpr3;    /* 20: System handler priority reg #3 */
    uint32_t shcsr;    /* 24: System handler control and state */
    uint32_t cfsr;     /* 28: Configurable fault status */
    uint32_t hfsr;     /* 2C: Hard fault status */
    uint32_t _unused;  /* 30: - */
    uint32_t mmar;     /* 34: Memory management fault address */
    uint32_t bfar;     /* 38: Bus fault address */
};

#define SCB_CCR_STKALIGN       (1u<<9)
#define SCB_CCR_BFHFNMIGN      (1u<<8)
#define SCB_CCR_DIV_0_TRP      (1u<<4)
#define SCB_CCR_UNALIGN_TRP    (1u<<3)
#define SCB_CCR_USERSETMPEND   (1u<<1)
#define SCB_CCR_NONBASETHRDENA (1u<<0)

#define SCB_SHCSR_USGFAULTENA    (1u<<18)
#define SCB_SHCSR_BUSFAULTENA    (1u<<17)
#define SCB_SHCSR_MEMFAULTENA    (1u<<16)
#define SCB_SHCSR_SVCALLPENDED   (1u<<15)
#define SCB_SHCSR_BUSFAULTPENDED (1u<<14)
#define SCB_SHCSR_MEMFAULTPENDED (1u<<13)
#define SCB_SHCSR_USGFAULTPENDED (1u<<12)
#define SCB_SHCSR_SYSTICKACT     (1u<<11)
#define SCB_SHCSR_PENDSVACT      (1u<<10)
#define SCB_SHCSR_MONITORACT     (1u<< 8)
#define SCB_SHCSR_SVCALLACT      (1u<< 7)
#define SCB_SHCSR_USGFAULTACT    (1u<< 3)
#define SCB_SHCSR_BUSFAULTACT    (1u<< 1)
#define SCB_SHCSR_MEMFAULTACT    (1u<< 0)

#define SCB_CFSR_DIVBYZERO     (1u<<25)
#define SCB_CFSR_UNALIGNED     (1u<<24)
#define SCB_CFSR_NOCP          (1u<<19)
#define SCB_CFSR_INVPC         (1u<<18)
#define SCB_CFSR_INVSTATE      (1u<<17)
#define SCB_CFSR_UNDEFINSTR    (1u<<16)
#define SCB_CFSR_BFARVALID     (1u<<15)
#define SCB_CFSR_STKERR        (1u<<12)
#define SCB_CFSR_UNSTKERR      (1u<<11)
#define SCB_CFSR_IMPRECISERR   (1u<<10)
#define SCB_CFSR_PRECISERR     (1u<< 9)
#define SCB_CFSR_IBUSERR       (1u<< 8)
#define SCB_CFSR_MMARVALID     (1u<< 7)
#define SCB_CFSR_MSTKERR       (1u<< 4)
#define SCB_CFSR_MUNSTKERR     (1u<< 3)
#define SCB_CFSR_DACCVIOL      (1u<< 1)
#define SCB_CFSR_IACCVIOL      (1u<< 0)

#define SCB_AIRCR_VECTKEY     (0x05fau<<16)
#define SCB_AIRCR_SYSRESETREQ (1u<<2)

#define SCB_BASE 0xe000ed00

/* Nested vectored interrupt controller */
struct nvic {
    uint32_t iser[32]; /*  00: Interrupt set-enable */
    uint32_t icer[32]; /*  80: Interrupt clear-enable */
    uint32_t ispr[32]; /* 100: Interrupt set-pending */
    uint32_t icpr[32]; /* 180: Interrupt clear-pending */
    uint32_t iabr[64]; /* 200: Interrupt active */
    uint8_t ipr[100];  /* 300: Interrupt priority */
};

#define NVIC_BASE 0xe000e100
