/* B310E OS — threads, scheduler, tick.
 *
 * Ready threads sit in one circular list per priority; a bitmap of
 * non-empty lists and CLZ find the highest at once. The running thread
 * is always the head of its list: a yield or an expired time slice moves
 * the head on, a wake-up appends at the tail. A thread that becomes ready
 * with a higher priority than the running one preempts it: at once from
 * thread context (svc), at interrupt exit otherwise.
 *
 * Interrupts are the only lock: every list operation runs with IRQs off,
 * and the scheduler itself runs in SVC or IRQ mode with IRQs off.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/arch.h"
#include "os/kernel.h"
#include "os/lib.h"
#include "kernel/internal.h"

thread_t *kernel_current;

static thread_t *ready[32];
static uint32_t ready_map;
static thread_t *timeouts;          /* sorted by wake tick */
static thread_t *zombies;
static thread_t *idle_thread;
static thread_t *all_threads;
static uint32_t total_cycles;
static volatile uint32_t ticks;
static bool running;
static bool need_resched;
static bool rotate;                 /* yield: move the running thread on */
static unsigned slice_left;
static uint32_t acct_mark;          /* timer_cycles() at the last accounting */
static uint32_t idle_window_cycles, idle_permille;
static uint32_t tick_mark;          /* timer_cycles() at the last whole tick */
static uint32_t next_window, next_stats;
static bool tickless = true;
/* The timer's period: the tick; a tickless sleep; or the rest of a tick,
 * after a sleep, so the tick lines up on the tick boundary again. */
static enum { PHASE_TICK, PHASE_SLEEP, PHASE_ALIGN } phase;

#define MAX_SLEEP_TICKS 1000        /* a tickless sleep, at most */

#define SLICE_TICKS 10
#define FRAME_WORDS 17

/* ---- lists (interrupts off) --------------------------------------------- */

static void ready_add(thread_t *t, bool head)
{
    thread_t *h = ready[t->prio];

    t->state = T_READY;
    if (!h) {
        t->next = t->prev = t;
        ready[t->prio] = t;
        ready_map |= 1u << t->prio;
        return;
    }
    t->prev = h->prev;
    t->next = h;
    h->prev->next = t;
    h->prev = t;
    if (head)
        ready[t->prio] = t;
}

static void ready_remove(thread_t *t)
{
    if (t->next == t) {
        ready[t->prio] = NULL;
        ready_map &= ~(1u << t->prio);
    } else {
        t->prev->next = t->next;
        t->next->prev = t->prev;
        if (ready[t->prio] == t)
            ready[t->prio] = t->next;
    }
    t->next = t->prev = NULL;
}

static int highest_ready(void)
{
    return 31 - __builtin_clz(ready_map);
}

static void waitq_add(waitq_t *q, thread_t *t)
{
    thread_t **pp = &q->head;

    while (*pp && (*pp)->prio >= t->prio)
        pp = &(*pp)->next;
    t->next = *pp;
    *pp = t;
    t->waitq = q;
}

static void waitq_remove(thread_t *t)
{
    thread_t **pp = &t->waitq->head;

    while (*pp && *pp != t)
        pp = &(*pp)->next;
    if (*pp)
        *pp = t->next;
    t->next = NULL;
    t->waitq = NULL;
}

static void timeout_add(thread_t *t, uint32_t ms)
{
    thread_t **pp = &timeouts;

    t->wake = ticks + (ms ? ms : 1);
    while (*pp && (int32_t)((*pp)->wake - t->wake) <= 0)
        pp = &(*pp)->tnext;
    t->tnext = *pp;
    *pp = t;
    t->timed = 1;
}

static void timeout_remove(thread_t *t)
{
    thread_t **pp = &timeouts;

    while (*pp && *pp != t)
        pp = &(*pp)->tnext;
    if (*pp)
        *pp = t->tnext;
    t->tnext = NULL;
    t->timed = 0;
}

/* ---- blocking and waking (interrupts off) -------------------------------- */

