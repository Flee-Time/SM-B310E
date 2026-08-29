@ -*- tab-width: 8 -*-
@ B310E-OS stock-RAM-boot shim (arch = armv5te, ARM state)
@
@ Loaded by spd_dump at 0x34000000 (the FDL "ram" target) together with the
@ first 1MB of dump_firmware.bin (padded after this file to 0x800). This
@ file is linked at 0x34000000 and runs with the MMU OFF (FDL1 state).
@
@ Goal: boot the STOCK main OS (flash offset 0x10000, entry 0xbf150) from
@ RAM instead of NOR, using the MMU to alias the flash window:
@
@   VA 0x00000000-0x00100000  ->  PA 0x34000800   (1MB stock image copy)
@   VA 0x04000000-0x04300000  ->  PA 0x35000800   (stock OS PSRAM window)
@   everything else           ->  identity
@
@ Why: the stock OS is XIP-linked at flash VA 0x0 (entry 0xbf150, boot vector
@ @0x10000) AND uses PSRAM at 0x04000000 (2.9MB high-water per its init_table:
@ 0x0423bba8 + 0xa50b8 zero-fill). PSRAM is 4MB @0x34000000 (MEM_REMAP=1,
@ the window the FDL uses). 1MB image + 3MB OS window == the whole 4MB.
@
@ After MMU-on we `bx` to VA 0x10000 (main OS boot vector, aliased to the RAM
@ copy). The OS's init_table then copies its payloads from the flash window
@ (reads beyond 0x100000 hit real NOR - identical bytes) into the OS PSRAM
@ window (aliased to the free PSRAM) and runs from RAM.
@
@ NOTE: hardware-iteration shim. Verify each step on the real phone; see
@ docs/stockram.md for the reasoning and the configurable knobs.

	.arch armv5te
	.syntax unified
	.code 32

	.section .text, "ax", %progbits
	.p2align 2

	.global _start
_start:
	b	1f
	.long	0x42533130			@ "BS10" magic marker (B310E stock-ram)
	.long	0x00000800			@ IMAGE_OFF (shim size; image follows)
	.long	0x00100000			@ ALIAS_SIZE (VA window aliased to image)
	.long	0x04000000			@ OS_PSRAM_VA  (stock OS PSRAM window)
	.long	0x35000800			@ OS_PSRAM_PA  (free PSRAM after image)
	.long	0x00300000			@ OS_PSRAM_SIZE (3MB)
	.long	0x00010000			@ MAIN_OS_VA  (boot vector jump target)
1:
	@ SVC mode, IRAM stack (same as FDL1)
	msr	cpsr_c, #0xd3
	ldr	sp, =0x40009000

	@ Invalidate I/D caches + TLB (cheap insurance; FDL left them off)
	mov	r0, #0
	mcr	p15, 0, r0, c7, c5, 0		@ invalidate I-cache
	mcr	p15, 0, r0, c7, c6, 0		@ invalidate D-cache
	mcr	p15, 0, r0, c8, c7, 0		@ invalidate TLB

	@ ---- build the TTB at IRAM 0x40000000 (16KB, 4096 sections) ----
	@ Descriptor = 0xc02: section, AP=11 (full access - bits 11:10),
	@ C=0 B=0 (uncached/unbuffered). Our validated kernel TTB uses AP=11;
	@ the earlier `| 0x12` had AP=00 = NO ACCESS (permission fault on the
	@ first touch of the aliased image = blank LCD). Uncacheable is safe
	@ for everything (the FDL wrote RAM with cache off; MMIO must not be
	@ cached).
	ldr	r6, =0x40000000			@ TTB (below FDL1 @0x40004000)
	ldr	r1, =0x00000c02
	mov	r2, #4096			@ 4096 x 1MB = 4GB
	mov	r3, #0				@ PA = i<<20
2:
	orr	r4, r3, r1
	str	r4, [r6, r3, lsr #18]		@ ttb[i] = PA|attr  (r3>>18 = i<<2)
	add	r3, r3, #0x100000
	subs	r2, r2, #1
	bne	2b

	@ ---- override: VA 0x0..0x100000 -> PA 0x34000800 (image) ----
	@ Section 1 (VA 0x100000+) stays IDENTITY (real NOR): the stock OS
	@ reads its init payloads (src 0x1095ec+) and the DRPS resources from
	@ the flash window - identical bytes whether RAM or NOR, and only the
	@ XIP-executed region (boot vector 0x10000, entry 0xbf150, init_table
	@ 0x10410 - all < 0x100000) must be in RAM.
	ldr	r4, =0x34000800
	orr	r4, r4, r1			@ r1 = 0xc02 descriptor
	str	r4, [r6]			@ ttb[0]

	@ ---- override: VA 0x04000000..0x04300000 -> PA 0x35000800 (3MB) ----
	ldr	r4, =0x35000800
	add	r5, r6, #(0x40 << 2)		@ ttb index 0x40 = VA 0x04000000
	mov	r7, #3				@ 3 sections
3:
	orr	r8, r4, r1
	str	r8, [r5], #4
	add	r4, r4, #0x100000
	subs	r7, r7, #1
	bne	3b

	@ ---- enable MMU ----
	mcr	p15, 0, r6, c2, c0, 0		@ TTBR
	mov	r0, #3
	mcr	p15, 0, r0, c3, c0, 0		@ domain 0 = client
	mrc	p15, 0, r0, c1, c0, 0
	bic	r0, r0, #(1 << 13)		@ V=0: low vectors at 0x0 (aliased vector table)
	orr	r0, r0, #1			@ MMU on
	mcr	p15, 0, r0, c1, c0, 0
	nop
	nop

	@ ---- DEBUG MARKER: keypad light ON (0x82001224, 4-bit level in
	@ bits 4-7, bit5=ON - dump-mined stock keylight fn). If the keypad
	@ glows we reached MMU-on and the shim is fine; a blank LCD with a
	@ lit keypad means the failure is in the stock OS, not the shim.
	@ Hold ON ~0.3 s so it is clearly visible before the aliastest
	@ (entered below) turns it OFF. 208 MHz: ~2 cycles/iter. ----
	ldr	r4, =0x82001224
	mov	r5, #0xe0			@ level 15 (bits 4-7), ON bit 5
	str	r5, [r4]
	ldr	r6, =0x02000000			@ ~0.32 s hold
1:	subs	r6, r6, #1
	bne	1b

	@ ---- ALIAS SELF-TEST: branch to a snippet at image offset 0x100
	@ (VA 0x100, same aliased section as the boot vector at 0x10000).
	@ The snippet holds the keylight OFF ~0.6s then jumps to the real
	@ boot vector. keylight ON the whole time = the aliased section
	@ does NOT execute (MMU mapping problem). ----
	mov	r0, #0x00000100
	bx	r0

	.p2align 2
	.end
