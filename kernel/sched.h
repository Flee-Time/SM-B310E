/*
 * B310E-OS — kernel/sched.h
 *
 * Cooperative task scheduler (v1: no timer preemption — that is the IRQ
 * wave). Round-robin over a fixed table of up to SCHED_MAX_TASKS tasks.
 *
 * Context switching is a stack swap: each task runs on its own stack
 * buffer and ctx_switch() saves/restores the CPU state around it.
 *   ARM (phone): arch/ctx.s — Thumb-1 asm
 *   host (tests): kernel/ctx_host.S — x86-64 asm
 * The scheduler logic in sched.c is identical on both.
 *
 * task_create() allocates the task's stack buffer from the kernel bump
 * pool (kmalloc) — a fixed pool, so this does not fragment. stack_words is
 * the stack size in 32-bit words; pass 0 for the 512-word (2 KiB) default.
 */

#ifndef B310E_OS_SCHED_H
#define B310E_OS_SCHED_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SCHED_MAX_TASKS           32u
#define SCHED_DEFAULT_STACK_WORDS 512u   /* 2 KiB per task stack */

typedef enum {
    TASK_READY = 0,
    TASK_RUNNING,
    TASK_BLOCKED,   /* reserved for a future wait/semaphore wave */
    TASK_DONE
} task_state_t;

typedef struct task task_t;
struct task {
    const char *name;        /* static string, not copied */
    void (*fn)(void *arg);
    void *arg;
    task_state_t state;
    uint32_t stack_words;    /* allocated size of ->stack, in words */
    uintptr_t *stack;        /* task stack buffer (kernel bump pool) */
    uintptr_t sp;            /* saved stack pointer (per-task stacks) */
};

/* Create a READY task. Returns task id (1..SCHED_MAX_TASKS) or -1. */
int task_create(const char *name, void (*fn)(void *), void *arg,
                uint32_t stack_words);

/* Cooperative yield: run the next READY task round-robin. */
void task_yield(void);

/* Yield WITHOUT re-enabling the tick on resume (the keypad task stays
 * tick-masked: a held matrix key + the tick IRQ freezes the phone on
 * hardware — see task_yield_tick_masked in sched.c). */
void task_yield_tick_masked(void);

/* Mark the current task DONE and switch away. */
void task_exit(void);

/* Currently running task, or NULL if the scheduler is idle. */
task_t *sched_current(void);

/* Number of tasks still active (READY/RUNNING/BLOCKED, not DONE). */
int sched_task_count(void);

/* Run READY tasks round-robin until none remain, then return. Arms the
 * 1 ms preemptive tick first (deferred: the tick flags need_resched;
 * task_yield performs the switch) and unmask IRQs. */
void sched_start(void);

/* 1 ms tick counter (system timer 2, IRQ line 23) — the Rockbox tick. */
uint32_t sched_ticks(void);

/* Save the current CPU state onto the current stack, store sp into
 * *cur_sp, load *next_sp and resume that task's frame. Implemented in
 * arch/ctx.s (ARM) and kernel/ctx_host.S (x86-64 host). */
void ctx_switch(uintptr_t *cur_sp, uintptr_t *next_sp);

/* v2 preemptive tick (parked-frame IRQ-return switch) — implemented but
 * REVERTED on hardware (2026-08-21 boot crash; see learnings). The C-side
 * pick/stage logic below stays (host-tested); the ARM asm
 * (ctx_preempt_switch / the IRQ stub's call) was reverted to the proven
 * cooperative path. sched_preempt_check() returns the next task index to
 * preempt to (staging s_preempt_cur_sp / s_preempt_next_sp), or -1. */
int sched_preempt_check(void);
extern uintptr_t *s_preempt_cur_sp;
extern uintptr_t *s_preempt_next_sp;

#if defined(HOST_TEST)
/* Host-test hook: pretend a tick fired (no real IRQ on the host). */
void sched_test_force_preempt(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* B310E_OS_SCHED_H */