int k_block(waitq_t *q, uint32_t timeout_ms)
{
    thread_t *t = kernel_current;

    ready_remove(t);
    t->state = T_BLOCKED;
    t->result = WAIT_OK;
    if (q)
        waitq_add(q, t);
    if (timeout_ms != WAIT_FOREVER)
        timeout_add(t, timeout_ms);
    arch_yield();               /* back here once woken */
    return t->result;
}

void k_wake(thread_t *t, int result)
{
    if (t->waitq)
        waitq_remove(t);
    if (t->timed)
        timeout_remove(t);
    t->result = (int8_t)result;
    ready_add(t, false);
    if (kernel_current && t->prio > kernel_current->prio)
        need_resched = true;
}

void k_preempt(void)
{
    if (!running)
        return;
    if (ready_map && highest_ready() > kernel_current->prio)
        need_resched = true;
    if (need_resched && !arch_in_irq())
        arch_yield();
}

/* Change a thread's effective priority wherever it is queued. */
void k_set_prio(thread_t *t, int prio)
{
    if (t->prio == prio)
        return;
    if (t->state == T_READY) {
        bool head = ready[t->prio] == t;
        ready_remove(t);
        t->prio = (uint8_t)prio;
        ready_add(t, head && t == kernel_current);
    } else if (t->state == T_BLOCKED && t->waitq) {
        waitq_t *q = t->waitq;
        waitq_remove(t);
        t->prio = (uint8_t)prio;
        waitq_add(q, t);
    } else {
        t->prio = (uint8_t)prio;
    }
    if (t->state == T_BLOCKED && t->blocked_on)
        k_mutex_inherit(t->blocked_on, prio);
    if (!kernel_current)
        return;
    if (t == kernel_current && ready_map && highest_ready() > t->prio)
        need_resched = true;
    else if (t != kernel_current && t->state == T_READY &&
             t->prio > kernel_current->prio)
        need_resched = true;
}

/* ---- scheduler ----------------------------------------------------------- */

static void account(void)
{
    uint32_t now = timer_cycles();
    int32_t d = (int32_t)(now - acct_mark);

    if (d <= 0)                         /* never count backwards */
        return;
    kernel_current->cycles += (uint32_t)d;
    total_cycles += (uint32_t)d;
    acct_mark = now;
}

void kernel_reschedule(void)
{
    thread_t *cur = kernel_current, *next;

    if (rotate && cur->state == T_READY && ready[cur->prio] == cur)
        ready[cur->prio] = cur->next;
    rotate = false;
    need_resched = false;
    next = ready[highest_ready()];
    if (next != cur) {
        account();
        slice_left = SLICE_TICKS;
        kernel_current = next;
    }
}

void kernel_irq_exit(void)
{
    if (need_resched)
        kernel_reschedule();
}

/* Ticks follow the timer's cycle count, so a tickless sleep advances them
 * by however many periods it lasted. Interrupts off. */
static void advance(void)
{
    thread_t *cur = kernel_current;
    uint32_t p = timer_load();
    int32_t since = (int32_t)(timer_cycles() - tick_mark);
    uint32_t n;

    if (since < (int32_t)p)             /* not a tick yet, or a read behind the
                                         * mark: never 2^32 cycles ahead */
        return;
    n = (uint32_t)since / p;
    tick_mark += n * p;
    ticks += n;
    account();
    while (timeouts && (int32_t)(ticks - timeouts->wake) >= 0)
        k_wake(timeouts, WAIT_TIMEOUT);

    if (slice_left) {
        slice_left = slice_left > n ? slice_left - n : 0;
        if (!slice_left) {
            slice_left = SLICE_TICKS;
            if (cur->state == T_READY && cur->next != cur) {
                ready[cur->prio] = cur->next;
                need_resched = true;
            }
        }
    }

    if ((int32_t)(ticks - next_window) >= 0) {  /* the clock governor's window */
        static uint32_t win_idle, win_total;
        uint32_t idle = idle_thread->cycles - win_idle;
        uint32_t all = total_cycles - win_total;

        next_window = ticks + 50;
        win_idle = idle_thread->cycles;
        win_total = total_cycles;
        if (all >= 1000)
            cpufreq_window(idle >= all ? 0 : 1000 - idle / (all / 1000));
    }

    if ((int32_t)(ticks - next_stats) >= 0) {   /* idle share of the cycles counted */
        static uint32_t total_window;
        uint32_t idle = idle_thread->cycles - idle_window_cycles;
        uint32_t all = total_cycles - total_window;

        idle_permille = all >= 1000 ? idle / (all / 1000) : 0;
        if (idle_permille > 1000)
            idle_permille = 1000;
        next_stats = ticks + 1000;
        idle_window_cycles = idle_thread->cycles;
        total_window = total_cycles;
    }
}

