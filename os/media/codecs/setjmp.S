/* B310E OS codec modules: setjmp and longjmp (Tremor's allocator leaves a
 * track this way when its arena is full). ARM, interworking returns.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
    .syntax unified
    .arm
    .text

    .global setjmp
    .type setjmp, %function
setjmp:
    stmia   r0, {r4-r11, sp, lr}
    mov     r0, #0
    bx      lr
    .size setjmp, . - setjmp

    .global longjmp
    .type longjmp, %function
longjmp:
    ldmia   r0, {r4-r11, sp, lr}
    movs    r0, r1
    moveq   r0, #1
    bx      lr
    .size longjmp, . - longjmp
