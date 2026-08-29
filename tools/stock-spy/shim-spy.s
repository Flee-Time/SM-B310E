@ -*- tab-width: 8 -*-
@ B310E-OS stock-spy shim (arch = armv5te, ARM state)
@
@ Identical to tools/stockram/shim.s PLUS one step: after building the TTB
@ (MMU still off), copy the 8 KB spy payload from the image (flash offset
@ 0xE000, the blank PBL region) to the OS-window PSRAM at PA 0x352F0800
@ (VA 0x042F0000 — where the ADI-helper patch at flash 0x3038A jumps to).
@
@ Layout (loaded by spd_dump at 0x34000000, MMU off):
@   [0x000000]  this shim (padded to 0x800)
@   [0x000800]  dump_firmware.bin[0..0x100000]  (1 MB stock image)
@               - flash 0xE000:  the 8 KB spy payload (blank PBL region)
@               - flash 0x3038A: the ADI-helper patch (6 B)
@
@ Memory map (MEM_REMAP=1, the FDL state):
@   VA 0x00000000-0x00100000  ->  PA 0x34000800   (1 MB image copy)
@   VA 0x04000000-0x04300000  ->  PA 0x35000800   (3 MB OS window)
@   everything else           ->  identity
@ The payload copy target PA 0x352F0800 = VA 0x042F0000 (OS window + 0x2F0000).

	.arch armv5te
	.syntax unified
	.code 32

	.section .text, "ax", %progbits
	.p2align 2

	.global _start
_start:
	b	1f
	.long	0x42533130			@ "BS10" magic marker (stock-ram)
	.long	0x00000800			@ IMAGE_OFF (shim size; image follows)
	.long	0x00100000			@ ALIAS_SIZE (VA window aliased to image)
	.long	0x04000000			@ OS_PSRAM_VA  (stock OS PSRAM window)
	.long	0x35000800			@ OS_PSRAM_PA  (free PSRAM after image)
	.long	0x00300000			@ OS_PSRAM_SIZE (3 MB)
	.long	0x00010000			@ MAIN_OS_VA  (boot vector jump target)
	@ --- stock-spy additions (payload copy) ---
	.long	0x0000e800			@ SPY_SRC_PA  (0x34000000+0x800+0xE000)
	.long	0x352f0800			@ SPY_DST_PA  (VA 0x042F0000 target)
	.long	0x00002000			@ SPY_SIZE    (8 KB)
1:
	@ SVC mode, IRAM stack (same as FDL1)
	msr	cpsr_c, #0xd3
	ldr	sp, =0x40009000

	@ Invalidate I/D caches + TLB
	mov	r0, #0
	mcr	p15, 0, r0, c7, c5, 0		@ invalidate I-cache
	mcr	p15, 0, r0, c7, c6, 0		@ invalidate D-cache
	mcr	p15, 0, r0, c8, c7, 0		@ invalidate TLB

	@ ---- build the TTB at IRAM 0x40000000 (16 KB, 4096 sections) ----
	ldr	r6, =0x40000000			@ TTB (below FDL1 @0x40004000)
	ldr	r1, =0x00000c02
	mov	r2, #4096
	mov	r3, #0
2:
	orr	r4, r3, r1
	str	r4, [r6, r3, lsr #18]		@ ttb[i] = PA|attr
	add	r3, r3, #0x100000
	subs	r2, r2, #1
	bne	2b

	@ ---- override: VA 0x0..0x100000 -> PA 0x34000800 (image) ----
	ldr	r4, =0x34000800
	orr	r4, r4, r1
	str	r4, [r6]			@ ttb[0]

	@ ---- override: VA 0x04000000..0x04300000 -> PA 0x35000800 (3 MB) ----
	ldr	r4, =0x35000800
	add	r5, r6, #(0x40 << 2)		@ ttb index 0x40 = VA 0x04000000
	mov	r7, #3
3:
	orr	r8, r4, r1
	str	r8, [r5], #4
	add	r4, r4, #0x100000
	subs	r7, r7, #1
	bne	3b

	@ ---- STOCK-SPY: copy the payload image[0xE000:0x10000] -> PA 0x352F0800 ----
	@ (MMU still off — physical PSRAM stores; the source is in the image we
	@ loaded at 0x34000000. Word copy, 2 KB words. Uses r9-r12 so r6 (the
	@ TTB) survives for the TTBR write below.)
	ldr	r9, =0x3400e800			@ source PA (image offset 0xE000)
	ldr	r10, =0x352f0800		@ dest PA (OS-window payload)
	mov	r11, #(0x2000 >> 2)		@ 2048 words
4:
	ldr	r12, [r9], #4
	str	r12, [r10], #4
	subs	r11, r11, #1
	bne	4b

	@ ---- enable MMU ----
	mcr	p15, 0, r6, c2, c0, 0		@ TTBR
	mov	r0, #3
	mcr	p15, 0, r0, c3, c0, 0		@ domain 0 = client
	mrc	p15, 0, r0, c1, c0, 0
	bic	r0, r0, #(1 << 13)		@ V=0: low vectors
	orr	r0, r0, #1			@ MMU on
	mcr	p15, 0, r0, c1, c0, 0
	nop
	nop

	@ ---- DEBUG MARKER: keypad light ON ~0.3 s (the stock-ram marker) ----
	ldr	r4, =0x82001224
	mov	r5, #0xe0
	str	r5, [r4]
	ldr	r6, =0x02000000			@ ~0.32 s hold
5:	subs	r6, r6, #1
	bne	5b

	@ ---- jump to the real boot vector (VA 0x10000, aliased) ----
	mov	r0, #0x00010000
	bx	r0

	.p2align 2
	.end
