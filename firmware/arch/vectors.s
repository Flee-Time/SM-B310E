/* B310E-OS — arch/vectors.s
 *
 * ARM exception vector table + mode-stack setter, ported VERBATIM from
 * fpdoom's asmcode.s:29-82 (Unlicense / public domain — copy freely).
 *
 * int_vectors: the 8 exception stubs, copied at runtime by kernel/irq.c
 * irq_init() to CHIPRAM 0x40019000 (the ARM926EJ-S high-vector page is
 * 0xffff0000; the MMU maps that page onto the CHIPRAM copy — see the
 * high-vector L1/L2 entries built in irq_init). Each stub saves
 * r0-r3,r12,lr on the exception-mode stack, then branches (blx) to the C
 * handler whose ADDRESS is stored in the three .word slots right after the
 * branch instructions (vector+0x20 / +0x24 / +0x28):
 *
 *     vector+0x20  IRQ            -> irq_handler()
 *     vector+0x24  data/prefetch  -> def_data_except(fsr|0x100, far, pc)
 *     vector+0x28  undefined      -> undef_handler(pc)
 *
 * This is EXACTLY fpdoom's mechanism: sys_set_handlers (syscode.c:1059-1075)
 * writes the C handler pointers into those slots; the stubs `ldr lr, 3b`
 * read the slot PC-relative (position independent) and `blx lr` dispatch.
 * The stubs restore the saved registers and CPSR with `ldm sp!,{...,pc}^`.
 *
 * set_mode_sp: fpdoom asmcode.s:29-33. Switch to the given CPSR mode,
 * set sp, return to SYS mode (0xdf) — the mode the whole kernel runs in.
 * Used by irq_init() to give IRQ(0xd2)/SVC(0xd3)/ABT(0xd7)/UND(0xdb) each
 * their own banked stack at 0x40022000.
 */

    .syntax unified
    .arch   armv5te
    .arm

    /* ---- int_vectors (fpdoom asmcode.s:35-82 verbatim) ------------------ */
    .section .text.int_vectors, "ax", %progbits
    .p2align 2
    .type   int_vectors, %function
    .globl  int_vectors
int_vectors:
1:  b       1b /* reset */
1:  b       6f /* undefined */
1:  b       6f /* swi */
1:  b       5f /* prefetch */
1:  b       4f /* data */
1:  b       1b /* reserved */
    b       2f /* irq */
1:  b       1b /* fiq */

3:  .long   0xffff0000          /* +0x20 IRQ        -> irq_handler (runtime) */
    .long   0xffff0000          /* +0x24 data/pref  -> def_data_except        */
    .long   0xffff0000          /* +0x28 undefined  -> undef_handler          */

2:  sub     lr, #4
    push    {r0-r3,r12,lr}
    ldr     lr, 3b
    blx     lr                  /* irq_handler (C, dispatch-ONLY) */
    ldm     sp!, {r0-r3,r12,pc}^ /* normal return (task resumes) */

4:  sub     lr, #8
    push    {r0-r3,r12,lr}
    mov     r2, lr
    ldr     lr, 3b + 4
    mrc     p15, #0, r0, c5, c0, #0 /* read DFSR */
    mrc     p15, #0, r1, c6, c0, #0 /* read FAR */
    mrs     r3, spsr                /* faulting CPSR (diagnostic) */
    /* r0 bit 8 is always zero */
    blx     lr
    ldm     sp!, {r0-r3,r12,pc}^

5:  sub     lr, #4
    push    {r0-r3,r12,lr}
    mov     r2, lr
    ldr     lr, 3b + 4
    mrc     p15, #0, r0, c5, c0, #1 /* read IFSR */
    orr     r0, #0x100
    mov     r1, r2
    mrs     r3, spsr                /* faulting CPSR (diagnostic) */
    blx     lr
    ldm     sp!, {r0-r3,r12,pc}^

6:  push    {r0-r3,r12,lr}
    sub     r0, lr, #4
    ldr     lr, 3b + 8
    blx     lr
    ldm     sp!, {r0-r3,r12,pc}^

    .globl  int_vectors_end
int_vectors_end:
    .size   int_vectors, . - int_vectors

    /* ---- set_mode_sp (fpdoom asmcode.s:29-33 verbatim) ------------------ */
    .section .text.set_mode_sp, "ax", %progbits
    .p2align 2
    .type   set_mode_sp, %function
    .globl  set_mode_sp
set_mode_sp:
    msr     CPSR_c, r0
    mov     sp, r1
    msr     CPSR_c, #0xdf /* SYS mode */
    bx      lr
    .size   set_mode_sp, . - set_mode_sp

    /* ---- irq_enable / irq_disable (CPSR I bit, bit 7) -------------------
     * Must be ARM state: ARMv5 Thumb-1 has no MSR-to-CPSR. Used to (a)
     * unmask IRQs once the preemptive tick is armed, and (b) mask them
     * around cooperative ctx_switch calls so a tick can never interrupt a
     * task-switch mid-way (the preemption hazard the parked-frame design
     * must avoid). Entry in SYS mode; returns in SYS mode. */
    .section .text.irq_ctl, "ax", %progbits
    .p2align 2
    .type   irq_enable, %function
    .globl  irq_enable
irq_enable:
    mrs     r0, CPSR
    bic     r0, r0, #0x80
    msr     CPSR_c, r0
    bx      lr
    .size   irq_enable, . - irq_enable

    .type   irq_disable, %function
    .globl  irq_disable
irq_disable:
    mrs     r0, CPSR
    orr     r0, r0, #0x80
    msr     CPSR_c, r0
    bx      lr
    .size   irq_disable, . - irq_disable
