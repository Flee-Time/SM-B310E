/* B310E boot loader — NOR entry, header and the jump into a program.
 *
 * The phone's boot vector (NOR word 0x20) points here, at NOR 0x3fc000.
 * The CPU arrives in ARM state from the 0x0 vector, MMU off.
 *
 * The no-key path must leave the chip as the stock PBL expects it, so it
 * does what fpdoom's sdboot did before chaining (the path the phone has
 * run on every power-on since 2026-08-23): MMU and D-cache off, I-cache
 * on, SVC mode with IRQ and FIQ masked, a stack below 0x40009000, the
 * keypad powered for 20 ms. boot_early() uses that stack and nothing
 * else: .data and .bss are only set up when the loader stays. All of this
 * is in .early, below NOR 4 MiB (boot.ld).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "boot.h"

    .syntax unified
    .arm

    .section .head, "ax", %progbits
    .global _start
_start:
    b       reset                       /* +0x00 */
    .global boot_header
    .word   STOCK_ENTRY                 /* +0x04 the stock PBL entry */
    .ascii  "B310BOOT"                  /* +0x08 */
    .word   __image_size                /* +0x10 */
    .word   BOOT_VERSION                /* +0x14 */
    .word   DEFAULT_MODE                /* +0x18 mkboot.py --default */
    .word   0                           /* +0x1c reserved */
    .ascii  DEFAULT_PROGRAM             /* +0x20 48 bytes, NUL padded */
    .space  HDR_PATH_LEN - (. - _start - 0x20)
    .word   0                           /* +0x50 CRC-32 (mkboot.py) */
    .word   0, 0, 0                     /* +0x54 reserved */

reset:
    mrc     p15, 0, r0, c1, c0, 0
    bic     r0, r0, #5                  /* MMU, D-cache off */
    orr     r0, r0, #0x1000             /* I-cache on */
    mcr     p15, 0, r0, c1, c0, 0
    msr     cpsr_c, #0xd3               /* SVC, IRQ and FIQ masked */
    ldr     sp, =EARLY_STACK_TOP
    bl      boot_early                  /* (request << 8) | flags */
    movs    r4, r0
    bne     stay
    ldr     r0, _start + 4              /* chain the stock firmware */
    bx      r0

stay:
    /* The rest lies past NOR 4 MiB: the flash controller's size to 8 MiB
     * first (SFC 0x20a00200, as the stock PBL sets it, NOR 0x10354), then
     * the marker at the image's end must read right; if not, the stock
     * firmware rather than a crash (the keypad light blinks 3 times). */
    ldr     r0, =SFC_SIZE_REG
    mov     r1, #8
    str     r1, [r0]
    ldr     r0, =boot_tail
    ldr     r1, [r0]
    ldr     r2, =BOOT_TAIL_MAGIC
    cmp     r1, r2
    ldreq   r1, [r0, #4]
    mvneq   r2, r2
    cmpeq   r1, r2
    bne     window_bad

    ldr     sp, =__stack_top
    ldr     r0, =__data_load            /* .data from NOR */
    ldr     r1, =__data_start
    ldr     r2, =__data_end
1:  cmp     r1, r2
    ldrlo   r3, [r0], #4
    strlo   r3, [r1], #4
    blo     1b
    ldr     r1, =__bss_start
    ldr     r2, =__bss_end
    mov     r3, #0
2:  cmp     r1, r2
    strlo   r3, [r1], #4
    blo     2b
    mov     r0, r4
    bl      boot_main
3:  b       3b

window_bad:
    mov     r0, #3                      /* the keypad light three times */
    bl      early_signal
    ldr     r0, _start + 4
    bx      r0

/* boot_enter(addr): run a program at addr the way the fpdoom menu left
 * B310E OS and Rockbox, minus the MMU: ARM state, MMU and D-cache off,
 * I-cache invalidated and on, CP15 V=1 (high vectors; Rockbox's crt0
 * maps 0xffff0000 because of it). */
    .section .text.boot_enter, "ax", %progbits
    .global boot_enter
    .type   boot_enter, %function
boot_enter:
    msr     cpsr_c, #0xd3
    mov     r4, r0
    mov     r0, #0
    mcr     p15, 0, r0, c7, c10, 4      /* drain the write buffer */
    mrc     p15, 0, r1, c1, c0, 0
    bic     r1, r1, #5
    orr     r1, r1, #0x2000             /* V */
    orr     r1, r1, #0x1000             /* I */
    mcr     p15, 0, r1, c1, c0, 0
    mcr     p15, 0, r0, c7, c5, 0       /* invalidate the I-cache */
    mcr     p15, 0, r0, c8, c7, 0       /* and the TLBs */
    mov     r1, #0
    mov     r2, #0
    mov     r3, #0
    bx      r4

/* boot_chain_stock(entry): the stock firmware after a full init (the
 * menu's "Stock firmware" uses a reset instead; this is the fallback). */
    .global boot_chain_stock
    .type   boot_chain_stock, %function
boot_chain_stock:
    msr     cpsr_c, #0xd3
    mrc     p15, 0, r1, c1, c0, 0
    bic     r1, r1, #0x2000             /* V off: the PBL's own vectors */
    mcr     p15, 0, r1, c1, c0, 0
    ldr     sp, =EARLY_STACK_TOP
    bx      r0

    .ltorg

/* The marker start.S checks: the last words of .text (boot.ld). */
    .section .tail, "a", %progbits
    .global boot_tail
boot_tail:
    .word   BOOT_TAIL_MAGIC, ~BOOT_TAIL_MAGIC
