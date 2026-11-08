/* Duke Nukem 3D on B310E OS: long long / int in ARM code (the link wraps libgcc's
 * __aeabi_ldivmod). Build's divscale and scale do one for each wall and sprite: libgcc's,
 * Thumb and general, took about an eighth of the game's instructions. Here only the
 * bits the quotient has. A divisor beyond 32 bits, a quotient beyond 32 bits or a
 * division by 0 go to libgcc's as before.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
    .syntax unified
    .arm
    .section .text.__wrap___aeabi_ldivmod, "ax", %progbits
    .p2align 2
    .global __wrap___aeabi_ldivmod
    .type   __wrap___aeabi_ldivmod, %function
__wrap___aeabi_ldivmod:             @ r1:r0 / r3:r2 -> quotient r1:r0, remainder r3:r2
    cmp     r3, r2, asr #31
    bne     __real___aeabi_ldivmod
    cmp     r2, #0
    beq     __real___aeabi_ldivmod
    push    {r4-r6, lr}
    eor     r6, r1, r3              @ the quotient's sign
    mov     r12, r1                 @ the remainder's
    mov     r4, r0                  @ |n| in r5:r4
    movs    r5, r1
    bpl     1f
    rsbs    r4, r4, #0
    rsc     r5, r5, #0
1:  eor     lr, r2, r2, asr #31     @ |d|
    sub     lr, lr, r2, asr #31
    cmp     r5, lr
    bhs     6f                      @ more than 32 bits of quotient
    teq     r5, #0
    beq     2f
    clz     r0, r5                  @ the bits that cannot be quotient: past them
    clz     r1, lr
    sub     r0, r0, r1
    subs    r0, r0, #1
    movmi   r0, #0
    teq     r0, #0
    beq     3f
    rsb     r1, r0, #32
    lsl     r5, r5, r0
    orr     r5, r5, r4, lsr r1
    lsl     r4, r4, r0
    b       3f
2:  teq     r4, #0                  @ only 32 bits of n
    beq     5f
    clz     r0, r4
    lsl     r4, r4, r0
3:  rsb     r3, r0, #32             @ the quotient's bits, one by one, into r4
4:  adds    r4, r4, r4
    adcs    r5, r5, r5
    cmpcc   r5, lr
    subcs   r5, r5, lr
    addcs   r4, r4, #1
    subs    r3, r3, #1
    bne     4b
5:  mov     r0, r4
    mov     r1, #0
    teq     r6, #0
    bpl     7f
    rsbs    r0, r0, #0
    rsc     r1, r1, #0
7:  mov     r2, r5
    mov     r3, #0
    teq     r12, #0
    bpl     8f
    rsbs    r2, r2, #0
    rsc     r3, r3, #0
8:  pop     {r4-r6, pc}
6:  pop     {r4-r6, lr}
    b       __real___aeabi_ldivmod
    .size   __wrap___aeabi_ldivmod, . - __wrap___aeabi_ldivmod
