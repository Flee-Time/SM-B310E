/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2008 by Karl Kurbjun
 * Copyright (C) 2026 by B310E-OS project
 *
 * Arm bootloader and startup code based on startup.s from the iPodLinux
 * loader
 *  Copyright (c) 2003, Daniel Palffy (dpalffy (at) rainstorm.org)
 *  Copyright (c) 2005, Bernard Leach <leachbj@bouncycastle.org>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/
#include "config.h"
#include "cpu.h"

/*
 * B310E-OS Rockbox port — b310e/target/arm/sc6530c/crt0.S
 * (GPLv2, Rockbox-derived; modeled on Rockbox's s3c2440/crt0.S)
 *
 * SC6530C startup. The B310E boot menu has ALREADY: brought up the PSRAM
 * window, enabled the clocks (208 MHz), initialized the SDRAM controller
 * and copied this image to its link address 0x04000000. So unlike the
 * s3c2440 crt0 there is NO memory-controller init, NO clock init, NO
 * self-copy, and no GPIO/pinmux writes (0x8c writes HANG this phone).
 *
 * What remains:
 *   - the vector table at image offset 0 (the menu jumps to 0x04000000
 *     in ARM state — the CPU must decode the first instruction here as
 *     ARM, hence .code 32)
 *   - SVC mode + IRQ/FIQ mask, mask all INTC lines
 *   - an IDENTITY MMU (VA == PA, uncached) plus the ARM high-vector page
 *     0xffff0000 aliased onto this image's vector table
 *   - bss zero, mode stacks, `ldr pc, =main`
 *
 * Why the MMU is REQUIRED (not an optimization): the menu's start.s left
 * CP15 c1 V=1 (high vectors), so IRQ exceptions fetch from 0xffff0000.
 * On the SC6530 that address decodes to NOTHING without the MMU — the tick
 * IRQ (kernel-sc6530c.c) could never fire and Rockbox would hang in
 * kernel_init. The identity mapping also means FRAME (the LCDC DMA source)
 * is at the same address for the CPU and the DMA — no aliasing headaches.
 * D-cache/MMU caching is a later optimization; M1 keeps everything
 * uncached (write-through to SDRAM) so the LCDC DMA always sees committed
 * pixels.
 */

/* Exception Handlers */
.section .vectors,"ax",%progbits
.code 32

.global vectors
vectors:
    /* HIGH-VECTOR-SAFE dispatch: the crt0 identity MMU maps the ARM
     * high-vector page 0xffff0000 onto THIS vector table (V=1 left by the
     * B310E-OS menu), so every exception vector EXECUTES at 0xffff0000+.
     * A link-time `b handler` would compute its offset relative to
     * 0x04000000 and misfire at 0xffff0000 (target = handler + 0xcbff0000)
     * — the QEMU test proved it: the first tick IRQ branched into garbage
     * and the guest spin-aborted at the vectors. `ldr pc, =handler` loads
     * the ABSOLUTE address from a literal pool right below (inside the
     * mapped 4 KiB window), so it works from both 0x04000000 (the menu's
     * bx entry) and 0xffff0000 (exception entry). Same pattern as the
     * Rockbox s5l8702/s5l8700 crt0.S and B310E-OS arch/vectors.s. */
    ldr     pc, =start
    ldr     pc, =undef_instr_handler
    ldr     pc, =software_int_handler
    ldr     pc, =prefetch_abort_handler
    ldr     pc, =data_abort_handler
    ldr     pc, =reserved_handler
    ldr     pc, =irq_handler
    ldr     pc, =fiq_handler

/* This branch is used to make sure that we know where the shutdown routine
 * is located in flash (0x040A0020) — kept for ROLO/bootloader compat.
 */
    b   rom_shutdown

/* Add some strings to detect the bootloader in flash and give it a version
 * number. (0x040A0028, 0x040A002C)
 */
.string "ROCKBOX\0"
.string "R 03.00\0"

