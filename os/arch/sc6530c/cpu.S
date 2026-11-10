/* B310E OS — ARM926EJ-S start-up, exception entry and context switch.
 *
 * Boot contract (the SD boot menu, as for Rockbox): the image sits at the
 * PSRAM base 0x04000000 and is entered at its first word in ARM state,
 * MMU and caches off, CP15 V=1 (high vectors). The MMU recipe is the one
 * the Rockbox port verified on the phone (ports/rockbox/.../crt0.S):
 * identity sections 0xc12 (manager domain), PSRAM sections cached
 * (0xc1e), and a coarse table that maps the high-vector page 0xffff0000
 * onto our vector table (small page 0x0e, domain 1, readable with R=1).
 * Do not "correct" the descriptor values from the ARM ARM.
 *
 * Threads run in SYS mode with interrupts enabled. Every switch, from an
 * interrupt or from svc (thread_yield), saves the same 17-word frame on
 * the thread's own stack:
 *     [cpsr] [r0..r12] [sp] [lr] [pc]      (lowest address first)
 * and kernel_current->sp points at it.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/arch.h"

    .syntax unified
    .arm

/* ---- vector table: first in the image, also mapped at 0xffff0000 ------- */
    .section .vectors, "ax", %progbits
    .global _vectors
_vectors:
    ldr     pc, =_start
    ldr     pc, =undef_entry
    ldr     pc, =svc_entry
    ldr     pc, =pabort_entry
    ldr     pc, =dabort_entry
    ldr     pc, =_start
    ldr     pc, =irq_entry
    ldr     pc, =fiq_entry
    .ltorg                      /* literals stay inside the vector page */
    .asciz  "B310E-OS"
    .align  2

/* ---- start-up ----------------------------------------------------------- */
    .section .text.start, "ax", %progbits
    .global _start
    .type   _start, %function
_start:
    mrc     p15, 0, r12, c1, c0, 0      /* the control register we were given */
    msr     cpsr_c, #0xd3       /* SVC, IRQ and FIQ masked */
    mov     r0, #0x80000000
    mvn     r1, #0
    str     r1, [r0, #0x0c]     /* INTC: mask every line */

    /* IRAM banks 1..3 on (0x8b0001a0 bits 21:19, as fpdoom's entry.c). The
     * fpdoom menu set them before it started us; our NOR loader does not,
     * and without them IRAM from about 0x4000c000 does not hold data: the
     * chain trampoline. The DSP needs them clear: drivers/dsp.c clears them
     * while it runs, as stock does for good (NOR 0x1a7c0). */
    ldr     r0, =0x8b0001a0
    ldr     r1, [r0]
    orr     r1, r1, #(7 << 19)
    str     r1, [r0]

    /* Entered with the MMU and D-cache on, the cache holds the truth:
     * write it back and switch both off before zeroing .bss. Otherwise
     * stores that hit lines the loader left cached stay in the cache and
     * mmu_setup's invalidate drops them, and those .bss words keep the
     * loader's data (on the phone 2026-11-07: the UI's screen stack and
     * the app list, a crash leaving the Stopwatch and at every boot). */
    and     r0, r12, #5
    cmp     r0, #5
    bne     2f
3:  mrc     p15, 0, APSR_nzcv, c7, c14, 3   /* test, clean and invalidate */
    bne     3b
    mov     r0, #0
    mcr     p15, 0, r0, c7, c10, 4      /* drain the write buffer */
    mrc     p15, 0, r0, c1, c0, 0
    bic     r0, r0, #0x1000             /* I */
    bic     r0, r0, #0x0004             /* C */
    bic     r0, r0, #0x0001             /* M (the loader's map is identity) */
    mcr     p15, 0, r0, c1, c0, 0
    nop
    nop
    mov     r0, #0
    mcr     p15, 0, r0, c7, c5, 0       /* invalidate the I-cache */
    mcr     p15, 0, r0, c8, c7, 0       /* and the TLBs */
2:  ldr     sp, =__svc_stack_top

    ldr     r0, =__bss_start    /* zero .bss (stacks, the L2 table) */
    ldr     r1, =__bss_end
    mov     r2, #0
1:  cmp     r0, r1
    strlo   r2, [r0], #4
    blo     1b
    ldr     r0, =boot_control   /* for the boot log */
    str     r12, [r0]

    msr     cpsr_c, #0xd2       /* IRQ */
    ldr     sp, =__irq_stack_top
    msr     cpsr_c, #0xd1       /* FIQ */
    ldr     sp, =__fiq_stack_top
    msr     cpsr_c, #0xd7       /* abort */
    ldr     sp, =__exc_stack_top
    msr     cpsr_c, #0xdb       /* undefined */
    ldr     sp, =__exc_stack_top
    msr     cpsr_c, #0xd3       /* back to SVC */

    bl      mmu_setup
    bl      kmain
2:  b       2b

/* Identity MMU with cached PSRAM and the high-vector page; then MMU, the
 * D-cache and the I-cache on. _start has written back and switched off a
 * cache the loader left on, so the invalidates below lose nothing. */
    .type   mmu_setup, %function
mmu_setup:
    ldr     r0, =__l2_vectors   /* va 0xffff0000 -> pa _vectors */
    ldr     r1, =_vectors
    orr     r1, r1, #0x0e
    str     r1, [r0, #0x3c0]

    ldr     r0, =TTB_ADDR       /* 4096 identity sections */
    mov     r1, #0
    mov     r2, #0x1000
    ldr     r3, =0xc12
1:  orr     r4, r1, r3
    str     r4, [r0], #4
    add     r1, r1, #0x100000
    subs    r2, r2, #1
    bne     1b
    ldr     r4, =__l2_vectors   /* entry 0xfff: the coarse table */
    orr     r4, r4, #0x31
    str     r4, [r0, #-4]

    /* The NOR window 0x00000000..0x03ffffff read-only: domain 1 (client),
     * AP=00 with R=1. Nothing writes there but a bug (a NULL pointer's
     * store lands in the flash's addresses): it faults (arch_data_abort). */
    ldr     r0, =TTB_ADDR
    ldr     r3, =0x032
    mov     r4, #0
    mov     r2, #64
4:  orr     r5, r4, r3
    str     r5, [r0], #4
    add     r4, r4, #0x100000
    subs    r2, r2, #1
    bne     4b

    ldr     r0, =TTB_ADDR + 0x40 * 4    /* cache 0x04000000..0x07ffffff */
    ldr     r3, =0xc1e
    ldr     r4, =0x04000000
    mov     r2, #64
2:  orr     r5, r4, r3
    str     r5, [r0], #4
    add     r4, r4, #0x100000
    subs    r2, r2, #1
    bne     2b

    ldr     r0, =TTB_ADDR
    mcr     p15, 0, r0, c2, c0, 0       /* TTBR */
    mov     r1, #0x57
    mcr     p15, 0, r1, c3, c0, 0       /* DACR: d0 manager, d1 client */
    mov     r0, #0
    mcr     p15, 0, r0, c8, c7, 0       /* invalidate TLBs */
    mcr     p15, 0, r0, c7, c6, 0       /* invalidate D-cache */
    mcr     p15, 0, r0, c7, c5, 0       /* invalidate I-cache */
    mcr     p15, 0, r0, c7, c10, 4      /* drain write buffer */
    mrc     p15, 0, r0, c1, c0, 0
    orr     r0, r0, #0x2000             /* V: high vectors */
    orr     r0, r0, #0x0200             /* R: the vector page is AP=00 */
    orr     r0, r0, #0x0001             /* M */
    mcr     p15, 0, r0, c1, c0, 0
    nop
    nop
    mrc     p15, 0, r0, c1, c0, 0
    orr     r0, r0, #0x0004             /* C: D-cache */
    orr     r0, r0, #0x1000             /* I: I-cache */
    mcr     p15, 0, r0, c1, c0, 0
    b       3f
3:  bx      lr
    .ltorg

/* ---- context save / restore -------------------------------------------- */
    .text
    .macro SAVE_CONTEXT         /* lr = the thread's resume address */
    stmdb   sp!, {r0}
    stmdb   sp, {sp}^           /* the thread's sp */
    nop
    sub     sp, sp, #4
    ldmia   sp!, {r0}
    stmdb   r0!, {lr}           /* frame: pc */
    mov     lr, r0
    ldmia   sp!, {r0}
    stmdb   lr, {r0-lr}^        /* frame: r0..r12, sp, lr */
    nop
    sub     lr, lr, #60
    mrs     r0, spsr
    stmdb   lr!, {r0}           /* frame: cpsr */
    ldr     r0, =kernel_current
    ldr     r0, [r0]
    str     lr, [r0]            /* kernel_current->sp */
    .endm

    .macro RESTORE_CONTEXT
    ldr     r0, =kernel_current
    ldr     r0, [r0]
    ldr     lr, [r0]
    ldmia   lr!, {r0}
    msr     spsr_cxsf, r0
    ldmia   lr, {r0-lr}^
    nop
    ldr     lr, [lr, #60]
    movs    pc, lr
    .endm

    .global irq_entry
    .type   irq_entry, %function
irq_entry:
    sub     lr, lr, #4
    SAVE_CONTEXT
    bl      irq_dispatch        /* IRQ mode, IRQ stack, interrupts off */
    RESTORE_CONTEXT

    .global svc_entry
    .type   svc_entry, %function
svc_entry:                      /* thread_yield and blocking waits */
    SAVE_CONTEXT
    bl      kernel_reschedule
    RESTORE_CONTEXT

    .global arch_start_first
    .type   arch_start_first, %function
arch_start_first:               /* SVC mode, interrupts off */
    RESTORE_CONTEXT

    .global arch_yield
    .type   arch_yield, %function
arch_yield:
    svc     #0
    bx      lr

/* First instructions of every thread: r4 = entry, r0 = argument. */
    .global arch_thread_trampoline
    .type   arch_thread_trampoline, %function
arch_thread_trampoline:
    blx     r4
    ldr     r4, =thread_exit
    blx     r4
    b       .
    .ltorg

/* ---- faults: dump to the screen ----------------------------------------- */
    .macro FAULT kind, adjust
    sub     lr, lr, #\adjust
    stmfd   sp!, {r0-r12, lr}
    mov     r0, #\kind
    mov     r1, sp
    mrs     r2, spsr
    ldr     r3, =arch_fault
    blx     r3
    b       .
    .endm

    .type   undef_entry, %function
undef_entry:    FAULT 0, 4
    .type   pabort_entry, %function
pabort_entry:   FAULT 1, 4
/* A data abort inside probe_read/probe_write (arch/sc6530c/probe.S)
 * returns to its fixup address instead, in the mode and state it came
 * from (ARM, interrupts off): the shell's peek and poke. */
    .type   dabort_entry, %function
dabort_entry:
    stmfd   sp!, {r0, r1}
    ldr     r0, =probe_fixup
    ldr     r1, [r0]
    cmp     r1, #0
    beq     1f
    mov     lr, r1
    mov     r1, #0
    str     r1, [r0]
    ldmfd   sp!, {r0, r1}
    movs    pc, lr
1:  ldmfd   sp!, {r0, r1}
/* Else arch_data_abort may send the thread on elsewhere (an app's write
 * into the NOR window: to its end): it returns the SPSR to go on with
 * and has set the pc and r0, r1 in the frame; 0: the fault screen. */
    sub     lr, lr, #8
    stmfd   sp!, {r0-r12, lr}
    mov     r0, sp
    mrs     r1, spsr
    ldr     r3, =arch_data_abort
    blx     r3
    cmp     r0, #0
    beq     2f
    msr     spsr_cxsf, r0
    ldmfd   sp!, {r0-r12, pc}^
2:  mov     r0, #2
    mov     r1, sp
    mrs     r2, spsr
    ldr     r3, =arch_fault
    blx     r3
    b       .
    .type   fiq_entry, %function
fiq_entry:      FAULT 3, 4
    .ltorg

/* ---- interrupts, caches, idle -------------------------------------------- */
    .global arch_irq_save
    .type   arch_irq_save, %function
arch_irq_save:
    mrs     r0, cpsr
    orr     r1, r0, #0x80
    msr     cpsr_c, r1
    bx      lr

    .global arch_irq_restore
    .type   arch_irq_restore, %function
arch_irq_restore:
    mrs     r1, cpsr
    bic     r1, r1, #0x80
    and     r0, r0, #0x80
    orr     r1, r1, r0
    msr     cpsr_c, r1
    bx      lr

    .global arch_in_irq
    .type   arch_in_irq, %function
arch_in_irq:
    mrs     r0, cpsr
    and     r0, r0, #0x1f
    cmp     r0, #0x12           /* IRQ mode */
    cmpne   r0, #0x13           /* SVC: the scheduler itself */
    moveq   r0, #1
    movne   r0, #0
    bx      lr

    .global arch_wait_for_irq
    .type   arch_wait_for_irq, %function
arch_wait_for_irq:
    mov     r0, #0
    mcr     p15, 0, r0, c7, c0, 4
    bx      lr

    .global dcache_clean_range
    .type   dcache_clean_range, %function
dcache_clean_range:             /* r0 = address, r1 = length */
    add     r1, r0, r1
    bic     r0, r0, #(CACHE_LINE - 1)
1:  cmp     r0, r1
    mcrlo   p15, 0, r0, c7, c10, 1
    addlo   r0, r0, #CACHE_LINE
    blo     1b
    mov     r0, #0
    mcr     p15, 0, r0, c7, c10, 4
    bx      lr

    .global dcache_flush_range
    .type   dcache_flush_range, %function
dcache_flush_range:
    add     r1, r0, r1
    bic     r0, r0, #(CACHE_LINE - 1)
1:  cmp     r0, r1
    mcrlo   p15, 0, r0, c7, c14, 1
    addlo   r0, r0, #CACHE_LINE
    blo     1b
    mov     r0, #0
    mcr     p15, 0, r0, c7, c10, 4
    bx      lr

    .global dcache_clean_all
    .type   dcache_clean_all, %function
dcache_clean_all:
1:  mrc     p15, 0, r15, c7, c10, 3     /* test and clean */
    bne     1b
    mov     r0, #0
    mcr     p15, 0, r0, c7, c10, 4
    bx      lr

    .global icache_invalidate_all
    .type   icache_invalidate_all, %function
icache_invalidate_all:
    mov     r0, #0
    mcr     p15, 0, r0, c7, c5, 0
    bx      lr

/* 4 cycles a loop with the I-cache on: 2^udelay_shift loops per us, set
 * by clock.c for the CPU clock (64 at 208 MHz, a little slow on purpose;
 * slower is harmless for the delays that use it). */
    .global udelay
    .type   udelay, %function
udelay:
    ldr     r1, =udelay_shift
    ldr     r1, [r1]
    movs    r0, r0, lsl r1
    bxeq    lr
1:  subs    r0, r0, #1
    bne     1b
    bx      lr
    .ltorg

    .bss
    .global boot_control
    .align  2
boot_control:                   /* the CP15 control register at entry */
    .space  4

/* ---- the stop of a sleep (sleep.c) ----------------------------------------
 * Copied to IRAM and run there, interrupts off, no PSRAM access. r0: 0 the
 * system may stop (once the memory controller is idle, a bounded wait; stock
 * waits for ever), 1 the core only. Stock's sequence (NOR 0x12e1c4). The WFI
 * is ours: on the chip the wake's line is pending, so it passes at once;
 * QEMU does not model the stop bit and waits there. */
    .section .text.sleep_stop, "ax", %progbits
    .global sleep_stop_code
    .global sleep_stop_end
sleep_stop_code:
    mov     r1, #0
    mcr     p15, 0, r1, c7, c10, 4      /* drain the write buffer */
    cmp     r0, #0
    bne     3f
    ldr     r1, =0x20000008
    mov     r2, #0x10000
1:  ldr     r3, [r1]
    tst     r3, #4
    beq     2f
    subs    r2, r2, #1
    bne     1b
2:  ldr     r1, =0x205000d4
    ldr     r3, [r1]
    orr     r3, r3, #0x18
    str     r3, [r1]
3:  mov     r1, #0x20
4:  subs    r1, r1, #1
    bne     4b
    ldr     r1, =0x205000d0
    ldr     r3, [r1]
    orr     r3, r3, #2
    str     r3, [r1]
    nop
    nop
    nop
    mov     r1, #0x20
5:  subs    r1, r1, #1
    bne     5b
    mov     r1, #0
    mcr     p15, 0, r1, c7, c0, 4
    bx      lr
    .ltorg
sleep_stop_end:

/* ---- chain to another image (arch_chain, power.c) -----------------------
 * Copied to IRAM and run there: r0 the image, r1 its bytes (a multiple of
 * 4), r2 where it goes and is entered (0x04000000, over this OS). The
 * D-cache is written back, MMU and caches go off (the map is identity, so
 * the addresses stay), then the copy and the jump: the state sdboot leaves
 * for the boot menu and the boot menu leaves for us. Interrupts are off. */
    .section .text.chain, "ax", %progbits
    .global chain_tramp
    .global chain_tramp_end
chain_tramp:
    mov     r4, r2
1:  mrc     p15, 0, APSR_nzcv, c7, c14, 3   /* test, clean and invalidate */
    bne     1b
    mov     r3, #0
    mcr     p15, 0, r3, c7, c10, 4      /* drain the write buffer */
    mrc     p15, 0, r3, c1, c0, 0
    bic     r3, r3, #0x1000             /* I */
    bic     r3, r3, #0x0005             /* C, M */
    mcr     p15, 0, r3, c1, c0, 0
    nop
    nop
    mov     r3, #0
    mcr     p15, 0, r3, c7, c5, 0       /* invalidate the I-cache */
    mcr     p15, 0, r3, c8, c7, 0       /* and the TLBs */
2:  ldr     r3, [r0], #4
    str     r3, [r2], #4
    subs    r1, r1, #4
    bgt     2b
    mov     r3, #0
    mcr     p15, 0, r3, c7, c10, 4
    mov     pc, r4
chain_tramp_end:
