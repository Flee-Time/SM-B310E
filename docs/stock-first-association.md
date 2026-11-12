# Stock first cell-tower association — what the ARM sends to the DSP

The DSP-facing sequence the stock SC6530C firmware runs from power-on to its first
LOCATION UPDATING, i.e. what the ARM posts into the DSP share window (0x10000000 /
DSP 0xa800). Commands/FI types and offsets are defined in
[dsp-arm-interface.md](dsp-arm-interface.md); this doc puts them in time order for
the cold-start → association flow.

Two evidence levels:
- **[QEMU]** — observed live by booting the real stock NOR in the B310E QEMU
  machine with the DSP command sniffer (`SC6530_DSP_SNIFF=1`, `sc6530_dsp.c`).
- **[RE]** — static reverse engineering (headless Ghidra on `dump_firmware.bin` +
  the interface map). Reached only in theory in QEMU — see "Why QEMU stops at
  bring-up" below.

---

## RE-traced control flow (static, 2026-11-11)
The phases below are not just spec-inferred — the stock L1 control flow was traced
in Ghidra. The L1 is an event/state machine: **`SetState` = `FUN_0002f348`**, whose
context struct is PSRAM `0x0422e0a8` (`+8` = current state) and which dispatches
through a per-state handler table (`+0x40`). The state handlers (the `SetState`
callers, cluster `FUN_000573xx–0x588xx`) each post DSP calls and transition:

| Handler (fn) | Posts to DSP | Transitions |
|---|---|---|
| `FUN_0005a874` | **cmd 1** (`FUN_0004b3ca`, start frame clock) | `SetState(1)` |
| `FUN_00057ab4` | — | `SetState(1)` |
| `FUN_0005755e` / `FUN_00057352` | (`FUN_0004bd0c`, RF/chan) | `SetState(3)` |
| `FUN_00057ec2` | (`FUN_0004bc86`, RF/chan) | **`SetState(4)` = SDCCH dedicated** |
| `FUN_00058812` | (`FUN_0004b046`) | **`SetState(6)` = TCH dedicated** |
| `FUN_000580d2` / `FUN_00058196` | — | `SetState(7)` |
| `FUN_00058bea` | **cmd 0x0d** (`FUN_0004b6aa`) | `SetState(8)` |
| **`FUN_000589d8`** (dedicated-establish) | **cmd 3** (`FUN_0004b410`) + RF writes (`FUN_00062dc2/62d78/62dce`) | `SetState(4)` SDCCH or `SetState(6)` TCH |

**The association trigger path, fully RE-confirmed:** the PSRAM L1 router
`FUN_003b13c6` calls `FUN_000589d8` (the only caller), which posts **command 3**
with the channel-mode field from the serving-cell struct, runs the RF/analog
register writes, and `SetState(4)` — i.e. it moves L1 into the **SDCCH dedicated
state** that carries the LOCATION UPDATING. Frame-clock start (`FUN_0005a874` →
cmd 1 → state 1) and the DSP wake sequence (`FUN_0005b004` → `ActiveDspByCmds`
`FUN_0002f11a` → cmds 2, 0x1b, 0x2f) are likewise traced. State labels 3/7/8 are
the receive/idle phases (their exact names need the runtime PSRAM table); states
**1** (frame clock), **4** (SDCCH), **6** (TCH) are code-confirmed. So the ordered
sequence below is the real control flow, not a spec guess.

## Phase A — DSP bring-up  [QEMU, confirmed]
1. **Download.** Reset released; the boot ROM answers `DSP_READY`; the ARM
   downloads the DSP program from NOR 0xcc874 in **66 blocks of 1024 halfwords**
   (`SM_BLOCK`/`SM_DATA`/`SM_COPY` handshake observed), `BOOT_DONE`, strap
   `EXT_PROG_MODE`, reset pulse → **the program runs** (`SM_RUN` observed once).
2. **First L1 command** (observed): `id=0 {0000 0000 0010 0000 0000}` posted via
   the doorbell at **pc 0x00067ff8** (= `FUN_00067fee`, MCU_IRQ_SET). That is
   command 0 (send a parameter record) with word 2 = 0x10 = the record length —
   the first of the stock's record list (`SharedMemProc_StartTranferDataToDsp`).
3. The stock then streams, in order **[RE]**: the ~50 parameter records (DSP data
   tables), command `{0, 0x10}` (the start marker that unblocks the DSP wait at
   program 0x1907), the 14 NV parameter blocks (RF cal + the APC TX-power tables,
   types 1, 3..15), then `{0, 0x10}` again.
4. **DSP wake** `ActiveDspByCmds` (`FUN_0002f11a`): commands **2** (activate),
   **0x1b** (sleep/clock config), **0x2f** (wake).
5. **Frame clock** command **1** → the DSP starts consuming the FI ring at
   ~4.615 ms/frame.

## Phase B — cell search (power sweep)  [RE]
`gsmframegen` (`FUN_00054a3c` / `FUN_00054524`) posts **type-11** power-measure FIs
— 6 ARFCN indexes per frame — across the band / stored BA list. Results come back
in **POWERMONQ** (+0x5c0) as rxlev per ARFCN; `rxlevmon` ranks the carriers.

## Phase C — sync (lock the strongest)  [RE]
For each candidate, `FUN_0005096e` posts a **type-9** FCCH search window and reads
the DSP's **type-10** SCH → BSIC + absolute GSM frame number. `SetState(search)`
shifts the frame timer while the search runs and takes it back at the end.

## Phase D — BCCH read + cell selection  [RE]
**Type-2** rx bursts (BCCH tag) at the BCCH block frames → System Information 1–4
→ PLMN / LAI / cell-selection params / RACH control. The C1 criterion picks a
suitable cell; the L1 camps (`FUN_0005c8ba`).

