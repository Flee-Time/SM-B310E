@ -*- tab-width: 8 -*-
@ B310E-OS stock-spy — ADI-write hook (arch = armv5te, THUMB-1 state)
@
@ Splice target: the stock OS's ADI WRITE helper store at flash 0x3038A
@ (`str r7, [r6]` — r6 = ANA address, r7 = value). The 6-byte patch:
@
@   0x3038A:  ldr pc, [pc, #0]     @ absolute jump (works from the XIP and
@   0x3038C:  .long 0x042F0000     @ the PSRAM-copied helper alike)
@
@ The hook (this file) lives at VA 0x042F0000 = PA 0x352F0800 (the stock
@ OS-window PSRAM, copied there by shim-spy.s from image offset 0xE000).
@ On EVERY stock ADI write the hook:
@   1. logs (addr, value) to ring1 (512 x 8 B — all ANA writes);
@   2. if addr is in the audio ANA block [0x82001000, 0x82001600), snapshots
@      four GLB registers (ownership 0x8b0001c4, PA power 0x8b000060,
@      0x8b0000a0, DSP IRQ 0x8b000160) to ring2 (128 x 16 B). All four are
@      always-clockable GLB reads — NO clock-gated/VBC reads (AHB-stall
@      safety); the VBC/DP state is a v2 target;
@   3. after T=2s, watches the keypad matrix (0x8700002c): if the status
@      differs from the idle value for 3 s, fires the watchdog reboot
@      (the user holds CENTER — the SAME hold re-enters download mode, so
@      `spd_dump read_mem 0x352F0800` can recover the rings from PSRAM);
@   4. replicates the original store `str r7, [r6]` and returns by popping
@      the helper's own frame {r3-r7, lr} — the helper's caller never
@      knows the difference (the stack balance is preserved).
@
@ The hook runs in the stock OS's SVC context with IRQs live; it touches
@ only r0-r5 (pushed) and never disturbs r6/r7 (addr/value). All waits
@ bounded; no 0x8c writes. Pure Thumb-1 (`.syntax divided`): 8-bit
@ immediates only; every larger constant comes from a `.ltorg` pool.
@
@ Payload memory map (the packer zero-fills + writes the magic):
@   0x042F0000  hook code + literal pools
@   0x042F0400  ring1 (512 x 8 B)
@   0x042F1400  ring2 (128 x 16 B)
@   0x042F1F00  header: u32 tick · ring1_idx · ring2_idx · timer_base ·
@                        keypad_idle · hold_start · magic · rebooted
@   0x042F1F20  end (fits the 8 KB payload 0x042F0000-0x042F2000)

	.arch armv5te
	.syntax divided
	.code 16

	.section .text, "ax", %progbits
	.p2align 2

	.global spy_hook
