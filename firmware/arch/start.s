/* B310E-OS — arch/start.s
 *
 * Boot stub for Spreadtrum SC6530C (ARM926EJ-S @ 208 MHz, ARMv5TE).
 * Linked at 0x14000000 (the fpdoom app window) and built -pie: the image
 * ships with a pack_reloc .rel table appended, and this stub applies it
 * (diff = runtime - link) so the SAME binary runs at 0x14000000 (the
 * sdboot readbin target — diff 0, in place) or at 0x34000000 (spd_dump
 * `fdl os.bin ram`, the SC6530 ram_addr — self-relocated).
 *
 * NOTE: entry must be ARM state — Thumb-1 cannot mask IRQ/FIQ or switch
 * modes via CPSR. C code (main) is compiled -mthumb; the linker inserts an
 * interworking veneer for the `bl main` below.
 *
 * Entry sequence byte-matches fpdoom's PROVEN boot path (start1.s, Unlicense):
 * CP15 setup FIRST (ICache invalidate + control reg), then SYS mode (0xdf),
 * then the low IRAM stack at __stack_bottom (0x40009000) — the only stack
 * window guaranteed valid before iram(1..3) is enabled later by
 * sc6530_init_iram (0x8b0001a0 |= 7<<19) in sc6530_chip_init(). Then the
 * relocation (apply_reloc), then the bss clear RELATIVE to the runtime
 * base (the absolute link-time bss symbols are wrong when relocated).
 */

    .syntax unified
    .arch   armv5te
    .arm

    .section .text.start, "ax", %progbits

    /* Offset 0 of the image: branch-to-entry, mimicking a reset vector
     * (objdump: ea0000xx  b <_start>). Boot ROM jumps here -> enters _start. */
    .globl  _start
    b       _start
    .word   0
    .word   0
    .word   0

    .globl  _start
_start:
    /* 1. CP15 setup — fpdoom start1.s:24-32 verbatim (Unlicense). MUST run
     *    first, before switching mode or touching the stack: if FDL1 or the
     *    boot ROM left the MMU enabled or the ICache in an unknown state,
     *    the very first instruction fetch can fault and the CPU locks up
     *    before anything (even the fdl_ack) is sent.
     *    Control reg bits: 0 MMU, 1 Alignment, 2 D-cache, 8 System prot,
     *    9 ROM prot, 12 ICache, 13 high vectors. MMU stays OFF (bit 0). */
    mov     r1, #0
    mcr     p15, #0, r1, c7, c5, #0   /* Invalidate ICache */
    mrc     p15, #0, r0, c1, c0, #0   /* Read Control Register */
    bic     r0, #5                    /* clear bit0 MMU + bit2 D-cache */
    bic     r0, #0x100                /* clear bit8 SYS protection */
    orr     r0, #2                    /* set bit1 Alignment */
    orr     r0, #0x3200               /* set bit13 high vectors + bit12
                                         ICache + bit9 ROM prot */
    mcr     p15, #0, r0, c1, c0, #0   /* Write Control Register */

    /* 2. SYS mode (M[4:0] = 0b11111), mask IRQ (bit 7) + FIQ (bit 6) -> 0xdf.
     *    Matches fpdoom start1.s:33. */
    msr     cpsr_c, #0xdf

    /* Backlight marker: os.bin's start.s is executing. The menu's readbin
     * launch sets a mid brightness before the jump; this write (FIFO-gated
     * ADI mailbox, led.c pattern) pushes the panel to FULL bright, so a
     * hang in the readbin/jump (no change) vs inside os.bin's boot (bright)
     * is visible without the LCD. r4-r6 only; no stack.
     *
     * BOUNDED waits (r7 = budget): the FIFO state is not guaranteed idle
     * here — the menu's last LCD write may still be draining, and an
     * unbounded spin would hang every launch when it isn't. A budget lets
     * the marker degrade to "no visible change" (same as "readbin hung")
     * instead of wedging the boot. */
    mov     r7, #0x40000         /* ~1M-iteration budget */
    ldr     r4, =0x82000020      /* ADI FIFO sts (bit 9 full, bit 8 empty) */
1:  ldr     r5, [r4]
    tst     r5, #0x200           /* bit9 = FIFO_FULL */
    beq     5f                   /* not full: proceed to the write */
    subs    r7, r7, #1
    bne     1b
5:  mov     r5, #0x1100
    orr     r5, r5, #0x5f        /* bright (100%) */
    ldr     r6, =0x82001220      /* ANA_WHTLED_CTRL (backlight) */
    str     r5, [r6]
    mov     r7, #0x40000
2:  ldr     r5, [r4]
    tst     r5, #0x100           /* bit8 = FIFO_EMPTY */
    bne     4f                   /* empty: done */
    subs    r7, r7, #1
    bne     2b