/* The rest of the current tick as the next period. */
static void align(void)
{
    /* signed: advance() can take longer than the rest of the tick (many
     * wake-ups, 26 MHz), and a negative load is a 165 s period. A tick
     * skipped here is caught up by the next interrupt's advance(). */
    int32_t p = (int32_t)timer_load();
    int32_t rest = p - (int32_t)(timer_cycles() - tick_mark);

    while (rest < 260)
        rest += p;
    timer_restart((uint32_t)rest);
    phase = PHASE_ALIGN;
}

/* The timer's interrupt: whole ticks. A sleep's end goes to the rest of
 * the tick, that one's end back to the plain tick (the reload repeats the
 * last load, so the tick has to be loaded again). */
void kernel_tick(void)
{
    advance();
    if (phase == PHASE_SLEEP) {
        align();
    } else if (phase == PHASE_ALIGN) {
        timer_restart(timer_load());
        phase = PHASE_TICK;
    }
}

/* Any interrupt: ending a tickless sleep early, the ticks catch up first,
 * so handlers and woken threads see the time, and the tick comes back for
 * the threads that run now. */
void kernel_irq_entry(void)
{
    if (phase == PHASE_SLEEP) {
        advance();
        align();
    }
}

void kernel_set_tickless(bool on)
{
    tickless = on;
}

thread_t *thread_first(void)
{
    return all_threads;
}

thread_t *thread_next(thread_t *t)
{
    return t->all_next;
}

uint32_t kernel_cycles(void)
{
    return total_cycles;
}

uint32_t kernel_ticks(void)
{
    return ticks;
}

bool kernel_running(void)
{
    return running;
}

uint32_t kernel_idle_permille(void)
{
    return idle_permille;
}

/* ---- threads ------------------------------------------------------------- */

thread_t *thread_create(const char *name, int prio, size_t stack_size,
                        thread_fn fn, void *arg)
{
    thread_t *t = kzalloc(sizeof(*t));
    uint32_t *stack, *sp, s;
    size_t i;

    stack_size = (stack_size + 7) & ~7u;
    stack = kmalloc(stack_size);
    if (!t || !stack) {
        kfree(t);
        kfree(stack);
        return NULL;
    }
    for (i = 0; i < stack_size / 4; i++)
        stack[i] = 0x57a57a57;      /* high-water mark */

    sp = stack + stack_size / 4 - FRAME_WORDS;
    sp[0] = 0x1f;                   /* SYS mode, IRQ on, ARM */
    sp[1] = (uint32_t)arg;          /* r0 */
    sp[5] = (uint32_t)fn;           /* r4 */
    sp[14] = (uint32_t)(sp + FRAME_WORDS);   /* sp */
    sp[15] = 0;                     /* lr */
    sp[16] = (uint32_t)arch_thread_trampoline;

    t->sp = sp;
    t->name = name;
    t->prio = t->base_prio = (uint8_t)(prio < 0 ? 0 : prio > PRIO_MAX ? PRIO_MAX : prio);
    t->stack = stack;
    t->stack_size = stack_size;

    s = arch_irq_save();
    t->all_next = all_threads;
    all_threads = t;
    ready_add(t, false);
    k_preempt();
    arch_irq_restore(s);
    return t;
}

