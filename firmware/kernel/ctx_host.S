/* B310E-OS — kernel/ctx_host.S
 *
 * Host (x86-64, Windows/mingw) context switch for the cooperative
 * scheduler. Mirrors arch/ctx.s on the ARM926 so kernel/sched.c is
 * identical on both platforms.
 *
 * void ctx_switch(uintptr_t *cur_sp, uintptr_t *next_sp);
 *   rcx = &cur->sp   (slot where the CURRENT rsp is stored)
 *   rdx = &next->sp  (slot holding the NEXT task's rsp)
 *
 * Saves the current register set on the current stack, stores rsp into
 * *rcx, loads the next task's rsp from *rdx, restores that frame and
 * "returns" into the next task.
 *
 * Frame layout (rsp -> higher addresses), 9 slots of 8 bytes:
 *   [rbp][rbx][rdi][rsi][r12][r13][r14][r15][ret_addr]
 * Must stay in sync with ctx_start_frame() in kernel/sched.c.
 *
 * Windows x64 ABI: callee-saved regs rbx, rbp, rdi, rsi, r12-r15 must be
 * preserved by ctx_switch (hence the pushes); the resumed task's entry
 * RSP is 8 mod 16 (as after a normal call).
 */

    .text
    .code64

    .globl  ctx_switch
ctx_switch:
    pushq   %rbp
    pushq   %rbx
    pushq   %rdi
    pushq   %rsi
    pushq   %r12
    pushq   %r13
    pushq   %r14
    pushq   %r15
    movq    %rsp, (%rcx)            /* *cur_sp = current rsp */
    movq    (%rdx), %rax            /* rax = *next_sp */
    movq    %rax, %rsp
    popq    %r15
    popq    %r14
    popq    %r13
    popq    %r12
    popq    %rsi
    popq    %rdi
    popq    %rbx
    popq    %rbp
    ret
