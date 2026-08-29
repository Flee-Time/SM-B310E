@ -*- tab-width: 8 -*-
@ Exception vector table for the stock-RAM-boot diagnostic.
@
@ Spliced at image offset 0x0 (VA 0x0 - low vectors, the shim forces V=0).
@ The PBL boot vector normally lives here but never runs in our RAM-boot,
@ so this region is free. Any exception (e.g. a prefetch/data abort from
@ the aliased execution) lands here: turn the keylight OFF with the proven
@ RMW pattern and hang. A keylight that goes OFF and stays OFF (with no
@ ~1s hold-back-on from the alias snippet) means an exception fired.

	.arch armv5te
	.syntax unified
	.code 32

	.section .text, "ax", %progbits
	.p2align 2
	.global _start
_start:
	b	fault				@ 0x00 reset
	b	fault				@ 0x04 undef
	b	fault				@ 0x08 svc
	b	fault				@ 0x0c prefetch abort
	b	fault				@ 0x10 data abort
	b	fault				@ 0x14 reserved
	b	fault				@ 0x18 irq
	b	fault				@ 0x1c fiq
fault:
	@ keylight OFF (RMW - proven pattern)
	ldr	r0, =0x82001224
	ldr	r1, [r0]
	bic	r1, r1, #0x30
	str	r1, [r0]
	ldr	r1, [r0]
	orr	r1, r1, #0x10
	str	r1, [r0]
1:	b	1b

	.p2align 2
	.end
