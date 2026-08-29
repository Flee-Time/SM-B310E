/* B310E-OS ??? arch/ctx.s
 *
 * ARM (Thumb-1) context switch for the cooperative scheduler.
 *
 * void ctx_switch(uint32_t *cur_sp, uint32_t *next_sp);
 *
 *   r0 = &cur->sp   (slot where the CURRENT sp is stored)
 *   r1 = &next->sp  (slot holding the NEXT task's sp)
 *
 * Saves the current register set on the current stack, stores the stack
 * pointer into *cur_sp, loads the next task's sp from *next_sp, restores
 * that frame and "returns" into the next task.
 *
 * Frame layout (sp -> higher addresses), 10 words:
 *   [r12][r10][r11][r8][r9][r4][r5][r6][r7][pc]
 * Must stay in sync with ctx_start_frame() in kernel/sched.c.
 *
 * Thumb-1 constraints honoured: only r0-r7 + lr in push/pop; r8-r12 are
 * moved through low registers; PC pop switches state via its Thumb bit.
 */

    .syntax unified
    .arch   armv5te
    .thumb

    .text

    .thumb_func
    .globl  ctx_switch
ctx_switch:
    push    {r4, r5, r6, r7, lr}     /* low regs + return address */
    mov     r2, r8
    mov     r3, r9
    push    {r2, r3}                 /* r8, r9  */
    mov     r2, r10
    mov     r3, r11
    push    {r2, r3}                 /* r10, r11 */
    mov     r2, r12
    push    {r2}                     /* r12 */
    mov     r2, sp
    str     r2, [r0]                 /* *cur_sp = current sp */
    ldr     r2, [r1]                 /* r2 = *next_sp */
    mov     sp, r2
    pop     {r2}
    mov     r12, r2
    pop     {r2, r3}
    mov     r11, r3
    mov     r10, r2
    pop     {r2, r3}
    mov     r9, r3
    mov     r8, r2
    pop     {r4, r5, r6, r7, pc}     /* restore low regs, return */
    .size   ctx_switch, . - ctx_switch

    .ltorg
