/*
 * B310E-OS — kernel/sched.c
 *
 * Cooperative round-robin scheduler. Original implementation.
 *
 * Every task runs on its own stack buffer (allocated from the kernel bump
 * pool). ctx_switch() — arch/ctx.s on the ARM926, kernel/ctx_host.S on the
 * host — saves the current CPU state on the current stack, stores the
 * stack pointer, loads the next task's stack pointer and resumes it, so a
 * suspended task continues exactly where it yielded.
 *
 * First run: task_create() lays a fake "just-switched-in" frame on the
 * task's stack whose return address is task_launcher(). The first
 * ctx_switch() into the task therefore "returns" into task_launcher(),
 * which calls fn() and then task_exit().
 *
 * Scheduling is O(n) over the fixed task table (< 32 tasks) — fine for a
 * feature-phone kernel.
 */

#include "sched.h"

#include "malloc.h"
#include "irq.h"
#include "printk.h"
#include "../arch/chip.h"   /* MEM4 (tick ISR timer clear) */

/* ---- task table -------------------------------------------------------- */

static task_t s_tasks[SCHED_MAX_TASKS];
static uint32_t s_task_count;        /* number of created tasks */
static int s_current = -1;           /* index of running task, -1 = idle */
static uintptr_t s_sched_sp;         /* scheduler's own saved SP */

/* ---- preemptive tick (1 ms sys timer) ---------------------------------- */

static volatile uint32_t s_ticks;    /* 1 ms tick counter (Rockbox tick) */
static volatile int s_need_resched;  /* set by the tick ISR */

uint32_t sched_ticks(void)
{
    return s_ticks;
}

/* The 1 ms timer ISR (overrides irq.c's weak default). Runs in IRQ mode
 * on the IRQ stack — MUST NOT call ctx_switch here (the interrupted task's
 * context is on the IRQ stack, not a switchable frame). It only sets the
 * need_resched flag; task_yield() performs the actual switch at the next
 * safe point (deferred preemption — the plan's recommended design).
 *
 * Timer re-arm is fpdoom's irq_handler verbatim (syscode.c:982-988): only
 * when the timer's own pending bit (int-reg bit 2) is set, write 9 (bit3
 * clear + bit0 en) — the SC6530 timer keeps running from its load register,
 * so the interrupt just needs acknowledging at the peripheral. NOTE the
 * INTC itself is never touched here (see kernel/irq.c irq_handler: the
 * 0x8000000C "clear" is actually INT_DISABLE and MASKED line 23 — the tick
 * fired once then stopped until that write was removed). */
#ifndef HOST_TEST
void sys_tick_isr(void)
{
    s_ticks++;
    s_need_resched = 1;
    if (MEM4(0x81000040u + 0xc) & 4u)   /* timer int pending bit */
        MEM4(0x81000040u + 0xc) = 9;    /* clear + keep enabled */
}
#endif

/* ---- helpers ----------------------------------------------------------- */

/* Next READY task after s_current (wraps; never the current task). */
static int sched_pick_next(void)
{
    if (s_task_count == 0)
        return -1;

    int start = s_current;
    for (uint32_t step = 1; step <= s_task_count; step++) {
        int idx = (start + (int)step) % (int)s_task_count;
        if (idx != s_current && s_tasks[idx].state == TASK_READY)
            return idx;
    }
    return -1;
}

/* ---- v2 preemptive tick: the parked-frame IRQ-return switch ------------- */

/* Staged by sched_preempt_check() (called from the IRQ-return path in
 * arch/vectors.s, IRQ mode) — the cur/next sp pointers for the parked-
 * frame dance. The stub reads them and calls ctx_preempt_switch. */
uintptr_t *s_preempt_cur_sp;
uintptr_t *s_preempt_next_sp;

/* Called from the IRQ stub on EVERY IRQ return. If the tick set
 * s_need_resched and another task is READY: pick it, clear the flag,
 * stage the cur/next sp pointers, mark it RUNNING and return its index —
 * the stub then runs ctx_preempt_switch (the dance) which never returns.
 * Returns -1 to continue the interrupted task (normal IRQ return). */
int sched_preempt_check(void)
{
    int idx;

    if (!s_need_resched)
        return -1;
    idx = sched_pick_next();
    if (idx < 0)
        return -1;
    s_need_resched = 0;
    s_preempt_cur_sp = (s_current >= 0) ? &s_tasks[s_current].sp
                                        : &s_sched_sp;
    s_preempt_next_sp = &s_tasks[idx].sp;
    s_tasks[idx].state = TASK_RUNNING;
    s_current = idx;
    return idx;
}

