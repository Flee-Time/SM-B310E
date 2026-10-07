/* B310E-OS — arch/smc_init.s
 *
 * SMC / SDRAM controller init for SC6530C, ported VERBATIM from fpdoom's
 * asmcode.s:474-534 (Unlicense / public domain).
 *
 * WHY IRAM: this routine reconfigures the SDRAM controller at 0x20000000
 * while the CPU is still fetching instructions from SDRAM (0x34000000).
 * Executing the register writes from SDRAM hangs the bus (observed on the
 * real B310E: spd_dump streams os.bin then hits `timeout reached` with a
 * black screen). fpdoom therefore runs this routine from on-chip IRAM:
 * arch/main.c copies [sc6530_init_smc_asm, sc6530_init_smc_asm_end) into
 * SMC_INIT_BUF (0x40009c00) and calls it there.
 *
 * The routine is position-independent and self-contained: `adr`/`ldmia`
 * load the inline data table (label `1:`) relative to the PC, and the two
 * delay loops (`2:`/`3:`) are local. No externs, no relocations.
 */

    .syntax unified
    .arch   armv5te
    .arm

    .section .text.sc6530_init_smc_asm, "ax", %progbits
    .p2align 2
    .type   sc6530_init_smc_asm, %function
    .globl  sc6530_init_smc_asm
sc6530_init_smc_asm:
    push    {r4-r9,lr}
    adr     r6, 1f
    ldmia   r6!, {r0-r3}

    mov     r7, #0x20000000
    mov     r4, #0
    str     r0, [r7]
    str     r4, [r7, #0x04]
    str     r4, [r7, #0x20]
    str     r1, [r7, #0x24]
    str     r2, [r7, #0x28]
    str     r3, [r7, #0x2c]
    mov     r0, #100
    bl      3f

    ldmia   r6!, {r0-r3,r5}

    /* psram */
    ldr     r5, [r5]
    ands    r5, #1
    movne   r5, #0x30000000
    orr     r5, #0x04000000

    str     r0, [r7]
    str     r3, [r7, #0x04]
    eor     r8, r0, #0x100

    ldr     r0, [r7, #0x24]
    orr     r0, #0x20000
    str     r0, [r7, #0x24]
    strh    r4, [r5, r1]
    bl      2f
    strh    r4, [r5, r2]
    bl      2f
    ldr     r0, [r7, #0x24]
    bic     r0, #0x20000
    str     r0, [r7, #0x24]
    bl      2f

    ldmia   r6!, {r0-r2}
    str     r0, [r7, #0x24]
    str     r1, [r7, #0x28]
    str     r2, [r7, #0x2c]
    str     r8, [r7]
    str     r3, [r7, #0x04]
    mov     r0, #100
    bl      3f
    pop     {r4-r9,pc}

2:  mov     r0, #10
3:  subs    r0, #1
    bne     3b
    bx      lr

1:  .long   0x22220000, 0x00924ff0, 0x0151ffff, 0x00a0744f
    .long   0x222211e0, 0x10323e, 0x20, 0x8080, 0x205000e0
    .long   0x00ac1fff, 0x015115ff, 0x00501015

    .globl  sc6530_init_smc_asm_end
sc6530_init_smc_asm_end:
    .size   sc6530_init_smc_asm, . - sc6530_init_smc_asm
