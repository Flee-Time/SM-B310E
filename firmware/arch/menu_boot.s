@ -*- tab-width: 8 -*-
@ B310E-OS — arch/menu_boot.s
@
@ The MERGED boot stub (replaces the fpdoom sdboot loader). Placed at
@ image offset 0 (before .text.start) and entered by the NOR boot vector
@ at 0x3fc000 (installed) or by spd_dump at the load address (USB test).
@
@ BOOT DECISION IS MADE HERE, BEFORE TOUCHING ANYTHING: the keypad is
@ probed first (minimal pre-init sequence, no chip init — the same
@ registers sdboot's read_key uses). If a key is held, OR no stock entry
@ is installed (header +4 = 0xffffffff, the USB-test case), the stub
@ brings up the PSRAM window at the LINK base (0x34300000), copies the
@ image there and jumps to _start (which runs the C menu). If NO key and
@ a stock entry IS installed, the stub jumps to it with the chip in its
@ PRISTINE power-on state — no clock change, no PSRAM remap — so the
@ stock PBL boots exactly as on an unpatched phone. (Doing the key check
@ after remapping, then jumping to stock, was the fragile "direct jump
@ from an initialized state" approach that hung — learnings #14-17.)
@
@ WHY the PSRAM setup here (the NOR power-on + key-held case): the image
@ is linked for 0x34300000, but at a plain power-on the PSRAM sits at the
@ STOCK window 0x04000000 (MEM_REMAP=0). Copying to 0x34300000 and
@ jumping there without moving the window writes to unmapped memory and
@ faults ("doesn't power on" — the first NOR-install failure, 2026-08-23).
@ The USB/FDL path worked because FDL1 already remapped to 0x34000000. So
@ the stub repeats the proven sdboot dance: 208 MHz selector, MEM_REMAP=1,
@ then re-run the SMC init (it reads MEM_REMAP and programs the SDRAM
@ base accordingly). The SMC re-init runs from NOR XIP / IRAM here — never
@ from SDRAM — so it cannot hang the bus (the arch/smc_init.s rule).
@
@ Header (link addresses = 0x34300000-based):
@   +0x00  b stub_body
@   +0x04  0xffffffff  original stock entry (install writes 0x46e4)
@   +0x08  __image_size   (linker-fill: total loadable bytes)
@   +0x0c  __start_offset (linker-fill: _start - 0x34300000)
@   +0x10  __smc_offset   (linker-fill: SMC routine offset in image)
@   +0x14  .long 0x8b000040  (CPU freq selector reg)
@   +0x18  .long 0x205000e0  (MEM_REMAP reg)
@ The stub body is fully position-independent: it computes its runtime
@ base via adr, so it runs equally from 0x3fc000 (NOR XIP), 0x40004000
@ (spd_dump FDL slot) or 0x34300000 (its own copy destination).

	.arch armv5te
	.syntax unified
	.code 32

	.section .text.boot, "ax", %progbits
	.p2align 2
	.global menu_boot_start
menu_boot_start:
	b	stub_body
	.long	0xffffffff		@ +0x04: original stock entry
	.long	__image_size		@ +0x08
	.long	__start_offset		@ +0x0c
	.long	__smc_offset		@ +0x10: SMC routine offset in image
	.long	0x8b000040		@ +0x14: CPU freq selector reg
	.long	0x205000e0		@ +0x18: MEM_REMAP reg
stub_body:
	adr	r0, menu_boot_start	@ r0 = runtime base
	mov	r4, r0			@ save base (header reads later)

#ifndef MENU_NO_KEYCHECK
	@ ---- BOOT DECISION: keypad probe FIRST, chip untouched. ----------
	@ Minimal pre-init keypad check (sdboot read_key port, no chip init):
	@ power on 0x8b0000a0 |= 0x80040, AHB reset 0x8b000060=1/DELAY/0x8b000064=1,
	@ init regs, settle DELAY(200000), read int_raw. r5 = 1 if any key.
	mov	r3, #0x8b000000
	orr	r3, r3, #0xa0		@ 0x8b0000a0 (keypad APB power)
	ldr	r1, [r3]
	mov	r2, #0x80000
	orr	r2, r2, #0x40		@ 0x80040 (keypad power bits)
	orr	r1, r1, r2
	str	r1, [r3]
	mov	r3, #0x8b000000
	orr	r3, r3, #0x60		@ 0x8b000060 (AHB reset assert)
	mov	r1, #1
	str	r1, [r3]
	mov	r2, #10
1:	subs	r2, r2, #1
	bne	1b
	add	r3, r3, #4		@ 0x8b000064 (AHB reset deassert)
	str	r1, [r3]

	mov	r3, #0x87000000		@ keypad controller
	mov	r2, #0xff0
	orr	r2, r2, #0xf		@ 0xfff
	str	r2, [r3, #0x10]		@ int_clr = 0xfff
	mov	r1, #1
	str	r1, [r3, #0x28]		@ clk_divide = 1
	mov	r1, #16
	str	r1, [r3, #0x1c]		@ debounce = 16
	str	r2, [r3, #0x04]		@ int_en = 0xfff
	mov	r1, #0xff00
	orr	r1, r1, #0xff		@ 0xffff
	str	r1, [r3, #0x18]		@ polarity = 0xffff
	ldr	r1, [r3]		@ ctrl
	orr	r1, r1, #1		@ enable
	bic	r1, r1, #2		@ clear sleep
	mov	r2, #0xfc00
	bic	r1, r1, r2		@ clear old row/col masks
	mov	r2, #0xfc0000
	bic	r1, r1, r2
	mov	r2, #0x1c00
	orr	r1, r1, r2		@ rows/cols 2-7 (0x1c)
	mov	r2, #0x1c0000
	orr	r1, r1, r2
	str	r1, [r3]
	mov	r2, #0x1e0000		@ DELAY(200000) settle (~40 ms @
	orr	r2, r2, #0x8400		@  208 MHz; longer at the boot clock)
	orr	r2, r2, #0x80
1:	subs	r2, r2, #1
	bne	1b
	ldr	r5, [r3, #0x08]		@ int_raw
	cmp	r5, #0
	bne	.Lboot_menu		@ key held -> the menu

	@ No key: chain to the original stock entry with the chip UNTOUCHED
	@ (no clock change, no PSRAM remap) — the stock PBL boots normally.
	ldr	r1, [r4, #4]		@ original stock entry (install: 0x46e4)
	cmp	r1, #0xffffffff
	bne	.Lstock		@ installed: pristine jump to stock
	@ Nothing installed (header +4 = 0xffffffff — the USB-test case):
	@ fall through to the menu so the test is usable without a key.
#endif /* !MENU_NO_KEYCHECK */

.Lboot_menu:
	@ Already at a valid PSRAM window? Two cases skip the remap + SMC
	@ (re-running SMC while fetching from SDRAM hangs the bus):
	@  - running at 0x34000000 (legacy spd_dump 'ram' slot: FDL1 loaded the
	@    image straight into the FDL window; MEM_REMAP already 1). The copy
	@    below then just moves the image up to the link base.
	@  - running at the LINK base 0x34300000 (the image was already copied
	@    here — e.g. a re-entrant load at the final address).
	cmp	r0, #0x34000000
	beq	.Lgo
	mov	r2, #0x34000000
	orr	r2, r2, #0x00300000	@ r2 = 0x34300000 (link base; MOV/ORR —
					@ 0x34300000 is not a single ARM imm)
	cmp	r0, r2
	beq	.Lgo

	@ NOR power-on / FDL slot: bring the PSRAM window up at 0x34000000.
	@ Stack first — the SMC routine pushes {r4-r9, lr}.
	mov	sp, #0x40000000
	orr	sp, sp, #0x9000		@ 0x40009000 (low IRAM, valid pre-init)

	@ 208 MHz (sc6530_init_freq: a |= 4; (a & ~3) | 1) — same freq-then-
	@ SMC order as fpdoom's init_sc6530.
	ldr	r1, [r0, #0x14]		@ freq reg
	ldr	r3, [r1]
	orr	r3, r3, #4
	bic	r3, r3, #3
	orr	r3, r3, #1
	str	r3, [r1]
	mov	r2, #1000
1:	subs	r2, r2, #1
	bne	1b

	@ MEM_REMAP = 1 — move the PSRAM window to 0x34000000.
	ldr	r1, [r0, #0x18]
	ldr	r3, [r1]
	orr	r3, r3, #1
	str	r3, [r1]

	@ Re-run the SMC init from its in-image position (it reads MEM_REMAP
	@ and programs the SDRAM base). Runs from NOR XIP / IRAM — not SDRAM.
	ldr	r1, [r0, #0x10]		@ smc_off
	add	r3, r0, r1		@ runtime address of sc6530_init_smc_asm
	blx	r3

.Lgo:
	mov	r0, r4			@ runtime base (r0 was clobbered by SMC)
	ldr	r2, [r4, #8]		@ r2 = __image_size (bytes)
	mov	r1, #0x34000000		@ dst = link base
	orr	r1, r1, #0x00300000	@   ... = 0x34300000
	add	r2, r2, #3
	bic	r2, r2, #3		@ round up to whole words
2:	ldr	r3, [r0], #4
	str	r3, [r1], #4
	subs	r2, r2, #4
	bne	2b
	ldr	r0, [r4, #12]		@ __start_offset
	mov	r1, #0x34000000		@ copied image base (constant - NOT the
	orr	r1, r1, #0x00300000	@   runtime base, which is 0x3fc000 when
					@   NOR-installed) -> 0x34300000
	add	r0, r1, r0		@ 0x34300000 + offset = copied _start
	bx	r0

.Lstock:
	bx	r1			@ stock entry, chip in pristine state
	.end