#if defined(HOST_TEST)
/* Host-test hook: pretend a tick fired (no real IRQ on the host). */
void sched_test_force_preempt(void)
{
    s_need_resched = 1;
}
#endif

/* Shared task body: run fn, then exit the task. First reached through the
 * fake frame in the task's stack. */
static void task_launcher(void)
{
    task_t *t = sched_current();
    if (t != NULL) {
        t->fn(t->arg);
        task_exit();                 /* switches away; never returns while
                                        another task is READY */
    }
}

#if defined(HOST_TEST)
/* x86-64 (Windows/mingw) initial frame, matching the pop sequence of
 * kernel/ctx_host.S ctx_switch(): 8 saved registers + return address
 * (9 slots of 8 bytes). The task's entry RSP must be 8 mod 16 (ABI). */
static void ctx_start_frame(task_t *t, void (*entry)(void))
{
    uintptr_t base = (uintptr_t)t->stack + (uintptr_t)t->stack_words * 4u;
    uintptr_t top = ((base - 8u) & ~(uintptr_t)15u) + 8u;
    uintptr_t *p = (uintptr_t *)(top - 9u * sizeof(uintptr_t));

    for (int i = 0; i < 8; i++)     /* rbp, rbx, rdi, rsi, r12..r15 */
        p[i] = 0;
    p[8] = (uintptr_t)entry;
    t->sp = (uintptr_t)p;
}
#else
/* ARM (Thumb-1) initial frame, matching the pop sequence of arch/ctx.s:
 *   [r12][r10][r11][r8][r9][r4][r5][r6][r7][pc]  (10 words)
 * Top of stack stays 8-aligned for the EABI. (The v2 16-word unified
 * frame was REVERTED 2026-08-21 — it crashed on hardware, see learnings.)
 */
static void ctx_start_frame(task_t *t, void (*entry)(void))
{
    uint8_t *base = (uint8_t *)t->stack + (uintptr_t)t->stack_words * 4u;
    uint32_t *top = (uint32_t *)((uintptr_t)base & ~(uintptr_t)7u);
    uint32_t *p = top - 10;

    p[0] = 0;                        /* r12 */
    p[1] = 0;                        /* r10 */
    p[2] = 0;                        /* r11 */
    p[3] = 0;                        /* r8  */
    p[4] = 0;                        /* r9  */
    p[5] = 0;                        /* r4  */
    p[6] = 0;                        /* r5  */
    p[7] = 0;                        /* r6  */
    p[8] = 0;                        /* r7  */
    p[9] = (uint32_t)(uintptr_t)entry | 1u;   /* pc, Thumb bit set */

    t->sp = (uintptr_t)p;
}
#endif

/* ---- public API -------------------------------------------------------- */

int task_create(const char *name, void (*fn)(void *), void *arg,
                uint32_t stack_words)
{
    if (name == NULL || fn == NULL)
        return -1;
    if (s_task_count >= SCHED_MAX_TASKS)
        return -1;
    if (stack_words == 0)
        stack_words = SCHED_DEFAULT_STACK_WORDS;
    if (stack_words < 16u)           /* keep the fake frame well inside */
        return -1;

    task_t *t = &s_tasks[s_task_count];

    t->stack = kmalloc(stack_words * sizeof(uint32_t));
    if (t->stack == NULL)
        return -1;

    t->name = name;
    t->fn = fn;
    t->arg = arg;
    t->state = TASK_READY;
    t->stack_words = stack_words;
    ctx_start_frame(t, task_launcher);

    s_task_count++;
    return (int)s_task_count;        /* task id = index + 1 */
}

void task_yield(void)
{
    if (s_task_count == 0 || s_current < 0)
        return;

    task_t *cur = &s_tasks[s_current];
    if (cur->state == TASK_RUNNING)
        cur->state = TASK_READY;

    int idx = sched_pick_next();
    if (idx < 0) {
        /* No other READY task (e.g. the only task, or all others DONE):
         * keep running. */
        if (cur->state == TASK_READY)
            cur->state = TASK_RUNNING;
        s_need_resched = 0;
        return;
    }

    /* Deferred preemption: a pending tick forces the switch now. IRQs are
     * masked around ctx_switch so the timer can never interrupt a
     * task-switch mid-way (the interrupted context must always be a task,
     * never a switch in progress — the parked-frame requirement). */
    if (s_need_resched)
        s_need_resched = 0;

    task_t *next = &s_tasks[idx];
    next->state = TASK_RUNNING;
    s_current = idx;
    irq_disable();
    ctx_switch(&cur->sp, &next->sp); /* returns when this task resumes */
    irq_enable();
}

