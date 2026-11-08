/*
 * cortex.c
 * 
 *  ARM Cortex-M33 management.
 * 
 * Written & released by Keir Fraser <keir.xen@gmail.com>
 * 
 * This is free and unencumbered software released into the public domain.
 * See the file COPYING for more details, or visit <http://unlicense.org>.
 */

struct extra_exception_frame {
    uint32_t r4, r5, r6, r7, r8, r9, r10, r11, lr;
};

/* Interrupt/exception stack. Thread mode runs on the Process stack (the
 * pico-sdk's main stack in SCRATCH_Y); exception_init() moves the Main
 * stack here for handlers. 2kB: USB (TinyUSB) runs in handler mode. */
#define IRQ_STACK_SIZE 2048
uint32_t _irq_stackbottom[IRQ_STACK_SIZE/4] aligned(8);
asm (".global _irq_stacktop\n"
     ".set _irq_stacktop, _irq_stackbottom + " STR(IRQ_STACK_SIZE));

static inline bool_t in_text(uint32_t p)
{
    return (p >= (uint32_t)_smaintext) && (p < (uint32_t)_emaintext);
}

static void show_stack(uint32_t sp, uint32_t top)
{
    unsigned int i = 0;
    sp &= ~3;
    while (sp < top) {
        uint32_t v = *(uint32_t *)sp & ~1;
        sp += 4;
        if (!in_text(v))
            continue;
        if ((i++&7) == 0)
            printk("\n ", sp);
        printk("%x ", v);
    }
    printk("\n");
}

void EXC_unexpected(struct extra_exception_frame *extra)
{
    struct exception_frame *frame;
    uint8_t exc = (uint8_t)read_special(psr);
    uint32_t msp, psp;

    if (extra->lr & 4) {
        frame = (struct exception_frame *)read_special(psp);
        psp = (uint32_t)(frame + 1);
        msp = (uint32_t)(extra + 1);
    } else {
        frame = (struct exception_frame *)(extra + 1);
        psp = read_special(psp);
        msp = (uint32_t)(frame + 1);
    }

    printk("Unexpected %s #%u at PC=%08x (%s):\n",
           (exc < 16) ? "Exception" : "IRQ",
           (exc < 16) ? exc : exc - 16,
           frame->pc, (extra->lr & 8) ? "Thread" : "Handler");
    printk(" r0:  %08x   r1:  %08x   r2:  %08x   r3:  %08x\n",
           frame->r0, frame->r1, frame->r2, frame->r3);
    printk(" r4:  %08x   r5:  %08x   r6:  %08x   r7:  %08x\n",
           extra->r4, extra->r5, extra->r6, extra->r7);
    printk(" r8:  %08x   r9:  %08x   r10: %08x   r11: %08x\n",
           extra->r8, extra->r9, extra->r10, extra->r11);
    printk(" r12: %08x   sp:  %08x   lr:  %08x   pc:  %08x\n",
           frame->r12, (extra->lr & 4) ? psp : msp, frame->lr, frame->pc);
    printk(" msp: %08x   psp: %08x   psr: %08x\n",
           msp, psp, frame->psr);
    
    if ((msp >= (uint32_t)_irq_stackbottom)
        && (msp < (uint32_t)_irq_stacktop)) {
        printk("IRQ Call Trace:");
        show_stack(msp, (uint32_t)_irq_stacktop);
    }

    if ((psp >= (uint32_t)_thread_stackbottom)
        && (psp < (uint32_t)_thread_stacktop)) {
        printk("Process Call Trace:", psp);
        show_stack(psp, (uint32_t)_thread_stacktop);
    }

    system_reset();
}

/* Fault entry: save the callee-saved registers and LR alongside the
 * hardware-stacked frame, then dump state via EXC_unexpected(). Naked, so
 * that no register is touched before it is saved. */
__attribute__((naked))
void EXC_unused(void)
{
    asm (
        "    push {r4, r5, r6, r7, r8, r9, r10, r11, lr}\n"
        "    mov  r0, sp\n"
        "    b    EXC_unexpected\n"
        );
}

/* The pico-sdk vector table (crt0.S) names its fault handlers as weak
 * symbols, which default to a breakpoint: route them all to the crash
 * dump instead. */
void isr_nmi(void) __attribute__((alias("EXC_unused")));
void isr_hardfault(void) __attribute__((alias("EXC_unused")));
void isr_memmanage(void) __attribute__((alias("EXC_unused")));
void isr_busfault(void) __attribute__((alias("EXC_unused")));
void isr_usagefault(void) __attribute__((alias("EXC_unused")));
void isr_securefault(void) __attribute__((alias("EXC_unused")));

static void exception_init(void)
{
    /* Initialise and switch to Process SP. Explicit asm as must be
     * atomic wrt updates to SP. We can't guarantee that in C. */
    asm volatile (
        "    mrs  r1,msp     \n"
        "    msr  psp,r1     \n" /* Set up Process SP    */
        "    movs r1,%0      \n"
        "    msr  control,r1 \n" /* Switch to Process SP */
        "    isb             \n" /* Flush the pipeline   */
        :: "i" (CONTROL_SPSEL) : "r1" );

    /* Set up Main SP for IRQ/Exception context. */
    write_special(msp, _irq_stacktop);

    /* Initialise interrupts and exceptions. */
    /* The vector table belongs to the pico-sdk runtime (VTOR is already
     * set). The FPU is enabled by the SDK, but FlashFloppy never uses it:
     * disable FP context stacking, so that every exception frame is the
     * basic 8-word frame which cancel_call() knows how to rewrite. */
    m33_hw->fpccr &= ~(M33_FPCCR_ASPEN_BITS | M33_FPCCR_LSPEN_BITS);
    scb->ccr |= SCB_CCR_STKALIGN | SCB_CCR_DIV_0_TRP;
    /* GCC inlines memcpy() using full-word load/store regardless of buffer
     * alignment. Hence it is unsafe to trap on unaligned accesses. */
    /*scb->ccr |= SCB_CCR_UNALIGN_TRP;*/
    scb->shcsr |= (SCB_SHCSR_USGFAULTENA |
                   SCB_SHCSR_BUSFAULTENA |
                   SCB_SHCSR_MEMFAULTENA);

    /* SVCall/PendSV exceptions have lowest priority. */
    scb->shpr2 = 0xff<<24;
    scb->shpr3 = 0xff<<16;
}

static void sysclk_init(void)
{
    /* Enable SysTick counter. */
    stk->load = STK_MASK;
    stk->ctrl = STK_CTRL_ENABLE;
}

void cortex_init(void)
{
    exception_init();
    cpu_sync();
    sysclk_init();
}

void delay_ticks(unsigned int ticks)
{
    unsigned int diff, cur, prev = stk->val;

    for (;;) {
        cur = stk->val;
        diff = (prev - cur) & STK_MASK;
        if (ticks <= diff)
            break;
        ticks -= diff;
        prev = cur;
    }
}

void delay_ns(unsigned int ns)
{
    delay_ticks((ns * STK_MHZ) / 1000u);
}

void delay_us(unsigned int us)
{
    delay_ticks(us * STK_MHZ);
}

void delay_ms(unsigned int ms)
{
    delay_ticks(ms * 1000u * STK_MHZ);
}

void system_reset(void)
{
    console_sync();
    printk("Resetting...\n");
    /* Full-chip reset via the watchdog: SYSRESETREQ resets only the core
     * domain on RP2350. */
    watchdog_reboot(0, 0, 0);
    for (;;) ;
}
