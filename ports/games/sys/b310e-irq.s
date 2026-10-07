/* ARMv5 IRQ critical sections callable from Thumb. */
.arch armv5te
.syntax unified
.text
.arm
.global b310e_irq_save
.type b310e_irq_save, %function
b310e_irq_save:
    mrs r0, cpsr
    orr r1, r0, #0x80
    msr cpsr_c, r1
    bx lr
.global b310e_irq_restore
.type b310e_irq_restore, %function
b310e_irq_restore:
    msr cpsr_c, r0
    bx lr