void thread_sleep(uint32_t ms)
{
    uint32_t s = arch_irq_save();

    k_block(NULL, ms);
    arch_irq_restore(s);
}

void thread_yield(void)
{
    uint32_t s = arch_irq_save();

    rotate = true;
    arch_yield();
    arch_irq_restore(s);
}

void thread_exit(void)
{
    thread_t *t = kernel_current;

    arch_irq_save();
    ready_remove(t);
    t->state = T_DEAD;
    t->next = zombies;
    zombies = t;
    arch_yield();
    for (;;)
        ;
}

thread_t *thread_self(void)
{
    return kernel_current;
}

int thread_count_owned(const void *owner)
{
    uint32_t s = arch_irq_save();
    int n = 0;

    for (thread_t *t = all_threads; t; t = t->all_next)
        if (t->owner == owner && t->state != T_DEAD)
            n++;
    arch_irq_restore(s);
    return n;
}

void thread_set_priority(thread_t *t, int prio)
{
    uint32_t s = arch_irq_save();

    t->base_prio = (uint8_t)prio;
    k_set_prio(t, k_mutex_ceiling(t));
    k_preempt();
    arch_irq_restore(s);
}

/* Nothing ready but the idle thread: when the next timeout is at least
 * three ticks away, let the timer run until it (a second at most) instead
 * of interrupting every millisecond. Interrupts off. */
static void tickless_sleep(void)
{
    uint32_t delta = MAX_SLEEP_TICKS;
    int32_t rest;

    /* from the align phase too: a thread woken by a sleep's end mostly runs
     * for well under a tick, and waiting for the aligning period first
     * would double the interrupts of every wake-up */
    if (!tickless || phase == PHASE_SLEEP)
        return;
    advance();                          /* a tick may be pending: be current */
    if (need_resched)                   /* that woke a thread: it runs first, */
        return;                         /* and may add a nearer timeout */
    if (timeouts) {
        int32_t d = (int32_t)(timeouts->wake - ticks);

        delta = d < 1 ? 1 : (uint32_t)d < MAX_SLEEP_TICKS ? (uint32_t)d : MAX_SLEEP_TICKS;
    }
    rest = (int32_t)(tick_mark + delta * timer_load() - timer_cycles());
    if (delta >= 3 && rest >= 2 * (int32_t)timer_load()) {
        timer_restart((uint32_t)rest);
        phase = PHASE_SLEEP;
    }
}

/* Lowest priority: free finished threads, then sleep until an interrupt. */
static void idle_main(void *arg)
{
    for (;;) {
        uint32_t s = arch_irq_save();
        thread_t *z = zombies;

        zombies = NULL;
        arch_irq_restore(s);
        while (z) {
            thread_t *n = z->next;
            s = arch_irq_save();
            for (thread_t **pp = &all_threads; *pp; pp = &(*pp)->all_next)
                if (*pp == z) {
                    *pp = z->all_next;
                    break;
                }
            arch_irq_restore(s);
            kfree(z->stack);
            kfree(z);
            z = n;
        }
        s = arch_irq_save();            /* decide and wait with no interrupt
                                         * in between (WFI still wakes) */
        if (!need_resched)
            tickless_sleep();
        /* tickless_sleep() catches the ticks up first, which can wake a
         * thread whose timeout just passed: run it now, not after the long
         * sleep it would otherwise wait out (the END key's 20 ms poll once
         * slept 496 ms, and short presses were lost) */
        if (need_resched)
            k_preempt();
        else
            arch_wait_for_irq();
        arch_irq_restore(s);
    }
}

void kernel_start(void)
{
    idle_thread = thread_create("idle", PRIO_IDLE, 1024, idle_main, NULL);
    running = true;
    kernel_current = ready[highest_ready()];
    slice_left = SLICE_TICKS;
    acct_mark = tick_mark = timer_cycles();
    next_window = 50;
    next_stats = 1000;
    arch_start_first();
}
