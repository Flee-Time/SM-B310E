/* The game kit: interrupts off and on again (ARM state: Thumb has no
 * mrs/msr), for state a game shares with its sound thread.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
    .syntax unified
    .arm
    .text

    .global kit_irq_save_arm
    .type   kit_irq_save_arm, %function
kit_irq_save_arm:
    mrs     r0, cpsr
    orr     r1, r0, #0x80
    msr     cpsr_c, r1
    bx      lr

    .global kit_irq_restore_arm
    .type   kit_irq_restore_arm, %function
kit_irq_restore_arm:
    msr     cpsr_c, r0
    bx      lr