/* Yield WITHOUT re-enabling the tick on resume. The keypad task uses this:
 * a HELD matrix key + the 1ms tick IRQ firing during keypad activity
 * freezes the phone on hardware (HW-verified 2026-08-21 — disabling the
 * tick entirely made multi-key spam and even SD-while-pressing work). The
 * keypad task therefore runs permanently tick-masked; the banner/SD tasks
 * (plain task_yield) re-enable the tick on their resumes, so s_ticks keeps
 * advancing and the scheduler stays round-robin. */
void task_yield_tick_masked(void)
{
    if (s_task_count == 0 || s_current < 0)
        return;

    task_t *cur = &s_tasks[s_current];
    if (cur->state == TASK_RUNNING)
        cur->state = TASK_READY;

    int idx = sched_pick_next();
    if (idx < 0) {
        if (cur->state == TASK_READY)
            cur->state = TASK_RUNNING;
        s_need_resched = 0;
        return;
    }

    if (s_need_resched)
        s_need_resched = 0;

    task_t *next = &s_tasks[idx];
    next->state = TASK_RUNNING;
    s_current = idx;
    irq_disable();
    ctx_switch(&cur->sp, &next->sp); /* resumes with I still SET */
    /* NO irq_enable: this task stays tick-masked */
}

void task_exit(void)
{
    if (s_task_count == 0 || s_current < 0)
        return;

    task_t *cur = &s_tasks[s_current];
    cur->state = TASK_DONE;

    int idx = sched_pick_next();
    if (idx >= 0) {
        task_t *next = &s_tasks[idx];
        next->state = TASK_RUNNING;
        s_current = idx;
        irq_disable();
        ctx_switch(&cur->sp, &next->sp);   /* DONE tasks are never resumed */
        irq_enable();
        return;
    }

    /* No READY task remains: hand control back to sched_start(). */
    irq_disable();
    ctx_switch(&cur->sp, &s_sched_sp);
    irq_enable();
}

task_t *sched_current(void)
{
    if (s_current < 0)
        return NULL;
    return &s_tasks[s_current];
}

int sched_task_count(void)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < s_task_count; i++) {
        if (s_tasks[i].state != TASK_DONE)
            n++;
    }
    return (int)n;
}

void sched_start(void)
{
#ifndef HOST_TEST
    /* Boot-stage print: the FIRST thing sched_start does. If the USB console
     * shows "sched: start" but no banner lines, the freeze is inside
     * sys_timer_start / irq_enable / the first ctx_switch. */
    kprintf("sched: start\n");
    /* Arm the 1 ms tick and unmask IRQs: preemption is deferred (the tick
     * sets need_resched; task_yield switches), so a well-behaved task
     * yields within one slice and a misbehaving one gets switched at its
     * next yield. The IRQ infra was built by irq_init() in main().
     *
     * RE-ENABLED 2026-08-22: the "held key + tick" freeze was NOT
     * electrical — it was the IRQ dispatcher. irq_dispatch_for_test()
     * dispatched the LOWEST pending INTC line, and the SC6530 pending
     * register (0x80000004) reflects DISABLED lines too. A keypad/EIC
     * edge (lower than the timer's line 23, no handler registered)
     * starved sys_tick_isr -> the timer source was never acked -> the
     * enabled line stayed asserted -> infinite IRQ re-entry (the silent
     * freeze). irq_handler now skips unhandled lines, so any stray
     * peripheral edge can no longer wedge the tick. See kernel/irq.c. */
    sys_timer_start();
    irq_enable();
#endif

    for (;;) {
        int idx = sched_pick_next();
        if (idx < 0)
            break;                   /* all tasks DONE */

        task_t *next = &s_tasks[idx];
        next->state = TASK_RUNNING;
        s_current = idx;

        irq_disable();
        ctx_switch(&s_sched_sp, &next->sp);
        irq_enable();
        /* resumed when the running task exited with no READY peer left */

        if (s_tasks[s_current].state == TASK_RUNNING)
            s_tasks[s_current].state = TASK_READY;   /* defensive */
    }
}
