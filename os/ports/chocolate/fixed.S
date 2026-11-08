/* Chocolate Doom, Heretic and Hexen on B310E OS: FixedDiv in ARM code
 * (the link wraps upstream's, a 64-bit division: libgcc's __aeabi_ldivmod
 * on the ARM926, which has no divider). a / b in 16.16, saturating like
 * upstream when |a| >> 14 >= |b|. The loop is fpdoom's FixedDivAsm
 * (fpdoom/doomasm.s, public domain: The Unlicense); its saturation test
 * is upstream's here.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
    .syntax unified
    .arm
    .section .text.__wrap_FixedDiv, "ax", %progbits
    .p2align 2
    .global __wrap_FixedDiv
    .type   __wrap_FixedDiv, %function
__wrap_FixedDiv:
    eor     r12, r0, r1             @ the sign of the quotient
    eor     r2, r0, r0, asr #31
    eor     r3, r1, r1, asr #31
    sub     r2, r2, r0, asr #31     @ |a|
    sub     r3, r3, r1, asr #31     @ |b|
    mov     r0, #0x7fffffff
    eor     r0, r0, r12, asr #31    @ saturated: INT_MAX or INT_MIN
    cmp     r3, r2, lsr #14         @ |b| <= |a| >> 14 (or b = 0): saturated,
    bxls    lr                      @ as upstream (demos stay in sync)
    lsr     r1, r2, #15
    lsl     r2, #17
    mov     r0, #2
1:  lsls    r2, #1
    adc     r1, r1
    cmp     r1, r3
    subhs   r1, r3
    adcs    r0, r0
    bcc     1b
    tst     r12, r12
    rsbmi   r0, #0
    bx      lr
