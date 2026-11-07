/* B310E OS — memory and register access that survives a data abort (the
 * network shell's peek and poke).
 *
 *   int probe_read(uintptr_t addr, int size, uint32_t *out)
 *   int probe_write(uintptr_t addr, int size, uint32_t value)
 *
 * size 1, 2 or 4 (aligned); 0, or -1 when the access aborted. With
 * interrupts off, probe_fixup holds the address to resume at; the data
 * abort handler (cpu.S) clears it and returns there. This only catches
 * aborts: a block whose clock is off stalls the bus instead and is never
 * caught, so the shell restricts the addresses it lets through.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
    .syntax unified
    .arm

    .bss
    .align  2
    .global probe_fixup
probe_fixup:
    .word   0

    .text
    .global probe_read
    .type   probe_read, %function
probe_read:
    stmfd   sp!, {r4, lr}
    mrs     r4, cpsr
    orr     r3, r4, #0xc0
    msr     cpsr_c, r3              /* IRQ and FIQ off */
    ldr     r3, =probe_fixup
    adr     ip, 9f
    str     ip, [r3]
    cmp     r1, #1
    ldrbeq  ip, [r0]
    cmp     r1, #2
    ldrheq  ip, [r0]
    cmp     r1, #4
    ldreq   ip, [r0]
    mov     r0, #0
    str     r0, [r3]                /* done: no fixup */
    str     ip, [r2]
    b       8f
9:  mvn     r0, #0                  /* aborted (the handler cleared the fixup) */
8:  msr     cpsr_c, r4
    ldmfd   sp!, {r4, lr}
    bx      lr

    .global probe_write
    .type   probe_write, %function
probe_write:
    stmfd   sp!, {r4, lr}
    mrs     r4, cpsr
    orr     r3, r4, #0xc0
    msr     cpsr_c, r3
    ldr     r3, =probe_fixup
    adr     ip, 9f
    str     ip, [r3]
    cmp     r1, #1
    strbeq  r2, [r0]
    cmp     r1, #2
    strheq  r2, [r0]
    cmp     r1, #4
    streq   r2, [r0]
    mov     r0, #0
    str     r0, [r3]
    b       8f
9:  mvn     r0, #0
8:  msr     cpsr_c, r4
    ldmfd   sp!, {r4, lr}
    bx      lr
    .ltorg