spy_hook:
	push	{r0, r1, r2, r3, r4, r5}	@ save scratch (r6=addr, r7=value kept)

	@ ---- tick counter (invocations) ----
	ldr	r0, =0x042f1f00
	ldr	r1, [r0]
	add	r1, r1, #1
	str	r1, [r0]

	@ ---- ring1: log (r6=addr, r7=value) ----
	ldr	r0, =0x042f1f04			@ ring1 idx
	ldr	r2, [r0]
	ldr	r3, =512
	cmp	r2, r3
	bcs	1f				@ full — skip
	ldr	r3, =0x042f0400			@ ring1 base
	lsl	r2, r2, #3			@ 8 B/entry
	add	r3, r3, r2
	str	r6, [r3, #0]			@ addr
	str	r7, [r3, #4]			@ value
	ldr	r0, =0x042f1f04
	ldr	r2, [r0]
	add	r2, r2, #1
	str	r2, [r0]
1:

	@ ---- gate: skip the snapshot + keypad reads until tick >= 100 ----
	@ (the first ~100 ADI writes are the boot bring-up; the keypad and the
	@ GLB snapshots must not read before the OS has powered those blocks —
	@ an early read of a gated peripheral stalls the AHB = the stock OS
	@ dies instantly. ring1 logging is PSRAM-only and always safe.)
	ldr	r2, =100
	cmp	r1, r2
	bcc	9f

	@ ---- ring2 snapshot: only for the audio ANA block ----
	mov	r0, r6
	ldr	r2, =0x82001000
	cmp	r0, r2
	bcc	2f
	ldr	r2, =0x82001600
	cmp	r0, r2
	bcs	2f
	ldr	r0, =0x042f1f08			@ ring2 idx
	ldr	r1, [r0]
	cmp	r1, #128
	bcs	2f
	ldr	r3, =0x042f1400			@ ring2 base
	lsl	r1, r1, #4			@ 16 B/entry
	add	r3, r3, r1
	ldr	r4, =0x8b0001c4
	ldr	r5, [r4]			@ ownership (always-safe GLB)
	str	r5, [r3, #0]
	ldr	r4, =0x8b000060
	ldr	r5, [r4]			@ PA/VBC power
	str	r5, [r3, #4]
	ldr	r4, =0x8b0000a0
	ldr	r5, [r4]			@ PA power 2
	str	r5, [r3, #8]
	ldr	r4, =0x8b000160
	ldr	r5, [r4]			@ DSP IRQ mailbox
	str	r5, [r3, #12]
	ldr	r0, =0x042f1f08
	ldr	r1, [r0]
	add	r1, r1, #1
	str	r1, [r0]
2:

	@ ---- keypad hold-to-reboot ----
	ldr	r0, =0x8100300c			@ sys-timer (~970/s)
	ldr	r0, [r0]
	ldr	r1, =0x042f1f0c			@ timer_base
	ldr	r2, [r1]
	cmp	r2, #0
	bne	3f
	str	r0, [r1]			@ set at the first invocation
	mov	r2, r0
3:
	sub	r0, r0, r2			@ ticks since base
	ldr	r2, =1940				@ ~2 s at 970/s
	cmp	r0, r2
	bcc	4f				@ before 2 s — no hold check yet
	ldr	r2, =0x042f1f10			@ keypad_idle
	ldr	r3, [r2]
	cmp	r3, #0
	bne	5f
	ldr	r3, =0x8700002c
	ldr	r3, [r3]			@ idle = current status (no key at 2 s)
	str	r3, [r2]
5:
	ldr	r3, =0x8700002c
	ldr	r3, [r3]			@ current status
	ldr	r4, =0x042f1f10
	ldr	r4, [r4]
	cmp	r3, r4
	beq	6f				@ idle — clear the hold start
	ldr	r2, =0x042f1f14			@ hold_start
	ldr	r3, [r2]
	cmp	r3, #0
	bne	7f
	ldr	r3, =0x8100300c
	ldr	r3, [r3]
	str	r3, [r2]			@ start the hold
7:
	ldr	r3, =0x8100300c
	ldr	r3, [r3]
	ldr	r2, =0x042f1f14
	ldr	r4, [r2]
	sub	r3, r3, r4
	ldr	r4, =2910				@ ~3 s at 970/s
	cmp	r3, r4
	bcs	spy_reboot
	b	8f
6:
	ldr	r2, =0x042f1f14
	mov	r3, #0
	str	r3, [r2]			@ clear the hold start
8:
4:
9:

	@ ---- replicate the original store + return via the helper's frame ----
	pop	{r0, r1, r2, r3, r4, r5}
	str	r7, [r6]			@ the ORIGINAL store (str r7, [r6])
	pop	{r3, r4, r5, r6, r7, pc}	@ helper's {r3-r7, lr} -> its caller

	.p2align 2
	.ltorg					@ pool 1 (the main hook's literals)

	@ ---- watchdog reboot (the menu's proven sys_wdg_reset sequence) ----
	.align 2
spy_reboot:
	@ clear the sticky HWRST1 boot flag (0x8b000228 bits 15:8)
	ldr	r0, =0x8b000228
	ldr	r1, [r0]
	ldr	r2, =0xffff00ff
	and	r1, r1, r2
	str	r1, [r0]
	ldr	r0, =0x820010e0
	mov	r1, #4
	bl	adi_store
	ldr	r0, =0x820010e4
	mov	r1, #2
	bl	adi_store
	ldr	r0, =0x820014a0			@ WDG LOCK unlock
	ldr	r1, =0xe551
	bl	adi_store
	ldr	r0, =0x82001488			@ WDG CTRL |= 9
	ldr	r1, [r0]
	mov	r2, #9
	orr	r1, r1, r2
	bl	adi_store
	ldr	r0, =0x82001480			@ WDG LOAD_LOW = 0x4000
	ldr	r1, =0x4000
	bl	adi_store
	ldr	r0, =0x82001484			@ WDG LOAD_HIGH = 0
	mov	r1, #0
	bl	adi_store
	ldr	r0, =0x82001488			@ WDG CTRL |= 2 (start)
	ldr	r1, [r0]
	mov	r2, #2
	orr	r1, r1, r2
	bl	adi_store
	ldr	r0, =0x820014a0			@ WDG LOCK relock
	ldr	r1, =0xffff1aae			@ ~0xe551
	bl	adi_store
1:	b	1b				@ spin until the reset

	@ ADI direct store with the FIFO-full poll (fpdoom form). r0=addr, r1=val.
	@ Called from the reboot path (the stack is already restored).
	.align 2
adi_store:
	push	{r0, r1, r2, r3, r4, lr}
1:
	ldr	r2, =0x82000020
	ldr	r3, [r2]
	ldr	r4, =0x200
	tst	r3, r4				@ FIFO_FULL
	bne	1b
	str	r1, [r0]
2:
	ldr	r3, [r2]
	ldr	r4, =0x100
	tst	r3, r4				@ wait FIFO_EMPTY
	beq	2b
	pop	{r0, r1, r2, r3, r4, pc}

	.p2align 2
	.ltorg					@ pool 2 (reboot + adi_store literals)
	.end
