@ -*- tab-width: 8 -*-
@ B310E-OS — arch/menu_copy.s
@
@ The old IRAM copy+launch stub (menu_copy_stub) is RETIRED (2026-08-27):
@ the menu now links at the TOP of PSRAM (0x34300000) and streams launched
@ images into the 3 MB region below it (0x34000000..0x34300000) with plain
@ C memcpy per 96 KB chunk — the menu SURVIVES the copy, so no IRAM stub is
@ needed. The launch sequence keeps the old stub's two cache ops (mcr c7,
@ c10,1 clean D-cache + mcr c7,c5,0 invalidate I-cache) — now as an ARM
@ helper, menu_flush_caches() — because Thumb-1 C code has no MCR (the
@ linker inserts the interworking veneer at the call site, same as
@ clean_dcache in arch/cache.s).
@
@ IMPORTANT: no literal pool (no `ldr rX, =const`) — the assembler can
@ place the pool OUTSIDE the copied range; all constants are MOV/ORR
@ immediates.

	.arch armv5te
	.syntax unified
	.code 32

	.section .text.menu_copy, "ax", %progbits
	.p2align 2
	.global menu_flush_caches
menu_flush_caches:
	mov	r0, #0
	mcr	p15, 0, r0, c7, c10, 1	@ clean entire D-cache
	mcr	p15, 0, r0, c7, c5, 0	@ invalidate entire I-cache
	bx	lr
	.size	menu_flush_caches, . - menu_flush_caches

@ __gnu_thumb1_case_uhi: Thumb-1 unsigned-16-bit switch dispatch (libgcc
@ _thumb1_case_uhi.o, verbatim shape). drivers/arm_helpers.c provides only
@ _shi/_sqi/_uqi; the menu's keypad switch (menu_main) needs the halfword
@ table, and drivers/ is out of bounds for the menu, so the helper lives
@ here. Called as `bl` from Thumb C with lr = inline table address and
@ r0 = index: computes lr = table + 2 * unsigned halfword table[index]
@ and returns there via bx (the Thumb bit is already set in lr).
	.section .text.menu_uhi, "ax", %progbits
	.code 16
	.thumb_func
	.global __gnu_thumb1_case_uhi
__gnu_thumb1_case_uhi:
	push	{r0, r1}
	mov	r1, lr
	lsrs	r1, r1, #1
	lsls	r0, r0, #1
	lsls	r1, r1, #1
	ldrh	r1, [r1, r0]
	lsls	r1, r1, #1
	add	lr, r1
	pop	{r0, r1}
	bx	lr
	.size	__gnu_thumb1_case_uhi, . - __gnu_thumb1_case_uhi
	.code 32

@ Boot-stock stub: clear MEM_REMAP, RE-RUN the SMC init (the routine is
@ passed in r0 - resident in IRAM at 0x40009c00; it reads MEM_REMAP and
@ reconfigures the PSRAM window at 0x04000000, which the plain remap
@ clear alone may not move), and jump to the stock PBL entry (0x46e4).
@ The MENU writes the warm-boot magic (0xFE519C04) to its own PSRAM base
@ first, so after the window moves 0x04000000 holds the magic and the
@ PBL takes its FAST path: it skips all init and jumps straight to the
@ main OS at 0x10000 - the original firmware boots in place, no power
@ cycle, no END-key needed. Runs from IRAM (the window move unmaps the
@ menu's PSRAM). Debug: the keylight is ON at entry and OFF after the
@ window move, so libc-less progress is visible on the phone.
@
@ Entry: r0 = SMC init routine address (IRAM). Uses r0-r5.
@ IMPORTANT: no `bl <imm>` to PSRAM-resident code - the linker would
@ insert a veneer in the menu's PSRAM, which is unmapped after the move.
@ The SMC call is register-based (blx r0) so the stub stays IRAM-local.
@
@ NOTE: UNUSED since 2026-08-23 (menu_reboot's watchdog + HWRST1 clear is
@ the BOOT STOCK path) - kept as the historical record of the direct-jump
@ approach (abandoned: the stock OS needs the PBL's full init).
	.p2align 2
	.global menu_stock_stub_start
menu_stock_stub_start:
	adr	r4, klit
	ldr	r4, [r4]			@ r4 = 0x82001224 (keylight)
	mov	r5, #0xe0
	str	r5, [r4]			@ keylight ON: stub entered
	mov	r6, #0x01000000
1:	subs	r6, r6, #1
	bne	1b				@ hold ON ~0.3 s (visible)
	mov	r1, #0x20000000
	orr	r1, r1, #0x00500000
	orr	r1, r1, #0xe0			@ r1 = 0x205000e0
	ldr	r2, [r1]
	bic	r2, r2, #1
	str	r2, [r1]			@ MEM_REMAP = 0
	blx	r0				@ re-run the SMC init (r0 passed in)
	mov	r5, #0x10
	str	r5, [r4]			@ keylight OFF: window moved
	mov	r6, #0x01000000
2:	subs	r6, r6, #1
	bne	2b				@ hold OFF ~0.3 s (visible)
	@ Jump STRAIGHT to the stock OS full-boot continuation (0xbf1b0),
	@ bypassing the OS entry 0xbf150: its ADI mailbox read of reg 0x140
	@ + compare can take the wrong (assert) path when the value does not
	@ match our non-stock handoff state, hanging the boot. 0xbf1b0
	@ re-does `mov fp, sp` itself and starts the RTOS.
	mov	r0, #0x000B0000
	orr	r0, r0, #0x0000F000
	orr	r0, r0, #0x00000100
	orr	r0, r0, #0xb0			@ r0 = 0xbf1b0
	bx	r0				@ -> stock OS full boot
klit:
	.word	0x82001224
	.global menu_stock_stub_end
menu_stock_stub_end:

	.p2align 2
	.end