## Phase E — CCCH monitoring  [RE]
**Type-2** rx bursts at the CCCH frames → paging / access-grant (AGCH).

## Phase F — first association (LOCATION UPDATING / IMSI attach)  [RE]
The actual association, once camped on a suitable cell with a SIM:
1. **Dedicated establish** `FUN_000589d8`: command **3** (serving cell) + the RF /
   state writes (`FUN_00062dc2/62d78/62dce`), `SetState(4)`, and the FI-ring rework
   (`FUN_0005f85c(4)`). (Firmware-confirmed in dsp-arm-interface.md §6.7.)
2. **RACH** `FUN_0004fe0a`: a **type-6** access burst + TXBUFQ `{0, bsic, 0, ra}` +
   command **4** (`{4,0,1}`, PA key), retransmitted per the cell's RACH control.
3. On IMMEDIATE ASSIGNMENT (decoded from CCCH) → move to the assigned SDCCH.
4. **SDCCH**: **type-2** rx + **type-7** uplink bursts; command **3** re-track;
   command **4** per uplink burst. The SABM carries the LOCATION UPDATING REQUEST
   (LAI + classmark + IMSI/TMSI).
5. L3: LU REQUEST → AUTHENTICATION → A5/1 CIPHER MODE (this DSP does **A5/1 only**,
   §7.1) → LOCATION UPDATING ACCEPT.

### The command order, condensed (power-on → associated)
```
[boot/download] → 0×~50 (records) → 0x10 → 0×14 (NV) → 0x10 → 2,0x1b,0x2f → 1
  → per-frame FIs: 11 (sweep) → 9/10 (sync) → 2 (BCCH/CCCH)
  → 3 (dedicated) → 6 (RACH)+4 (PA) → [2+7 SDCCH, 3 re-track, 4]
```

---

## Why QEMU stops at bring-up (and what a faithful sniff needs)
The sniffer captured the real download + the first command, then the stock stalls:
the fake DSP in `sc6530_dsp.c` does not **execute the downloaded program**, so it
cannot reproduce the record/parameter handshake (the stock L1 waits on the DSP
program's own responses and messages, not just a ring-index bump — acking the
command ring + raising the IRQ was not enough to advance it). Two faithful routes:

- **Fake-cell responder** (medium effort): make the DSP model answer the share
  protocol — ack records the way the program would, return POWERMONQ rxlev for a
  synthetic cell, return a crafted SCH (BSIC+FN) and BCCH System Information, then
  immediate-assignment on the CCCH. This never runs DSP code; it injects the
  *results* the ARM expects, which is enough to walk the L1 through association and
  capture the real ARM→DSP command/FI stream. It is brittle (every handshake must
  match the program's semantics).
- **TeakLite core** (large effort, "most faithful"): emulate the TeakLite-I DSP as
  a co-sim device inside the QEMU machine and let it **run the real downloaded
  program**, wired to the share RAM + the APB mailbox/IRQs. This reproduces the
  bring-up handshake authentically. **Caveat:** it still will not reach cell
  *association* on its own — the program then drives the RF front end and waits for
  **receive samples/measurements that QEMU does not model**, so it would find no
  cell. Reaching association also needs a synthetic RF/BTS feeding the DSP's
  receive path — a second large piece of RE/modelling. So the DSP core buys
  faithful bring-up (and the authentic DSP↔ARM handshake), not association by
  itself. `teakra` emulates the newer 3DS *Teak*, a different ISA from this chip's
  TeakLite-I, so it is a reference, not a drop-in; the repo's `tools/dsp/tl_disasm.py`
  shows the ISA is understood well enough to disassemble but a full core is new work.

### Empirical blocker map (2026-11-11, measured in QEMU)
Attempting the live sniff pinned the **root blocker below the DSP entirely**: the
stock **modem subsystem never runs**, so there is no GSM L1 traffic to sniff —
only the audio DSP path.
- **Baseline (no SIM):** the only ARM→DSP command the stock posts is **one** (the
  audio codec's `id=0 {0,0,0x10}` at pc 0x67ff8); it then spins ~2.35M times on
  `0x8b0001c4` (PERI_CTL0, audio ownership) at pc 0x1ba54. That is the audio
  subsystem, not L1.
- **With a SIM inserted** (`-global sc6530_sim.sim-slots=1`): the stock **resets
  the card 8×** (`sc6530_sim_reset`) but never issues a SELECT/READ — its SIM
  driver is **interrupt-driven and needs IRQ 10, which the SIM model does not
  raise** (it only pushes the ATR into the RX FIFO). So the card is never read,
  the SIM is not detected, and **the DSP command trace is byte-for-byte identical
  to the no-SIM case** (still one doorbell, audio only). MM/registration never
  starts; no cell search; no type-11 power FIs; no sync.

So the chain to a live association sniff is **SIM IRQ 10 + ATR/STS semantics →
SIM detected → MM/registration task runs → cell search → the DSP L1 response
protocol (POWERMONQ/SCH/BCCH/immediate-assignment) → (for a real grant) synthetic
RF**. Each link is its own bring-up; the "modem unmodeled" note in
[b310e-qemu.md](b310e-qemu.md) is exactly this. The fake-cell DSP responder only
matters **after** the modem runs — it is not the first blocker.

**Bottom line:** the command/FI *sequence* above is the answer to "what it sends to
the DSP", and QEMU confirms the bring-up end empirically. A live in-emulator
association sniff is gated on bringing up the stock **modem subsystem** (SIM
detection first), a separate project independent of how the DSP answers.
