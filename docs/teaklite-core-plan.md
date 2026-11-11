# TeakLite-I DSP core for QEMU — implementation plan

Goal: emulate the SC6530C's TeakLite-I DSP well enough to **run the real downloaded
DSP program** inside the B310E QEMU machine (or a standalone sim), so the DSP↔ARM
share-memory handshake is reproduced **authentically** instead of hand-faked. This
is the "most faithful" route the owner asked for; this doc scopes it honestly after
the groundwork done 2026-11-11.

## Why (and why it is not a quick win)
- The fake-DSP in `sc6530_dsp.c` answers the boot handshake but cannot reproduce the
  record/parameter handshake — the stock L1 waits on the real program's own
  responses/messages (acking the ring + raising the IRQ did not advance it). A real
  core removes that wall for the whole bring-up.
- **Caveats that bound the payoff** (both measured/established this session):
  1. **Association still needs the modem running + synthetic RF.** Even a perfect
     core does not reach cell association: (a) the stock **modem subsystem never
     runs** in QEMU today — with a SIM inserted it resets the card 8× but never
     reads it (SIM driver needs **IRQ 10**, unraised), so MM/registration never
     starts and the DSP sees only audio traffic (see
     [stock-first-association.md](stock-first-association.md)); (b) once running, the
     DSP L1 waits on **RF receive samples/measurements QEMU does not model**. So the
     core buys faithful *bring-up*, not association on its own.
  2. **The existing decoder is partial/buggy.** `tools/dsp/tl_disasm.py` mis-decodes
     core opcodes — the reset `br` (`0x4180`) and `call` (`0x41c0`) come out as
     `???` (its table row `(0x4180, 0xF000, "br")` is inconsistent: `0x4180 & 0xF000
     = 0x4000 != 0x4180`). The decoder must be corrected against the GBATEK
     TeakLite-II encoding before it can drive an interpreter.

## The program image (ready)
- Reset-aligned program extracted to `dsp_prog.bin`: the segment at
  `dsp-blob-CC874.dec.bin + 0x28`, **66861 halfwords**, program word 0 = `0x4180
  0x001E` (`br 0x1e`, the reset vector). Parameter records (after the `5a5a a5a5`
  marker) are loaded by the ARM into DSP **data** memory at the addresses they name
  — the core must preload them to match the real DSP after `l1_start`.
- DSP memory map (from the interface RE): data word `0xa800` = the ARM share window
  `0x10000000`; the L1 rings, cipher regs (`0xec4c`/`0xec70`/`0xec71`), AFC/mode
  regs (`0xfbf2`/`0xfbf4`/`0xff72`) and the control block live in data space.

## Architecture (Python first, then C in QEMU)
Build and validate a **Python interpreter** (fast to iterate, reuses the corrected
decoder), then port the validated semantics to a **C co-sim device** in the QEMU
machine wired to the shared RAM + APB mailbox/IRQs.

Core components:
- **Memory:** program ROM (66861 w), data RAM (incl. the share window and the
  memory-mapped I/O regs ≥0xff00), with the parameter records preloaded.
- **Register file:** `r0..r7`, accumulators `a0/a1/b0/b1` (40-bit with saturation),
  `sp`, `pc`, status/flags, the `rep`/`bkrep` loop state, page regs, the index/step
  registers for the addressing modes.
- **Decode:** fix `tl_disasm`'s table (br/call masks first), then expose a clean
  `decode(word, next) -> (mnemonic, operands, length)` the interpreter steps.
- **Execute (the hard half):** ALU (add/sub/logic/shift with 40-bit sat + flags),
  the mov/load/store variants, control flow (br/call/ret/reti, conditions), the
  addressing modes (direct, `[rN]` with modular + bit-reversed stepping), `rep`/
  `bkrep` loops, interrupts. The memory-mapped regs drive the real L1 behavior.
- **Harness:** run from reset; on share-memory writes/reads log the interaction;
  compare the DSP→ARM responses it produces against the interface map.

## Milestones
1. **Decoder fix + reset/init trace** — correct br/call/ret etc.; single-step the
   Python core from reset through the early init; confirm it matches the `.dis`.
2. **Full execute semantics** — run the record/parameter handshake against a mock
   share window and reproduce the exact DSP responses the stock L1 expects (the
   thing the hand-faked responder could not do). This is the main effort.
3. **QEMU co-sim device** — port to C, run alongside the ARM in the machine so the
   stock L1 bring-up completes against the real program.
4. **(For association) synthetic RF + modem bring-up** — separate large pieces: SIM
   IRQ 10 / detection so MM runs, and injected receive data so the DSP L1 finds a
   cell. Only then does a live association sniff become possible.

## Status (2026-11-11)
Groundwork done: program image extracted and verified (reset vector confirmed),
decoder-bug identified, memory map known, QEMU DSP command sniffer in place
(`SC6530_DSP_SNIFF=1`). Not started: the interpreter itself (milestone 1+). This is
a multi-session build; milestones 1–2 (a Python core that runs the bring-up
handshake) are the first self-contained chunk and the highest-value next step.
