@ -*- tab-width: 8 -*-
@ Diagnostic boot stub for the stock-RAM-boot hardware iteration (v3).
@
@ Replaces the stock main-OS entry for ONE hardware test. The boot vector
@ (image offset 0x10020) is patched from 0xbf150 to 0xE000 (a free area in
@ the dead PBL region - the PBL never runs in our RAM-boot). This stub
@ replicates the stock entry's ADI mailbox read (reg 0x140 + busy spin) and
@ signals each step. ALL keylight writes use the PROVEN stock RMW pattern
@ (led_keylight_set, HW-verified) - direct writes may not take effect.
@
@   keylight ON  (shim marker) .............. shim ran (MMU on)
@   OFF ~0.8s then ON (alias snippet) ....... aliased section 0 EXECUTES
@   OFF (stub entry, then stays OFF) ........ stub reached = boot vector
@       dispatch worked; compare MISMATCH -> full boot path (0xbf1b0)
@   OFF then back ON (stub) ................. compare MATCH -> boot-mode/
@       assert path (0x105bc)
@   OFF with no ~0.8s hold .................. an exception fired (vector
@       table) - prefetch/data abort in the aliased execution
@
@ Then it hands off to the REAL entry at 0xbf150 so the natural dispatch
@ still runs. Assembly for arm-none-eabi; spliced by diag-pack.ps1.

	.arch armv5te
	.syntax unified
	.code 32

	.section .text, "ax", %progbits
	.p2align 2
	.global _start
_start:
	@ --- marker: stub reached = boot vector dispatch worked ---
	@ (keylight OFF via the proven RMW pattern)
	ldr	r0, =0x82001224
	ldr	r1, [r0]
	bic	r1, r1, #0x30
	str	r1, [r0]
	ldr	r1, [r0]
	orr	r1, r1, #0x10
	str	r1, [r0]

	@ --- secondary confirmation: vibrator ON (may not work here) ---
	ldr	r0, =0x82001154
	ldr	r1, =0xa1b2
	str	r1, [r0]			@ vib power gate enable
	ldr	r0, =0x82001244
	ldr	r1, =0x8300
	str	r1, [r0]			@ vib intensity (level 4)
	ldr	r0, =0x82001240
	mov	r1, #1
	str	r1, [r0]			@ vib ON

	@ --- replicate the stock entry's ADI mailbox read of reg 0x140 ---
	ldr	r2, =0x82000018
	mov	r3, #0x140
	str	r3, [r2]			@ ADI RD_CMD = 0x140
	ldr	r2, =0x8200001c
1:	ldr	r3, [r2]
	tst	r3, #0x80000000
	bne	1b				@ spin while ADI busy

	@ --- vibrator OFF: ADI read completed ---
	ldr	r0, =0x82001240
	mov	r1, #0
	str	r1, [r0]			@ vib OFF
	ldr	r0, =0x82001154
	mov	r1, #0
	str	r1, [r0]			@ vib power gate off

	@ --- dispatch-path signal (keylight RMW): match -> ON, mismatch -> OFF ---
	mov	fp, r3
	and	fp, fp, #31			@ fp = ADI result & 31
	ldr	ip, =0x000c29ac
	ldr	ip, [ip]
	and	ip, ip, #0xff			@ ip = *(0xC29AC) & 0xff = 0x1f
	cmp	ip, fp
	ldr	r0, =0x82001224
	bne	2f
	ldr	r1, [r0]
	bic	r1, r1, #0xf0
	orr	r1, r1, #0xe0			@ match: level 14
	str	r1, [r0]
	ldr	r1, [r0]
	bic	r1, r1, #0x30
	orr	r1, r1, #0x20			@ match: ON bit
	str	r1, [r0]
	b	3f
2:	ldr	r1, [r0]
	bic	r1, r1, #0x30
	str	r1, [r0]
	ldr	r1, [r0]
	orr	r1, r1, #0x10			@ mismatch: OFF bit
	str	r1, [r0]
3:
	@ --- hand off to the REAL entry; the natural dispatch runs ---
	ldr	r0, =0x000bf150
	bx	r0

	.p2align 2
	.end
