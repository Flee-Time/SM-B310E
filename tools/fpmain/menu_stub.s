@ B310E-OS — tools/fpmain/menu_stub.s
@
@ IRAM-resident copy+launch stub for the ported fpmain boot menu.
@
@ The menu reads the selected firmware into an IRAM buffer, copies THIS
@ stub to IRAM (0x4000a000) and enters it with:
@   r0 = byte count (rounded up to 4), r1 = source (IRAM buffer),
@   r2 = SMC init routine address (resident in IRAM at 0x40009c00,
@   entry.c SMC_INIT_BUF — the fpmain framework copied it there).
@ The stub:
@   0. LCDC refresh: waits for the in-flight DMA burst to complete, then
@      disables it (irq.en/irq.clr + ctrl bit 3). Required BEFORE the
@      remap flip — the DMA reads PA 0x04000000 and once the PSRAM leaves
@      that window it pushes garbage to the panel and its read of the
@      emptied window stalls the AHB bus, hanging the phone ("LCDC DMA
@      bypasses the MMU", docs/stockram.md). The panel holds its last
@      GRAM = the "STARTING..." view through the boot.
@   1. sets MEM_REMAP=1 (0x205000e0 bit 0) — the sdboot NOR-boot context
@      runs with the PSRAM in the 0x04000000 window, but our os.bin is
@      linked for the FDL window 0x34000000;
@   2. re-runs the SMC init via blx r2 — the routine reads MEM_REMAP and
@      reconfigures the PSRAM window at the computed base (the plain
@      remap bit alone may not move it);
@   3. streams the image down to 0x34000000 — overwriting the menu's own
@      PSRAM image (harmless: the stub runs from IRAM);
@   4. INVALIDATES the D-cache (never cleans — the framework's dirty
@      0x04000000 lines would write back into the emptied window after the
@      remap flip and stall the AHB) + the I-cache, then jumps to
@      0x34000000.
@
@ Progress markers via the BACKLIGHT (ANA_WHTLED_CTRL 0x82001220 — the one
@ channel proven visible in the fpmain context; the keypad-light block is
@ never powered by the framework, so 0x82001224 markers never showed).
@ Bright = 0x1100|0x40|0x1f (0x115F, ~100%), dim = 0x1100|0x40|0x02
@ (0x1142, ~10%). The register is ADI-mapped: writes MUST go through the
@ mailbox FIFO gate (led.c led_adi_write: wait FIFO-not-full, write, wait
@ FIFO-empty). A bare write with the FIFO busy STALLS the AHB.
@ r5 = FIFO sts 0x82000020 (bit 9 full, bit 8 empty), r6 = 0x82001220.
@
@ Entry: r0 = size (multiple of 4), r1 = source buffer, r2 = SMC routine.
@ Uses:  r0-r8, lr (the stack is IRAM — survives the window move).
@
@ IMPORTANT: no literal pool (no `ldr rX, =const`) — the assembler can
@ place the pool OUTSIDE menu_stub_start..end, so the menu would copy a
@ stub with a dangling reference. All constants are MOV/ORR immediates;
@ the source and SMC addresses come in registers.

	.arch armv5te
	.syntax unified
	.code 32

	.section .text.menu_stub, "ax", %progbits
	.p2align 2
	.global menu_stub_start
menu_stub_start:
	push	{r0, r1, lr}		@ save size/src across the calls below
	mov	r5, #0x82000000
	orr	r5, r5, #0x20		@ r5 = 0x82000020 (ADI FIFO sts)
	mov	r6, #0x82000000
	orr	r6, r6, #0x00120000
	orr	r6, r6, #0x20		@ r6 = 0x82001220 (backlight)
	mov	r7, #0x1100
	orr	r7, r7, #0x5f		@ bright (100%)
	bl	marker			@ BRIGHT: stub entered
	mov	r8, #0x08000000		@ ~1.2 s hold — the entry marker must
9:	subs	r8, r8, #1		@ be unmissable (the C-side left the
	bne	9b			@ panel VERY DIM right before the call)

	@ LCDC refresh: wait for the in-flight DMA burst, then disable it.
	mov	r3, #0x20000000
	orr	r3, r3, #0x00d00000	@ r3 = 0x20d00000 (LCDC base)
	ldr	r4, [r3, #0x110]	@ irq.en
	tst	r4, #1
	beq	2f			@ no refresh armed - skip the wait
	mov	r8, #0x01000000		@ bounded poll (~0.5 s @ 208 MHz)
1:	ldr	r4, [r3, #0x11c]	@ irq.raw
	tst	r4, #1
	bne	3f
	subs	r8, r8, #1
	bne	1b
3:	ldr	r4, [r3, #0x110]	@ irq.en &= ~1 (no more refresh IRQs)
	bic	r4, r4, #1
	str	r4, [r3, #0x110]
	ldr	r4, [r3, #0x114]	@ irq.clr |= 1 (drop the latched raw)
	orr	r4, r4, #1
	str	r4, [r3, #0x114]
	ldr	r4, [r3]		@ ctrl &= ~8 (clear the refresh start bit)
	bic	r4, r4, #8
	str	r4, [r3]
	mov	r7, #0x1100
	orr	r7, r7, #0x42		@ dim (~10%)
	bl	marker			@ DIM: LCDC refresh stopped
2:	mov	r3, #0x20000000
	orr	r3, r3, #0x00500000
	orr	r3, r3, #0xe0		@ r3 = 0x205000e0 (MEM_REMAP ctrl)
	ldr	r4, [r3]
	orr	r4, r4, #1
	str	r4, [r3]		@ MEM_REMAP = 1 (PSRAM -> FDL window)
	blx	r2			@ re-run the SMC init (addr in r2)
	mov	r7, #0x1100
	orr	r7, r7, #0x5f		@ bright
	bl	marker			@ BRIGHT: SMC re-init done
	pop	{r0, r1, lr}		@ restore size/src
	mov	r2, #0x34000000		@ dst: our os.bin load address
4:	ldr	r3, [r1], #4
	str	r3, [r2], #4
	subs	r0, r0, #4
	bne	4b
	mov	r7, #0x1100
	orr	r7, r7, #0x42		@ dim
	bl	marker			@ DIM: copy done
	mov	r0, #0
	mcr	p15, 0, r0, c7, c6, 0	@ invalidate entire D-cache (NO write-back)
	mcr	p15, 0, r0, c7, c5, 0	@ invalidate entire I-cache
	mov	r7, #0x1100
	orr	r7, r7, #0x5f		@ bright
	bl	marker			@ BRIGHT: jumping to os.bin
	mov	r0, #0x34000000
	bx	r0

@ Backlight marker via the ADI mailbox (led.c led_adi_write pattern).
@ r6 = backlight reg, r7 = value, r5 = FIFO sts, r4/r8 scratch. Bounded.
	.p2align 2
marker:
	mov	r8, #0x01000000		@ bounded wait for FIFO not-full
5:	ldr	r4, [r5]
	tst	r4, #0x200		@ BIT_FIFO_FULL
	beq	7f
	subs	r8, r8, #1
	bne	5b
7:	str	r7, [r6]		@ the keylight write
	mov	r8, #0x01000000		@ bounded wait for FIFO empty
6:	ldr	r4, [r5]
	tst	r4, #0x100		@ BIT_FIFO_EMPTY
	bne	8f
	subs	r8, r8, #1
	bne	6b
8:	bx	lr
	.global menu_stub_end
menu_stub_end:
