/*
 * B310E-OS — kernel/irq.h
 *
 * IRQ infrastructure (wave: exception vectors + per-mode stacks + IRQ
 * dispatch + minimal MMU). This wave installs the vector table and turns
 * the MMU on with the high-vector page mapped; NO interrupt source is
 * enabled yet and IRQs stay masked at the CPSR level (msr cpsr_c,#0xdf in
 * arch/start.s). The next wave (preemptive scheduler) enables the 1 ms
 * system timer IRQ on top of this.
 *
 * Pure dispatch logic (irq_line_from_pending / irq_dispatch_for_test) is
 * host-compilable; the hardware/asm paths (irq_init, the C handlers) are
 * #ifndef HOST_TEST stubs so `make hosttest` links.
 */

#ifndef B310E_OS_IRQ_H
#define B310E_OS_IRQ_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Boot the IRQ infrastructure. Called from arch/main.c right before the
 * first task_create(): copies the fpdoom exception stubs to CHIPRAM
 * 0x40019000, sets IRQ/SVC/ABT/UND stacks at 0x40022000, installs the C
 * handler pointers into the vector slots, builds the 16 KB MMU page table
 * at the top of SDRAM (identity low 64 MB, full-access device region,
 * SDRAM + CHIPRAM cached, high-vector page 0xffff0000 -> CHIPRAM) and
 * enables the MMU. Never returns to the LCD path once called. */
void irq_init(void);

/* Register (or, with fn == NULL, unregister) the C handler for interrupt
 * line `line` (0..31 — the SC6530 INT controller bit numbers, e.g. the
 * system-timer line). Handlers run on the IRQ stack in IRQ mode. */
void irq_register(int line, void (*fn)(void));

/* The IRQ dispatcher installed in the vector table slot at vector+0x20
 * (fpdoom sys_set_handlers): read the SC6530 pending register
 * (0x80000004), dispatch to the registered handler for the lowest set
 * line, then clear it; a spurious pending with no line clears all. */
void irq_handler(void);

/* Fatal-exception handlers installed at vector+0x24 (data/prefetch abort,
 * r0=FSR[+0x100 for prefetch], r1=FAR, r2=PC, r3=faulting CPSR from
 * SPSR_abt) and vector+0x28 (undefined instruction, r0=PC). Print
 * diagnostics and halt. */
void def_data_except(uint32_t fsr, uint32_t far, uint32_t pc, uint32_t cpsr);
void undef_handler(uint32_t pc);

/* ---- pure logic exposed for host tests --------------------------------- */

/* Lowest set bit of the pending bitmap -> its line index (0..31), or -1
 * if nothing is pending. The SC6530 pending register semantics match
 * fpdoom (usbio.c:287-291 uses bit 25 for USB on SC6530): bit N = line N. */
int irq_line_from_pending(uint32_t pending);

/* Pure dispatch: given a pending bitmap, call the registered handler for
 * the lowest set line (if any) and return the line, or -1 for none. The
 * real irq_handler() feeds it the live pending register and does the
 * clear. */
int irq_dispatch_for_test(uint32_t pending);

/* Hook for the preemptive scheduler: the 1 ms system-timer ISR. Weak
 * default (no-op) here; the scheduler overrides it and calls
 * irq_register(SYS_TIMER_IRQ_LINE, sys_tick_isr). */
void sys_tick_isr(void);

/* Arm the 1 ms system-timer (timer 2 @ 0x81000040, IRQ line 23, 26 MHz
 * clock, load 26000). Registers sys_tick_isr on the timer line and enables
 * the INT-controller line. Does NOT unmask the CPU I bit — call irq_enable
 * after tasks are created (sched_start does). */
void sys_timer_start(void);

/* Fully disable / re-enable the tick IRQ line at the INT controller (not
 * just the CPU I bit). Used around the SD probe: a constantly-asserting
 * timer line during SDIO init kills the phone on hardware (see the
 * sys_timer_pause comment in irq.c). The timer hardware keeps running;
 * only IRQ delivery is off. */
void sys_timer_pause(void);
void sys_timer_resume(void);

/* IRQ mask helpers (arch/vectors.s, ARM state). irq_enable clears the
 * CPSR I bit (0x5f); irq_disable sets it (0xdf). The scheduler masks IRQs
 * around ctx_switch so a tick can never interrupt a task-switch. */
void irq_enable(void);
void irq_disable(void);

#ifdef __cplusplus
}
#endif

#endif /* B310E_OS_IRQ_H */
