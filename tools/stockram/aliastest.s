@ -*- tab-width: 8 -*-
@ Alias self-test snippet for the stock-RAM-boot diagnostic.
@
@ Spliced at image offset 0x100 (VA 0x100, aliased section 0 - the same
@ section as the main OS boot vector at 0x10000). Entered by the shim's
@ `bx 0x100`. Holds the keylight OFF for ~0.8s (clearly visible), turns it
@ back ON, then jumps to the real boot vector at 0x10000.
@
@ Keylight writes use the PROVEN stock RMW pattern (led_keylight_set,
@ HW-verified): OFF = clear bits 4-5 then set bit 4; ON = set level in
@ bits 4-7 then set bit 5. The previous diag stub's direct writes may not
@ have taken effect - RMW is ground truth.

	.arch armv5te
	.syntax unified
	.code 32

	.section .text, "ax", %progbits
	.p2align 2
	.global _start
_start:
	@ --- keylight OFF (RMW) ---
	ldr	r0, =0x82001224
	ldr	r1, [r0]
	bic	r1, r1, #0x30
	str	r1, [r0]
	ldr	r1, [r0]
	orr	r1, r1, #0x10
	str	r1, [r0]

	@ --- hold OFF ~0.64 s (208 MHz: ~2 cycles/iter) ---
	ldr	r2, =0x04000000
1:	subs	r2, r2, #1
	bne	1b

	@ --- keylight ON (RMW, level 14) ---
	ldr	r0, =0x82001224
	ldr	r1, [r0]
	bic	r1, r1, #0xf0
	orr	r1, r1, #0xe0
	str	r1, [r0]
	ldr	r1, [r0]
	bic	r1, r1, #0x30
	orr	r1, r1, #0x20
	str	r1, [r0]

	@ --- jump to the real boot vector ---
	mov	r0, #0x10000
	bx	r0

	.p2align 2
	.end
