@ -*- tab-width: 8 -*-
@ B310E-OS stock-spy — UART-putc hook (arch = armv5te, THUMB-1 state)
@
@ Splice target: the stock OS's UART character-output function at flash
@ 0x38402 (`push {r4,lr} ... str r5,[r4]` at 0x38416 — r4 = UART base from
@ the runtime table 0x0422E4B8, r5 = the char). The 6-byte patch:
@
@   0x38416:  ldr pc, [pc, #0]     @ absolute jump (XIP + PSRAM copies)
@   0x38418:  .long 0x042F1C00     @ this hook
@
@ The store + the function's pop {r4,pc} are replicated by the hook, and the
@ char is appended to the debug-text ring (ring3). The stock OS's entire
@ debug output (trace messages + the "AST_BLUESCREEN" assert text) flows
@ through this function — the captured text tells us EXACTLY where the
@ RAM-booted stock OS hangs.
@
@ The clobbered 2 bytes at 0x3841A (the next function's `push {r4,lr}`) are
@ safe: no bl caller of 0x3841A exists in 0x30000-0x80000 (verified).
@
@ Payload map (fits after ring2, before the header):
@   0x042F1C00  this hook
@   0x042F1D00  ring3: 512 chars of debug text
@   0x042F1F00  header (+ring3_idx at 0x1F20)

	.arch armv5te
	.syntax divided
	.code 16

	.section .text, "ax", %progbits
	.p2align 2

	.global spy_uart_hook
spy_uart_hook:
	push	{r0, r1, r2, r3, r6, r7}	@ save (r4=UART base, r5=char kept)

	@ ---- ring3: log the char (r5) ----
	ldr	r0, =0x042f1f20			@ ring3 idx
	ldr	r1, [r0]
	cmp	r1, #255
	bcs	1f				@ full (255 chars — enough for the text)
	ldr	r2, =0x042f1d00			@ ring3 base
	add	r2, r2, r1
	strb	r5, [r2, #0]			@ the char (byte)
	add	r1, r1, #1
	str	r1, [r0]
1:

	pop	{r0, r1, r2, r3, r6, r7}
	str	r5, [r4]			@ replicate the TX store (str r5,[r4])
	pop	{r4, pc}			@ the putc's frame -> its caller

	.p2align 2
	.ltorg
	.end