/* The literal pool for the 8 `ldr pc, =handler` slots above. MUST stay
 * inside the first 4 KiB (the high-vector page window) — it does: the
 * pool lands at image offset ~0x34. The addresses are baked at link time
 * (Rockbox is fixed-linked at 0x04000000, no -pie). */
.ltorg

/*
 * Entry: start
 *  Variables:
 *      none
 */

.section    .init.text,"ax",%progbits
.code       32
.align      0x04
.global     start
start:

    /* enter supervisor mode, disable IRQ and FIQ */
    msr     cpsr, #0xd3

    /* Mask all interrupt lines at the SC6530 INTC. 0x8000000C is
     * INT_DISABLE (a set-ones-to-mask register), NOT an acknowledge —
     * writing 0xFFFFFFFF here masks every line. The timer ISR
     * (kernel-sc6530c.c tick_start) enables line 23 later via 0x80000008. */
    ldr     r2, =0xFFFFFFFF
    mov     r1, #0x80000000
    str     r2, [r1, #0x0C]

    /* BOOT MARKER (level 1 = dim): crt0 entered, SVC mode + INTC masked.
     * The user's HW hang report decodes by keylight brightness — see the
     * marker table in the rockbox-port learnings. */
    mov     r0, #1
    bl      b310e_boot_mark

    /************************** IDENTITY MMU (M1) **************************/
    /* The B310E-OS boot path runs with the MMU off and caches off; we turn
     * the MMU on with an identity table so (a) 0xffff0000 (high vectors,
     * V=1 left by the menu) maps onto our vector table and (b) every
     * address is VA == PA (the LCDC DMA reads FRAME at its physical
     * address; the CPU writes it at the same address — no cache/alias
     * issue, everything uncached).
     *
     * Descriptor values are the B310E-OS/fpdoom-proven bit patterns
     * (B310E-OS kernel/irq.c mmu_build — HW-verified on this chip):
     *   section:  phys | 0xC12   (full access, buffered, uncached)
     *   coarse L1: l2 | 0x31
     *   small L2: phys | 0x0E    (cached+buffered page, type 2)
     * Do NOT "fix" them from the ARM ARM — this implementation is quirky.
     */

    /* L2 small-page table for the high-vector page. l2_table lives in
     * .init.text (zero-filled in the binary), 1 KB aligned. Entry for
     * va 0xffff0xxx is index 0xf0 (byte offset 0x3C0): map it to the
     * vector table at physical 0x04000000. */
    ldr     r0, =l2_table
    ldr     r1, =0x0400000E          /* pa 0x04000000 | CB | small page */
    str     r1, [r0, #0x3C0]

    /* Identity L1: all 4096 sections map to themselves. */
    ldr     r0, =TTB_BASE_ADDR
    mov     r1, #0
    mov     r2, #0x1000
    ldr     r3, =0xC12
mmu_id_loop:
    orr     r4, r1, r3
    str     r4, [r0], #4
    add     r1, r1, #0x100000
    subs    r2, r2, #1
    bne     mmu_id_loop

    /* L1[0xfff] = coarse-page descriptor for l2_table (domain 1).
     * r0 still points one PAST the table end from the IDENTITY loop above
     * (TTB_BASE_ADDR + 0x4000), so entry 0xfff is at [r0, #-4] — a 0x3FFC
     * literal offset would exceed the ARMv5 STR 12-bit immediate limit.
     * MUST run BEFORE the cache patch below: that loop uses its own r0
     * and would otherwise move this store to the wrong entry (the bug
     * that hung the boot — entry 0x7f got the coarse-page descriptor and
     * entry 0xfff stayed identity, so the high-vector map was lost and
     * the PSRAM window top became invalid). */
    ldr     r4, =l2_table
    orr     r4, r4, #0x31
    str     r4, [r0, #-4]

    /* Cache the PSRAM window (0x04000000-0x08000000): patch L1 entries
     * 64..127 (indices 0x40..0x7f) from the flat uncached 0xC12 to 0xC1E
     * (|C|B = 3<<2) — the B310E-OS mmu_build pattern (kernel/irq.c
     * cb = 3u<<2). Rockbox rendering runs 10-20x faster cached;
     * peripherals and IRAM stay uncached (the SD bounce buffer at
     * 0x4000b000 stays DMA-coherent without cache ops). */
    ldr     r0, =TTB_BASE_ADDR
    add     r0, r0, #(64 * 4)     /* L1 entry 0x40 (0x04000000 >> 20) */
    ldr     r3, =0xC1E
    ldr     r4, =0x04000000
    mov     r2, #64               /* sections 0x40..0x7f */
mmu_cache_loop:
    orr     r4, r4, r3
    str     r4, [r0], #4
    add     r4, r4, #0x100000
    subs    r2, r2, #1
    bne     mmu_cache_loop

    /* Enable the MMU (B310E-OS arch/mmu.s enable_mmu, verbatim order:
     * TTBR -> DACR 0x57 -> invalidate TLB/D/I -> drain WB -> c1 |= R|M).
     * The TLB invalidate runs AFTER the descriptor patch (the new cache
     * attributes take effect). The D-cache invalidate (c7,c6,0) runs HERE
     * while C=0 — the ARM926 overrides C while M=0, so the cache is empty
     * and the invalidate is harmless; the C-bit enable below starts from
     * a clean cache.
     * ALSO set SCTLR bit 9 (R, ROM prot) — the high-vector L2 small page
     * descriptor (0x0400000E) has AP bits 00, and in a CLIENT domain the
     * ARMv5 grants AP=00 read/exec only when R or S is set. B310E-OS
     * start.s sets R=1 before its own MMU on; the QEMU arm926 resets with
     * R=0, so without this the tick IRQ's vector fetch faults. The
     * identity L1 sections are domain-0 MANAGER (0xC12) so they ignore AP
     * entirely — only the vector page needs R. */
    ldr     r0, =TTB_BASE_ADDR
    mov     r1, #0x57
    mcr     p15, 0, r0, c2, c0, 0
    mcr     p15, 0, r1, c3, c0, 0
    mov     r0, #0
    mcr     p15, 0, r0, c8, c7, 0
    mcr     p15, 0, r0, c7, c6, 0
    mcr     p15, 0, r0, c7, c5, 0
    mcr     p15, 0, r0, c7, c10, 4
    mrc     p15, 0, r0, c1, c0, 0
    orr     r0, r0, #0x200       /* R bit (ROM prot) */
    orr     r0, r0, #1           /* MMU */
    mcr     p15, 0, r0, c1, c0, 0

    /* Enable the D-cache SEPARATELY (B310E-OS enable_dcache ordering): the
     * caches were invalidated above while C=0, so the D-cache is empty;
     * set the C bit and BRANCH to flush the pipeline (the ARM926 TRM
     * requires a branch after an M/C SCTLR change). NEVER invalidate the
     * D-cache after C is live — instructions between the C-bit set and a
     * late invalidate would allocate lines that the invalidate discards
     * (the hang mechanism: literal-pool loads / stack pushes in that
     * window get corrupted). */
    mrc     p15, 0, r0, c1, c0, 0
    orr     r0, r0, #4           /* C bit (D-cache) */
    mcr     p15, 0, r0, c1, c0, 0
    b       .Ldcache_on

.Ldcache_on:
    /* BOOT MARKER (level 4): MMU + D-cache are on (TTB + high-vector map).
     * A hang here means the TTB/MMU setup wedged. */
    mov     r0, #4
    bl      b310e_boot_mark

    /* Initialise bss section to zero */
    ldr     r2, =_edata
    ldr     r3, =_end
    mov     r4, #0
bsszero:
    cmp     r3, r2
    strhi   r4, [r2], #4
    bhi     bsszero

    /* Set up stack for IRQ mode */
    msr     cpsr_c, #0xd2
    ldr     sp, =irq_stack
    /* Set up stack for FIQ mode */
    msr     cpsr_c, #0xd1
    ldr     sp, =fiq_stack

    /* Let svc, abort and undefined modes use irq stack */
    msr     cpsr_c, #0xd3
    ldr     sp, =irq_stack
    msr     cpsr_c, #0xd7
    ldr     sp, =irq_stack
    msr     cpsr_c, #0xdb
    ldr     sp, =irq_stack

    /* Switch to sys mode */
    msr     cpsr_c, #0xdf

    /* Set up some stack and munge it with 0xdeadbeef */
    ldr     sp, =stackend
    mov     r3, sp
    ldr     r2, =stackbegin
    ldr     r4, =0xdeadbeef
stackmunge:
    cmp     r3, r2
    strhi   r4, [r2], #4
    bhi     stackmunge

    /* Start the main function */
    /* BOOT MARKER (level 8): crt0 setup complete; main() is next. A hang
     * here means the bss zero / stack setup wedged (or never ran). */
    mov     r0, #8
    bl      b310e_boot_mark
    ldr     lr, =vectors
    ldr     pc, =main

    /* Should never get here, but let's restart in case (also needed for
     * linking) */
    b       vectors

/* ---- b310e_boot_mark: FIFO-gated keylight boot marker -------------------
 * r0 = 4-bit brightness (0-15); writes 0x1100|level to ANA_WHTLED_CTRL
 * 0x82001220 through the ADI mailbox, exactly the B310E-OS arch/start.s
 * pattern (bounded waits — a wedged FIFO degrades to "no visible change"
 * instead of hanging the boot). Runs in SVC mode with IRQs masked (MMIO
 * only); preserves r4-r7 + lr so it can be called anywhere in the crt0
 * flow; r0 is the documented input. */
.section    .init.text, "ax", %progbits
.code       32
.align      4
.type       b310e_boot_mark, %function
.global     b310e_boot_mark
b310e_boot_mark:
    push    {r4-r7, lr}
    mov     r7, #0x40000         /* ~1M-iteration budget (bounded) */
    ldr     r4, =0x82000020      /* ADI FIFO sts (bit9 full, bit8 empty) */
1:  ldr     r5, [r4]
    tst     r5, #0x200           /* bit9 = FIFO_FULL */
    beq     5f                   /* not full: proceed to the write */
    subs    r7, r7, #1
    bne     1b
5:  mov     r5, #0x1100
    orr     r5, r5, r0           /* 0x1100 | level */
    ldr     r6, =0x82001220      /* ANA_WHTLED_CTRL (backlight) */
    str     r5, [r6]
    mov     r7, #0x40000
2:  ldr     r5, [r4]
    tst     r5, #0x100           /* bit8 = FIFO_EMPTY */
    bne     4f                   /* empty: done */
    subs    r7, r7, #1
    bne     2b
4:  pop     {r4-r7, pc}
.size       b310e_boot_mark, .-b310e_boot_mark

#if defined(BOOTLOADER)
fiq_handler:
    b UIE
#else
/* The vector table branches here; FIQ is never used on this target. */
fiq_handler:
    b fiq_handler
#endif

UIE:
    b UIE

/*
 * Function: rom_shutdown
 *  Variables:
 *      none
 */

.section    .init.text, "ax", %progbits
.align      0x04
.global     rom_shutdown
.type       rom_shutdown, %function
rom_shutdown:
    /* Turn off the MMU and halt. */
    mrc     p15, 0, r1, c1, c0, 0
    bic     r1, r1, #0x0001
    mcr     p15, 0, r1, c1, c0, 0
1:
    b       1b
.size       rom_shutdown, .-rom_shutdown

/* L2 small-page table for the high-vector page (see start above).
 * 1 KB aligned; 256 entries of 0 = invalid (the 0xf0th entry is written
 * at boot). This MUST stay in a loadable section so the zeros are in the
 * binary (the linker zero-fills .space). */
.section    .init.text, "ax", %progbits
.align      10
.global     l2_table
l2_table:
    .space  0x400

.section .text
/* 256 words of IRQ stack */
    .space 256*4
irq_stack:

/* 256 words of FIQ stack */
    .space 256*4
fiq_stack:
