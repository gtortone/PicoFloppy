/*
 * timer.c
 * 
 * Deadline-based timer callbacks.
 * 
 * Written & released by Keir Fraser <keir.xen@gmail.com>
 * 
 * This is free and unencumbered software released into the public domain.
 * See the file COPYING for more details, or visit <http://unlicense.org>.
 */

/* TIMER0 alarm 0 (claimed from the pico-sdk in timers_init()). */
#define TIMER_ALARM 0
#define TIMER_IRQ RP_TIMER0_IRQ_0
DEFINE_IRQ(TIMER_IRQ, "IRQ_timer");


/* TIMER0 counts the 1MHz tick (== TIME_MHZ); alarms match on the low
 * 32 bits of the count. */
#define SLACK_TICKS 2

static void reprogram_timer(int32_t delta)
{
    if (delta < (SLACK_TICKS + 1))
        delta = SLACK_TICKS + 1;
    timer0_hw->alarm[TIMER_ALARM] = timer0_hw->timerawl + delta - SLACK_TICKS;
}


#define TIMER_INACTIVE ((struct timer *)1ul)

static struct timer *head;

void timer_init(struct timer *timer, void (*cb_fn)(void *), void *cb_dat)
{
    timer->cb_fn = cb_fn;
    timer->cb_dat = cb_dat;
    timer->next = TIMER_INACTIVE;
}

static bool_t timer_is_active(struct timer *timer)
{
    return timer->next != TIMER_INACTIVE;
}

static void _timer_cancel(struct timer *timer)
{
    struct timer *t, **pprev;

    if (!timer_is_active(timer))
        return;

    for (pprev = &head; (t = *pprev) != timer; pprev = &t->next)
        continue;

    *pprev = t->next;
    t->next = TIMER_INACTIVE;
}

void timer_set(struct timer *timer, time_t deadline)
{
    struct timer *t, **pprev;
    time_t now;
    int32_t delta;
    uint32_t oldpri;

    oldpri = IRQ_save(TIMER_IRQ_PRI);

    _timer_cancel(timer);

    timer->deadline = deadline;

    now = time_now();
    delta = time_diff(now, deadline);
    for (pprev = &head; (t = *pprev) != NULL; pprev = &t->next)
        if (delta <= time_diff(now, t->deadline))
            break;
    timer->next = *pprev;
    *pprev = timer;

    if (head == timer)
        reprogram_timer(delta);

    IRQ_restore(oldpri);
}

void timer_cancel(struct timer *timer)
{
    uint32_t oldpri;
    oldpri = IRQ_save(TIMER_IRQ_PRI);
    _timer_cancel(timer);
    IRQ_restore(oldpri);
}

void timers_init(void)
{
    timer_hardware_alarm_claim(timer0_hw, TIMER_ALARM);
    hw_set_bits(&timer0_hw->inte, m(TIMER_ALARM));
    IRQx_set_prio(TIMER_IRQ, TIMER_IRQ_PRI);
    IRQx_enable(TIMER_IRQ);
}

static void IRQ_timer(void)
{
    struct timer *t;
    int32_t delta;

    timer0_hw->intr = m(TIMER_ALARM); /* clear the alarm interrupt */

    while ((t = head) != NULL) {
        if ((delta = time_diff(time_now(), t->deadline)) > SLACK_TICKS) {
            reprogram_timer(delta);
            break;
        }
        head = t->next;
        t->next = TIMER_INACTIVE;
        (*t->cb_fn)(t->cb_dat);
    }
}