4:

    /* 3. Stack: low IRAM window, grows down from __stack_bottom (0x40009000).
     *    iram(1..3) above 0x40009000 is NOT enabled until sc6530_init_iram
     *    (0x8b0001a0 |= 7<<19) runs in sc6530_chip_init(); the high
     *    __stack_top (0x40022000) must not be touched before then. */
    ldr     sp, =__stack_bottom

    /* 4. Relocate for the runtime base (fpdoom start1.s:41-57): the image
     *    is linked at 0x14000000, but the USB loader (spd_dump ram) puts it
     *    at 0x34000000. diff = runtime - link; if nonzero, apply the .rel
     *    table (appended to the .bin, at runtime + __image_size). r0-r3 are
     *    saved across apply_reloc (it clobbers them).
     *
     *    NOTE the bases: pack_reloc encodes relocation offsets relative to
     *    e_entry (= _start, 0x14000010), and apply_reloc walks from r0 =
     *    runtime _start — so the diff must be runtime _start - link _start
     *    (= runtime __image_start - link __image_start), and the .rel table
     *    sits at runtime __image_start + __image_size (= runtime _start +
     *    __image_size - 0x10, because the reset-vector preamble puts _start
     *    at image offset 0x10). The old code subtracted link __image_start
     *    and used runtime _start + __image_size — both +0x10 off, which
     *    misread the table header (nbits=47) and shifted every relocated
     *    pointer by 16 bytes (os.bin froze in chip_init on USB, at
     *    sched_start on SD). The games are immune: fpdoom's _start IS at
     *    image offset 0. */
    adr     r0, _start          /* r0 = runtime _start (walk base, e_entry) */
    ldr     r1, =__image_size
    ldr     r2, =__bss_size
    push    {r0-r3}
    ldr     r3, =_start         /* link _start (0x14000010) — read pre-reloc */
    subs    r2, r0, r3          /* diff = runtime _start - link _start */
    add     r1, r0              /* r1 = runtime _start + image_size */
    sub     r1, r1, #0x10       /* ... - preamble = runtime base + image_size */
    blne    apply_reloc
    pop     {r0-r3}             /* r0 = runtime _start, r1 = image_size,
                                   r2 = bss_size */

    /* 5. r0 = runtime IMAGE BASE (the 4-word reset-vector preamble puts
     *    _start at image offset 0x10). The C entry (entry_main) uses r0
     *    to locate bss and the heap — it must be the base, not _start. */
    sub     r0, r0, #0x10

    /* 6. Clear .bss RELATIVE to the runtime base: [base+image_size,
     *    base+image_size+bss_size) <- 0. Uses r4-r6 ONLY — r1
     *    (image_size) and r2 (bss_size) are ARGUMENTS to the C entry and
     *    must survive. (The old code clobbered them: entry_main received
     *    r1 = bss_end and r2 = 0, so its heap base landed at ~0x080058c8
     *    — just past the 4 MB PSRAM window end 0x08000000 — and the first
     *    malloc header write stalled the AHB: the fpload-our "full-bright
     *    then hang".) */
    mov     r4, r0
    add     r4, r4, r1          /* r4 = bss_start = base + image_size */
    add     r4, r4, #3          /* word-align bss_start: image_size is the
                                   .bin length (.text+.rodata+.data) and is
                                   NOT word-aligned (e.g. os.bin % 4 == 2).
                                   The bss-clear str [r4],#4 below runs with
                                   the CP15 A-bit (alignment) SET from the
                                   entry CP15 setup — an unaligned store is a
                                   data abort, so the WHOLE relocatable
                                   family (os.bin/os-sd.bin/os-dsp-boot.bin:
                                   spd_dump at 0x34000000 or the fpmain
                                   readbin at 0x04000000) hung before main().
                                   Aligning up overlaps ≤3 bytes of the .rel
                                   table tail at base+image_size — harmless,
                                   apply_reloc consumed it above. */
    bic     r4, r4, #3
    add     r5, r4, r2          /* r5 = bss_end */
    mov     r6, #0
1:  cmp     r4, r5
    bcs     2f
    str     r6, [r4], #4
    b       1b
2:

    /* 7. Call the C entry (fpdoom's proven `ldr pc, =sym` form: loads the
     *    relocated address from the literal pool — NO interworking veneer
     *    for the ARM->Thumb call). r0 = runtime image base,
     *    r1 = image_size, r2 = bss_size. The fpload-our diagnostic
     *    (-DFPLOAD_ENTRY) calls the framework's entry_main() directly —
     *    the SAME entry the working fpload.bin uses. All other targets
     *    call main(). */
#ifdef FPLOAD_ENTRY
    ldr     pc, =entry_main
#else
    ldr     pc, =main
#endif

.Lhalt:
    b       .Lhalt             /* main returned — park the CPU */

    /* fpdoom start.s apply_reloc (Unlicense): unpack the pack_reloc format.
     * r0 = image, r1 = reloc table, r2 = diff. Clobbers r0-r3, r4-r7, ip, lr. */
    .type   apply_reloc, %function
    .global apply_reloc
apply_reloc:
    push    {r4-r7,lr}
    mov     r5, #1
    ldrb    r6, [r1], #1    /* nbits */
    ldrb    r7, [r1], #1    /* maxrun */
1:  mov     ip, #0
    mov     lr, r7
2:  ldrb    r4, [r1], #1
    sub     lr, lr, r4, lsl ip
    lsrs    r4, r6
    add     ip, r6
    beq     2b
    adds    lr, lr, r5, lsl ip
    popeq   {r4-r7,pc}
    asrs    ip, lr, #1
    mvncc   r3, #0
    movmi   r3, lr
3:  ldr     r4, [r0, -r3, lsl #2]!
    add     r4, r2
    str     r4, [r0]
    subs    ip, #1
    bpl     3b
    b       1b

    .ltorg                     /* literal pool for the stack/bss/reloc symbols */
    .size   _start, . - _start
