/* Duke Nukem 3D on B310E OS: the two loops that draw most of the screen, in ARM code. Four
 * pixels, one word to the frame (a-c.c from prepare.py says why). A compiler keeps much
 * of this on the stack: here only what changes least.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
    .syntax unified
    .arm

/* void draw4_wall(int rows, void *pp, const void *pal, const struct wall4 *w)
 * struct wall4 { unsigned v[4]; int inc[4]; const void *buf[4]; int shift, step; };
 * Four columns with one palette; w->v is left as it ended. pp and step keep to words. */
    .section .text.draw4_wall, "ax", %progbits
    .p2align 2
    .global draw4_wall
    .type   draw4_wall, %function
draw4_wall:
    push    {r4-r11, lr}
    sub     sp, sp, #32
    ldr     r12, [r3, #52]          @ step
    mul     lr, r12, r0
    add     lr, lr, r1              @ the end
    str     lr, [sp, #24]
    str     r12, [sp, #20]
    str     r3, [sp, #28]
    add     r12, r3, #16
    ldm     r12, {r4-r7}            @ inc
    stm     sp, {r4-r7}
    add     r12, r3, #32
    ldm     r12, {r8-r11}           @ buf
    ldm     r3, {r4-r7}             @ v
    ldr     r3, [r3, #48]           @ shift
1:  lsr     r12, r4, r3
    ldrb    r0, [r8, r12]
    lsr     r12, r5, r3
    ldrb    r0, [r2, r0]
    ldrb    r12, [r9, r12]
    ldrb    r12, [r2, r12]
    orr     r0, r0, r12, lsl #8
    lsr     r12, r6, r3
    ldrb    r12, [r10, r12]
    ldrb    r12, [r2, r12]
    orr     r0, r0, r12, lsl #16
    lsr     r12, r7, r3
    ldrb    r12, [r11, r12]
    ldrb    r12, [r2, r12]
    orr     r0, r0, r12, lsl #24
    ldm     sp, {r12, lr}
    add     r4, r4, r12
    add     r5, r5, lr
    ldr     r12, [sp, #8]
    ldr     lr, [sp, #12]
    add     r6, r6, r12
    add     r7, r7, lr
    ldr     r12, [sp, #20]
    ldr     lr, [sp, #24]
    str     r0, [r1], r12
    cmp     r1, lr
    bne     1b
    ldr     r3, [sp, #28]
    stm     r3, {r4-r7}
    add     sp, sp, #32
    pop     {r4-r11, pc}
    .size   draw4_wall, . - draw4_wall

/* void draw4_floor(int groups, void *pp, struct floor4 *f)
 * struct floor4 { unsigned bx, by; int bxinc, byinc; const void *buf, *pal; int xshift, yup, yshift; };
 * groups > 0 words going down the frame from pp (the word's own address), each with its
 * first pixel in the top byte. f->bx, f->by are left as they ended. */
    .section .text.draw4_floor, "ax", %progbits
    .p2align 2
    .global draw4_floor
    .type   draw4_floor, %function
draw4_floor:
    push    {r4-r11, lr}
    mov     lr, r0
    mov     r12, r2
    ldm     r12, {r3-r11}           @ r3 bx, r4 by, r5 bxinc, r6 byinc, r7 buf, r8 pal,
                                    @ r9 xshift, r10 yup, r11 yshift
    str     r12, [sp, #-4]!
1:  lsr     r0, r3, r9
    lsr     r2, r4, r11
    add     r0, r7, r0, lsl r10
    sub     r3, r3, r5
    ldrb    r0, [r0, r2]
    sub     r4, r4, r6
    ldrb    r12, [r8, r0]

    lsr     r0, r3, r9
    lsr     r2, r4, r11
    add     r0, r7, r0, lsl r10
    sub     r3, r3, r5
    ldrb    r0, [r0, r2]
    sub     r4, r4, r6
    ldrb    r0, [r8, r0]
    orr     r12, r0, r12, lsl #8

    lsr     r0, r3, r9
    lsr     r2, r4, r11
    add     r0, r7, r0, lsl r10
    sub     r3, r3, r5
    ldrb    r0, [r0, r2]
    sub     r4, r4, r6
    ldrb    r0, [r8, r0]
    orr     r12, r0, r12, lsl #8

    lsr     r0, r3, r9
    lsr     r2, r4, r11
    add     r0, r7, r0, lsl r10
    sub     r3, r3, r5
    ldrb    r0, [r0, r2]
    sub     r4, r4, r6
    ldrb    r0, [r8, r0]
    orr     r12, r0, r12, lsl #8

    str     r12, [r1], #-4
    subs    lr, lr, #1
    bne     1b
    ldr     r12, [sp], #4
    stm     r12, {r3, r4}
    pop     {r4-r11, pc}
    .size   draw4_floor, . - draw4_floor
