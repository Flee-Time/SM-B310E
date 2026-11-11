# ARM ↔ DSP interface: the GSM L1 RX and TX call map

A consolidated map of the ARM-side calls into the SC6530C's GSM layer-1 DSP, and
the share-memory protocol they speak. It pulls together what is spread across
`os/drivers/dsp.c` (transport, commands, frame instructions, TX, cipher),
`os/drivers/l1cell.c` (the RX/dedicated schedulers), and the reverse-engineering
notes in `docs/gsm-plan.md` (the authoritative FI/command decode, ~line 1014),
`docs/a5-cipher-re.md` (ciphering) and `docs/l1-osmocom.md` (the primitive map).
The TCH / voice-traffic map (§8, not built in our OS) is drawn from the stock SDK
signal headers (`tools/mocor-zw217/BASE/PS/export/inc/*.h`) and the L1 trace
strings (`.../common/export/inc/trace_id/`). The command surface (§9.0) and message
codes (§3.4) are decompiled from the stock ARM firmware with headless Ghidra 12
(the `B310E` project over `dump_firmware.bin`; `GHIDRA_INSTALL_DIR` is set, driven
by a Java `GhidraScript` — PyGhidra is not needed).

Confidence tags follow gsm-plan.md: **[C]** read from the stock binary/headers,
**[L]** likely, **[G]** guess, **[spec]** fixed by the GSM standard. Stock
function names are the Ghidra `FUN_xxxxxxxx` labels of the ARM "user" firmware
(`drps_12E524_0.bin`); bare `0xNNNN` addresses in the decode tables are **DSP**
program/data words (TeakLite, `dsp-blob-CC874.dis`).

---

## 1. Architecture

```
  OsmocomBB-style L2/3 (RR/MM/CC) — os/net/*.c
      │  l1_api (the single stock-faithful surface; os/include/os/dsp.h)
  ARM L1 driver:  dsp.c (boot, rings, commands, FI, TX, cipher)
                  l1cell.c (sync, BCCH/CCCH, dedicated SDCCH scheduler)
      │  share memory @ ARM 0x10000000  ==  DSP word address 0xa800
  stock GSM L1 (DSP blob booted from NOR 0xcc874) + NV calibration
      │  GMSK / TDMA scheduler / channel coding / interleaving / ciphering / AGC-AFC
  RF front end
```

The DSP does all the real PHY. The ARM **posts frame instructions** (one per TDMA
frame: "receive this burst", "measure these ARFCNs", "transmit this burst") and
**commands** (out-of-band control: start, dedicated-mode establish, PA key), and
**drains result rings** (decoded blocks, power measurements, BEP reports,
messages). Every exchange is through the 4 KB share window; no other DSP address
is ever touched (reads above the window permanently stalled the AHB — `dsp.c`
header).

**Address rule [C]:** share byte offset `N` (ARM `0x10000000 + N`) is DSP word
`0xa800 + N/2`. So the FI ring at ARM `+0x000` is DSP `0xa800`; the cipher COUNT
regs at ARM `+0x3f0` are DSP `0xa9f8`→`0xec4c`.

---

## 2. Share-memory map (ARM offsets into the 0x1000-byte window)

Defined at the top of `dsp.c`. Each ring is a producer/consumer pair of indices
with power-of-two or modulo wraparound; the **producer writes its slot then bumps
its index last** so the consumer never reads a half-written slot.

| Offset | Region | Dir | Slots × size | In / Out idx | Driver |
|---|---|---|---|---|---|
| `+0x000` | **FI ring** (frame instructions) | ARM→DSP | 26 × 22 B | `+0x23c` / `+0x23e` | `post_fi` |
| `+0xeea` | FI extension (6 bytes per FI slot) | ARM→DSP | 26 × 6 B | — | `post_fi` |
| `+0x240` | **TXBUFQ** (TX payload descriptors) | ARM→DSP | 4 × 62 B | `+0x338` / `+0x33a` | `tx_desc_post` |
| `+0x33c` | **CMD ring** (commands) | ARM→DSP | 8 × 10 B | `+0x38c` / `+0x38e` | `post_cmd` |
| `+0x390` | **RESP ring** (responses) | DSP→ARM | 8 × 10 B | `+0x3e0` / `+0x3e2` | `l1_service` |
| `+0x40c` | **RX ring** (decoded blocks) | DSP→ARM | 6 × 72 B | `+0x5bc` / `+0x5be` | `l1_rx_read` |
| `+0x5c0` | **POWERMONQ** (rxlev results) | DSP→ARM | 30 × 8 B | `+0x6b0` / `+0x6b2` | `l1_pm_read` |
| `+0x6f6` | parameter-record staging body | ARM→DSP | ≤0x280 HW | — | `send_block` |
| `+0xfa0` | **BEP ring** (bit-error reports) | DSP→ARM | 7 × 4 B | `+0xfbc` / `+0xfbe` | `l1_service` |
| `+0xfe0` | **control block** (ping/dump + msg) | both | 0x1e B | see §3.4 | `l1_service` |
| `+0xfc0` | layer-1 start clear boundary (`SHARE_CLEAR`) | — | — | — | `l1_start` |
| `+0xffe` | start marker `0x8181` | ARM→DSP | — | — | `l1_start` |

Dedicated-mode scalar slots (pulled in by FI word-5 flags, §6.3 / §7):

| Offset | Meaning | Writer |
|---|---|---|
| `+0x3e4` | uplink **timing advance** (0..63), double-buffered | `l1_tx_ta_set` |
| `+0x3ec` | **TX power level** (PCL), double-buffered at `+0x3ec + 2·t` | `l1_tx_level_set` |
| `+0x3f0` | A5 **COUNT** (GSM frame number) → DSP `0xec4c/4d/4e` | `fi_init` |
| `+0x3f8` | A5 **Kc[8]** (one byte per word) → DSP key reg `0xec71..78` | `l1_cipher_set` |

---

## 3. Transport protocol

### 3.1 Word access discipline [C] (`sm_w16` / `sm_r16`, FUN_00061040 / FUN_00061066)
- **Write** until the value reads back; give up after 6 tries.
- **Read** until two successive reads agree; give up after 6 tries.
- Data area takes **16-bit stores only** — byte stores corrupt the DSP image.

### 3.2 Command ring (ARM→DSP) [C] (`post_cmd`, FUN_0005f474)
- 8 slots of 5 halfwords. Write slot `(in+1)&7`, then set `CMD_IN`, then ring the
  **doorbell**: `REG32(0x8b000160) = 1` (MCU_IRQ_SET, FUN_00067fee).
- Word 0 is the command id (see catalog §9).
- `wait_cmd_taken` considers a command accepted when `CMD_OUT` catches up to
  `CMD_IN`, or a reply arrives, or the DSP interrupts.

### 3.3 Frame-instruction ring (ARM→DSP) [C] (`post_fi`, FUN_0005ef76)
- 26 slots of 11 halfwords + a 6-byte extension. Write the extension and the 11
  words into slot `(in+1)%26`, then bump `FI_IN`.
- Each post advances the ARM's local `frame_number` by one (mod `0x297000`, the
  23-bit hyperframe). `l1_fi_free()` = free slots; `note_fi_out()` tracks how far
  the DSP's consumer (`FI_OUT`) has advanced = where its frame clock is.
- **"Ask more frames"** (`l1_ask_more`, FUN_0005f208): OR `0x8200` into word 0 of a
  slot N back from `in`, so the DSP raises an interrupt when it reaches it. Used to
  keep the ring topped up without a tight poll.

### 3.4 DSP→ARM: the service routine [C] (`l1_service`, FUN_0004ee54 + FUN_0003ab0a)
Runs both from the DSP ISR (`dsp_isr`, IRQ line cleared via `0x8b000160`) and
from thread-side `l1_poll()`. It drains, in order:
1. **RESP ring** `+0x390`: 5-halfword responses. Word 0 low byte selects a handler
   [C]; `0x000f` = a parameter-record ack (kept in `last_ack`), anything else is a
   generic reply.
2. **BEP ring** `+0xfa0`: one bit-error report per decoded burst; word-0 bits 13:0
   are the ARFCN snapshot `[0x62]`, i.e. which channel the burst came in on.
3. **Control block** `+0xfe0`: if the ack word `+0xff4` is 0 there is a DSP→ARM
   **message** at `+0xff0`/`+0xff2` (code+param; `0xffff....` = a DSP assert id).
   Read it, then re-arm the ack to 1 (the DSP's message routine at DSP `0x17df`
   spins until the ack is 1, so this must be kept set).

The DSP side posts a message through a generic routine at DSP `0x40E7`: it writes
the code to `0xaff0` (= ARM `+0xff0`) and the parameter to `0xaff2`, then signals.
The ARM handler is **`FUN_0003ab0a`** (Ghidra-decompiled): it reads the code at
control-block `+0x10`, the parameter at `+0x12`, then sets the ack `+0x14 = 1`.
Code dispatch [C]:
- **`0xffff` = DSP Assert** — logs `"DSP Assert ID %d"` with the `+0x12` parameter
  (confirmed: the handler tests `code + 0xffff0001 == 0`, i.e. `code == 0xffff`).
- **`5`** and **`0x6a`** are the two recognized data messages; on this build their
  deep consumers (`FUN_00034fd4`/`fd6`) are **empty stubs** (like the share-clock
  semaphore noted in gsm-plan.md), so they are acked but do nothing further here.

So for our driver the only message that matters is the assert (`0xffff`); the
richer L1-event mapping that the stock would run from codes 5/0x6a is stubbed out
in this firmware build.

### 3.5 Boot & lifecycle [C] (`dsp_boot` → `download` → `l1_start` → `l1_run`)
- `dsp_boot`: LZMA-unpack the DSP image from NOR `0xcc874`, CRC-check, download in
  1024-halfword blocks over the `+0x00..+0x08` handshake (ARM_READY / DATA_READY /
  START_COPY ↔ DSP_READY / READY_TO_COPY / COPY_DONE + a per-block IRQ), then
  strap EXT_PROG_MODE and pulse reset to start the program.
- `l1_start`: clear to `+0xfc0`, arm the ack, enable the DSP IRQ, write the start
  marker `0x8181` at `+0xffe`, then send the stock sequence
  **records → cmd {0,0x10} → NV parameter blocks → cmd {0,0x10}** (the second
  `0x10` is what unblocks the program's wait at DSP `0x1907`).
- `l1_run` (FUN at DSP `0x5a874`): reset `frame_number`, post 4 idle frames,
  `ask_more(2)`, then command `{1,0,...}` → handler `0x9a20` sets `0xff72` bit 2
  and the **frame clock** begins consuming the FI ring (~4615 µs/frame).

---

## 4. Frame-instruction (FI) format

`fi_init` (FUN_0004f5fc) builds the skeleton: words 0..5 = 0, words 6..10 and the
extension = `0xffff`; word 0 bits 4:0 = type; the frame number goes in **word 1
bits 15:9 (high 7)** and **word 2 (low 16)**. Builders then fill type-specific
fields. The DSP decoder (DSP `0x7c4f`, per-frame while `0xff72` bit 2 is set)
unpacks slot `out+1` into DSP regs `0xec1a..0xec52`:

| Word | Field | Notes |
|---|---|---|
| w0 | bits 4:0 **type** → `0xec50`; bits 15:9 flags → `0xec1a` | flags 0 for us except TX `0xa000` |
| w1 | 1:0 **burst**; 3:2; 7:4 **tag**; 15:9 FN high; **bit 8** | bit 8 = 0 ⇒ DSP reloads A5 COUNT from `+0x3f0` [C] |
| w2 | FN low | |
| w3 | 10:8 **timeslot (TN)**; 15:12 | |
| w4 | 11:8 **TSC** (training sequence) | TSC table DSP `0x4578+tsc` |
| w5 | flags; **14:13 cipher alg**; **bit 15 Kc-load** | pulls in `+0x3e4/+0x3ec/+0x3f8`; see §7 |
| w6 | 9:0 **ARFCN index**; 15:10 **gain/AGC** | gain `= (w6>>10)·2 \| w5.0` |
| w7..w9 | monitor channels (same layout; `0x3ff` = none) | w7 reused as ARFCN index for TX |
| w10 | 15:10 **cell slot**; 9:0 | plus the `0x100`/card bits |

Type handlers live in the DSP record table at `0x10dd + type`:
0 idle `0x867c`, 1 sleep `0x8724`, **2 block `0x8771`**, 3 `0x88b2`,
**9 FCCH `0x4e7d`**, **10 SCH `0x8ee6`**, **11 power `0x8f6a`**.

### 4.1 ARFCN → DSP channel index (`l1_arfcn_index`, DSP FUN 0x5287) [C]
FI word 6 (and TX words 6/7) carry a **flat channel index**, not an ARFCN. The DSP
packs all bands into one 0..970 space (`RF_TOTAL_CHANS` = 971); the index→band
order was read from the stock DSP:

| Index | Band | ARFCN |
|---|---|---|
| 0..124 | P-GSM 900 | 0..124 |
| 125..498 | DCS 1800 | 512..885 |
| 499..547 | E-GSM 900 | 975..1023 |
| 548..671 | GSM 850 | 128..251 |
| 672..970 | PCS 1900 | 512..810 |

Note E-GSM is a **separate index range** (499..547), so e.g. ARFCN 998 is index
522 — not band-0 "arfcn 998". Every `l1_*_post` that takes `index` expects this
mapped value.

### FI type catalog

| Type | Name | Builder (ARM) | Direction | Purpose |
|---|---|---|---|---|
| 0 | idle | `l1_idle_post` | — | keep the frame clock fed |
| 1 | sleep | `l1_sleep_post` | — | skip N frames (word 3 = count) |
| 2 | block | `l1_burst_post` | RX | one burst of a 4-burst DL block |
| 6 | access | `l1_access_post` | TX | RACH (word0 `6\|0xa000`) |
| 7 | uplink | `l1_uplink_post` / `l1_uplink_fi` | TX | normal uplink burst (word0 `7\|0xa000`) |
| 9 | FCCH | `l1_fcch_post` | RX | frequency-correction search window |
| 10 | SCH | `l1_sch_post` | RX | sync-burst read |
| 11 | power | `l1_pm_post` | RX | measure up to 6 ARFCNs |

---

## 5. RX path

### 5.1 Call graph (`l1cell.c` over `dsp.c`)
```
l1_cell_select (choose a cell)
 ├─ l1_rxlev_measure ──► l1_pm_post (type 11) ──► l1_pm_read (POWERMONQ +0x5c0)
 ├─ l1_cell_sync    ──► l1_fcch_post (type 9) ──► l1_rx_read (SCH, hdr[0]=1)   ──► sch_decode → fn_offset,bsic
 └─ l1_cell_read    ──► l1_burst_post (type 2, tag 7) ×4/block ──► l1_rx_read (hdr[0]=7) ──► si_parse
l1_cch_read         ──► l1_burst_post (type 2, tag 7) at CCCH frames ──► l1_cch_classify (paging / imm-assign)
l1_dm_sched         ──► l1_burst_post (type 2, tag 9) on the SDCCH ──► l1_rx_read (hdr[0]=9) + interleaved FCCH re-lock
                    └─► l1_uplink_post (SACCH window, §6.5) when a block is armed
```
(`l1_arfcn_index` maps ARFCN→the flat index every post takes, §4.1;
`l1_cell_gain_set` sets the AGC the RX bursts run at, §5.4.)

### 5.2 Measurement — `l1_pm_post` / `l1_pm_read` (FUN_0004f646 / FUN_0005ed64) [C]
- One type-11 FI carries up to 6 ARFCN indexes (words 6..9 + ext 0..1), each
  `index | gain<<10`. Results land in POWERMONQ: halfword 0 = index, 1 = level
  (bits 11:4 = rxlev, unit 1/16; round `(v+8)>>4`), 3 = status (1 ok, 2 too
  strong → retry at higher gain, 3 nothing there). `l1_rxlev_measure` walks gain
  (10/25/40) until it gets a clean sample.

### 5.3 Sync — `l1_cell_sync` (FUN_0005096e) [C]
- Posts a **type-9** FCCH window (`WINDOW`=26 frames). The search returns its own
  SCH in the RX ring with `hdr[0]=1`, `hdr[4]` = (hi−lo) frame span to the burst.
- `sch_decode` recovers BSIC and the absolute GSM frame number; `fn_offset` =
  cell-FN − local-FN is stored so every later `(local + fn_offset) % FN_MOD`
  gives the cell's TDMA position. Must also check BSIC (false `+0x08==0` decodes).
- **No type-10 reads** standalone: a failed SCH read recenters the synth when not
  camped and poisons later reads (phone-observed). Reposts the window on a
  26-frame miss rather than one-shot failing.

### 5.4 Broadcast / common control — `l1_cell_read` / `l1_cch_read`
- BCCH block = four **type-2** bursts (tag 7) at cell `FN%51 == 2`; `si_parse`
  decodes SI 1–4/13 (TS 44.018). CCCH = the nine blocks per 51-multiframe at the
  `cch_start` frames; `l1_cch_classify` flags paging (0x21/22/24) and immediate
  assignment (0x3f/3a/3b).
- **Gain source for word 6:** `l1_burst_post` has no cell being tracked, so it
  carries the AGC setting itself — the cell's, set by `l1_cell_gain_set(cell,
  rxlev/2)` (stored `0x80|setting`), else the stock's starting 25. A fixed 25 lost
  ~¾ of CCCH blocks on a strong cell; `l1_cell_stay` re-measures and re-sets it as
  the level drifts (camped AGC). `l1_gain_for_rxlev` maps rxlev→setting (cap 45).

### 5.5 RX result header — `l1_rx_read` (FUN_0005f692) [C]
Returns `hdr[0..8]` + 58 data bytes (`+0x12` on, each halfword high-byte-first):

| hdr | Meaning |
|---|---|
| `[0]` | block type / **RxChannelTag**: 1 SCH, 7 BCCH/CCCH block, 8 (CCCH alt), 9 dedicated |
| `[1]` | **BFI** — 0 = good, non-zero = bad CRC |
| `[2]` | frame number low 16 |
| `[3]` | bits 7:0 FN high, bits 12:8 cell |
| `[4]` | SCH only: hi<<8\|lo frame span from search (`FUN_000461e4`) |
| `[7]` | signed SNR / freq-error proxy [G] (diagnostic `l1_cch_hdr7`) |

---

## 6. TX path

All TX primitives **refuse in `L1_TX_OFF`** (the safe default); the power-clamp
interlock (`l1_tx_power_set`, §7) must be armed first. Every uplink burst is two
share writes plus a doorbell command:

```
  FI ring (type 6 or 7)  +  TXBUFQ descriptor(s)  +  command {4,0,1}  →  DSP keys the PA for that frame
```

### 6.1 TXBUFQ descriptors — `tx_desc_post` (FUN_0005f332) [C]
A slot at `+0x240 + next·62`: word0 = tag, word1 = type, then the payload as
halfwords **high byte first**. Descriptor types:
- **type 0** — 4-byte control word `{0, bsic, 0, ra|burst}`. Byte 1 is the BSIC
  (cell table `+32`), which the burst parity is coded with (TS 45.003 4.6) — it
  was long sent as 0, so any BTS with another BSIC dropped every burst.
- **type 3** — the 23-byte L2 block (length table[3]=0x17), posted once per block
  on burst 0.

Pacing: `txbuf_free()` reports usable slots; the scheduler never posts a type-7
FI without its TXBUFQ entry (a type-7 FI with no entry stalls the DSP frame clock).

### 6.2 RACH — `l1_access_post` (FUN_0004fe0a) [C, SDR-verified]
- **Type-6** FI: word0 = `6|0xa000`, word3 = 0 (no hop, power band 0), word5 =
  pending power flag, word7 = `w7hi<<10 | index`, word10 = `slot<<10 | 0x3ff`.
- TXBUFQ **type-0** descriptor `{0, bsic, 0, ra}` (ra = the 8-bit CHANNEL REQUEST),
  then command `{4,0,1}`. Verified on air to emit spec-compliant access bursts.

### 6.3 Normal uplink burst — `l1_uplink_post` / `l1_uplink_fi` + `l1_uplink_desc` (FUN_000504f8 / FUN_00050426 / FUN_0004f526) [C, HW-gated]
- The burst is a **type-2 RX frame rewritten to type 7**: word0 = `7|0xa000`,
  word1 = tag/burst/FN, word3 = TN (high nibble cleared), word4 = TSC, **words 6
  and 7 = the ARFCN index**, word5 = power flag `|` cipher bits, word10 = cell.
- Per block the stock posts the 23-byte block once (burst 0, type-3 descriptor)
  plus a 4-byte control word per burst, paced one burst per frame so the 4-slot
  ring never holds more than two. The split `l1_uplink_fi` (FI + PA command) /
  `l1_uplink_desc` (TXBUFQ entries) lets the scheduler feed the two rings
  independently (`l1_dm_sched`).

### 6.4 Power & timing advance
- `l1_tx_level_set(pcl)` — write the PCL at `+0x3ec + 2·t` (toggling buffer `t`);
  the next TX frame's word5 = `0x200 | t<<10` (`txpwr_w5`) tells the DSP to read it
  (FUN_00062cce / FUN_0005f810).
- `l1_tx_ta_set(ta)` — write TA (0..63) to both `+0x3e4` slots; signalled by the
  same word-5 flag (FUN_00062d92). Without it a normal burst lands TA bit-periods
  late, outside the BTS window, and is dropped.
- The APC setpoint tables (NV item 2, cal types 4–7) are clamped to the calibrated
  minimum in `send_parameters` unless `L1_TX_NORMAL` is armed — the safety floor.

### 6.5 SACCH on the dedicated channel — `l1_sacch_set` + `l1_dm_sched` [C, HW-gated]
The slow associated control channel rides its own window once per 102-multiframe
(GSM 05.02 Table 4), independent of SDCCH traffic, so the link survives between L3
steps. `l1_sacch_set(block)` arms a 23-byte uplink SACCH block; `l1_dm_sched` posts
it as a **type-7 uplink burst** (tag `TAG_DCH`=9) at the subchannel's SACCH window
(`sacch_burst`: DL at `FN%102` = `32+k·4` for subch 0–3 / `83+(k−4)·4` for 4–7, UL
offset +15). The posting state is file-static so a 4-burst block begun in one
`l1_dm_sched` call finishes in the next. SDCCH traffic wins a shared frame; the
call is held open until all four SACCH bursts are out (a 3-burst block will not
decode). Downlink SACCH blocks arrive in the RX ring like any dedicated block.

### 6.6 Dedicated scheduler ring-depth & AFC re-lock — `l1_dm_sched` [C]
Two protocol details the scheduler enforces per frame:
- **Ring depth is per-frame, not per-call:** downlink reads and idle fills queue
  deep (`AHEAD`) so the DSP has steady look-ahead; only an actual uplink/SACCH
  burst throttles to a shallow ring (2), because its TXBUFQ slot frees only when
  the DSP transmits. Gating the whole call shallow dropped DL decode to ~18%.
- **FCCH re-lock:** at `FN%51 ∈ {0,10,20,30,40}` the scheduler reads an FCCH burst
  on the BCCH carrier (same ARFCN, tn0) to re-lock the DSP AFC — without it the AFC
  free-runs on the dedicated timeslot and the SDCCH CRC drifts to ~0 within a
  second. `window=1` keeps the TDMA frame accounting exact.

### 6.7 Dedicated-mode establish — `l1_dm_est` / `l1_cell_track` (command 3, FUN_0004b410 / FUN_000589d8) [C, partial]
- `l1_cell_track(cell, rxlev)` posts `{3, cell, mode, rxlev, flags}`. A **tracked**
  cell's per-burst frequency error is steered into the AFC (DSP `0x79b3`) and the
  synth — and therefore the transmitter — follows the cell. **This is the piece
  the stock runs continuously that our L1 does not:** command 3 lives inside the
  stock L1 state machine (PSRAM `FUN_003b13c6` → `FUN_000589d8`, state via
  `FUN_0002f348` "SetState") with continuous AFC/AGC. Our `l1_dm_sched` re-asserts
  command 3 and interleaves FCCH re-reads on the BCCH carrier to approximate it —
  see `docs/gsm-plan.md` "SDCCH AFC drift" and the project memory.
- **Firmware-confirmed** (Ghidra decompile of `FUN_000589d8`): it builds command 3
  via `FUN_0004b410` with the channel-mode field from the serving-cell struct, then
  — only when a channel is actually assigned — runs the RF writes `FUN_00062dc2` /
  `62d78` / `62dce`, sets the L1 state via `FUN_0002f348(mode==2 ? 6 : 4)`, and
  reworks the frame-instruction ring via `FUN_0005f85c(4)`. This matches the
  dedicated-mode sequence in `docs/l1-osmocom.md` exactly — the RF/state writes are
  stock-internal PSRAM bookkeeping, not share-memory, which is why our share-driven
  L1 reaches the channel without replicating them (only the scheduler is missing).

---

## 7. A5 ciphering hooks (`l1_cipher_set`, RE in `docs/a5-cipher-re.md`) [C]

Once a CIPHERING MODE COMMAND is honoured, both RX (`l1_burst_post`) and TX
(`l1_uplink_post`) OR a `cipher_w5` value into **FI word 5**:
- **bit 15** → DSP `0xec70`: when set, the DSP copies **Kc[8]** from share `+0x3f8`
  (8 words, one byte each) into key reg `0xec71..0xec78` (DSP copy at dis `0x85A5`).
- **bits 14:13** → DSP `0xfbf2`: algorithm select; the engine (dis `0x8640`)
  activates only when this field `== 1`, so **A5/1 = bits 14:13 = 0b01**. Thus
  cipher-on word5 = `(1<<15)|((alg+1)<<13)` = `0xA000` for A5/1.
- **COUNT** = share `+0x3f0` → DSP `0xec4c/4d/4e` (packed `0xec4c<<16 | 0xec4e<<8 |
  0xec4d`, engine dis `0x1645`). `fi_init` seeds it with the cell's absolute FN
  (`frame_number + fn_offset`) on the first ciphered frame (**word-1 bit 8 = 0**),
  then sets **bit 8 = 1** so the DSP advances its own counter. A wrong COUNT =
  wrong keystream = no ACCEPT. A5/3 (`e000`) cannot activate on this DSP path.

### 7.1 There is no A5/3 (KASUMI) engine — only A5/1 [C, exhaustively checked 2026-11-11]
The DSP blob (and the whole firmware) implements **only A5/1**. Checked three
independent ways:
- **Engine select** (dis `0x8640`): `0xfbf4 = ([0xfbf2] == 1) ? 1 : 0` — a single
  comparison against algorithm-field value **1**, with **no branch for 2 or 3**. A5/2
  and A5/3 both fall through to "cipher off". A5/1 is the only algorithm the engine
  can turn on.
- **No KASUMI S-boxes:** the fixed S7 (128-entry) and S9 (512-entry 9-bit) tables
  that any KASUMI must contain are **absent from every firmware image** (60 `.bin`
  files — DSP blobs, flash dumps, the ARM `dump_firmware.bin`, `user_raw`), in every
  byte/word/endian layout. KASUMI cannot be computed without them (the boolean-ANF
  form would still need the key-schedule constants below).
- **No key-schedule constants:** KASUMI's fixed `C1..C8` = `0123 4567 89AB CDEF FEDC
  BA98 7654 3210` never appear as an array, and 4 of the 8 words (`4567 89AB BA98
  3210`) do not occur **at all** in the decoded blob.

**Consequence for registration:** the stock phone itself cannot do A5/3, so a live
network that completes a call with this handset must accept **A5/1** for it — the
"TT might require A5/3, a hard wall" worry (project memory) is unfounded. The right
and only strategy is to advertise **A5/1-only** in the MS classmark so the network
never challenges A5/3; there is no hidden A5/3 engine to enable. (`e000`/A5/3 in a
CIPHER MODE COMMAND means *we* advertised A5/3 — fix the classmark, not the DSP.)

---

## 8. TCH / voice traffic (not built in our OS — stock-interface RE map)

Our OS has **no TCH, FACCH or voice-call code** (`os/` has none). This maps the
stock interface from the SDK signal headers (`BASE/PS/export/inc/*.h`), the L1
trace strings (`common/.../trace_id/`) and the command model, so a future
voice-call build has the shape. The ARM-side primitive shape is **[C]** (read from
those headers/strings); the DSP numeric command ids and any traffic buffer
addresses are **[G]** — they live in the ARM firmware (`drps_12E524_0.bin`) and
are not yet traced.

### 8.1 The decisive model: speech does not transit the ARM [C]
The L1↔L3 data primitives (`PH_DATA_IND`/`PH_DATA_REQ`, `MPH_DATA_IND`) carry only
**signalling** L2 blocks, tagged with a `CHANNEL_TYPE_E` — and there is **no "TCH"
channel type** in that enum. The traffic (speech) is handled entirely inside the
DSP: its vocoder encodes mic→speech→channel-coding→burst and decodes
burst→speech→speaker, exchanging PCM with the audio codec directly (see
`docs/dsp-audio-route.md`). So there is no speech ring in the share map (§2); the
ARM only (a) selects the codec, (b) establishes the channel, (c) starts/stops the
speech loop, and (d) exchanges FACCH/SACCH over the normal burst primitives.

### 8.2 Codec mode — `MPH_CHANNEL_MODE_REQ.chan_mode` (`CHANNEL_MODE_E`) [C]
| val | mode |
|---|---|
| 0 | `SIGNAL_ONLY` — SDCCH / FACCH, no speech |
| 1 | `F_H_RATE_VER_1` — FR / HR speech |
| 2 | `F_H_RATE_VER_2` — EFR |
| 3 | `F_H_RATE_VER_3` — AMR (`MULTI_RATE_CFG_T` carries the active codec set, start mode, thresholds/hysteresis) |
| 4..16 | `DATA_*` — CSD circuit-switched data at various rates |

### 8.3 Signalling channel types — over `DATA_IND`/`DATA_REQ` (`CHANNEL_TYPE_E`) [C]
`SACCH`=0, `SDCCH`=1, `FACCH_H`=2, `FACCH_F`=3, `BCCH`=4, `PCH`=5, `AGCH`=6,
`PBCCH`=7, `PPCH`=8, `PAGCH`=9, `PDTCH`=10, `PACCH`=11, `NO_CHANNEL`=12.
FACCH_F/H = fast associated control, stolen from traffic frames (a stealing flag
in the burst); PDTCH/PACCH are the GPRS packet-data equivalents.

### 8.4 TCH setup sequence [C for shape, G for numeric ids]
1. RR gets ASSIGNMENT COMMAND → `MPH_CHANNEL_MODE_REQ(chan_mode, multi_rate_cfg)`.
2. L1 (MphHandle) → **command 3** `CMDQ_Dedicate_Establish` (our `l1_dm_est` /
   `l1_cell_track`) with the channel-mode selector in word 1 — the same field
   `l1_dm_est` already maps (0/1 pass through, 3→2, 4→3). The trace
   `"CMDQ_Dedicate_Establish: 0x%x, is_dsp_addr_sent %d"` shows a DSP data-buffer
   address is handed over for traffic.
3. On an assignment/handover to another cell: `"TCH search"` / `"TCH BSIC read"`
   (sync on the traffic cell, like `l1_cell_sync`).
4. Start the speech loop: `CmdQ_StartVoiceUpLoad` + `CmdQ_StartVoiceDownLoad` DSP
   commands (speech ↔ audio codec); `SCI_EnableMute` mutes.
5. Release: `SCI_StopTransferVoiceData` / the dedicated-release command.

### 8.5 The 26-multiframe TCH schedule [spec, GSM 05.02]
On the assigned timeslot, TCH/F maps traffic over frames 0–11 and 13–24, **SACCH on
frame 12** (its sub-slot rotating over the 104-multiframe), frame 25 idle; TCH/H
interleaves two subchannels. A scheduler would post type-2 RX and type-7 uplink
bursts on these frames exactly as `l1_dm_sched` does for SDCCH/8 — but speech frames
are never read back, only FACCH (stealing flag set) and SACCH blocks reach the RX
ring. `SCI_TransferVoiceData(buffer,size)` is a separate PCM inject/capture path
(e.g. in-call voice memo), not the per-frame speech.

### 8.6 Diagnostic / effect paths (DSP-side, ARM only parameterises) [C]
TCH loopback (`PH_SCT_TCH_LOOP`/`_ACK`, `"No TCH active, ignore LOOP cmd"`), DSP
voice loopback (`DspVoiceLoopbackInd`), and the "magic voice" / "mix voice" effects
(`LAYER1_SetMagicVoiceParam`, `CmdQ_StartMagicVoiceFun` …).

### 8.7 What a voice-call build would need
Extend the dedicated scheduler to the 26-multiframe TCH map; add
`MPH_CHANNEL_MODE_REQ`→command-3 mode selection; route FACCH_F/H + SACCH through the
existing `l1_burst_post`/`l1_uplink_post` primitives; and issue the voice
up/down-load commands (one of the firmware's command ids in §9.0 — the table is
known, the specific voice id is not yet matched to a name). The speech
PHY and vocoder are already in the DSP blob — **no new DSP code is required**, only
ARM-side scheduling and the voice-loop commands. This also depends on the same
dedicated-mode AFC/scheduler work that SDCCH registration needs (§6.6–6.7).

## 9. Command catalog (word 0 = id) [C]

| id | Words | Meaning | ARM | DSP handler |
|---|---|---|---|---|
| 0 | `{0,type,len,0,0}` | send a parameter record / NV block at `+0x6f6`; `type`=0x10 at start | `send_block` / `send_records` / `send_start` | `0x98ab` |
| 1 | `{1,0,...}` | start the frame clock | `l1_run` | `0x9a20` |
| 3 | `{3,cell,mode,rxlev,flags}` | dedicated-mode / serving-cell track | `l1_cell_track` / `l1_dm_est` | `0x9ab4` |
| 4 | `{4,0,1,0,0}` | key the PA for the posted TX frame | `l1_access_post` / `l1_uplink_*` | (TX group) |

Control-block requests (not the CMD ring): `+0xfe0 = 3` + fast-IRQ = ping/crash
dump (FUN_0001a516 / FUN_000185fc), used by `dsp_probe` as a read-back.

### 9.0 The full ARM→DSP command surface [C — Ghidra-extracted]
Decompiling every caller of the poster `FUN_0005f474` in the stock ARM firmware
(`dump_firmware.bin`, headless Ghidra) gives the complete set of command ids the
stock L1 posts — one builder per id, each hardcoding word 0. Our driver uses only
**0, 1, 3, 4**; the firmware defines **34**:

| id | builder | id | builder | id | builder |
|---|---|---|---|---|---|
| 0 send record/NV | `0004b3a4` | 0x0f | `0004b712` | 0x25 | `0004b9ca` |
| 1 start frame clock | `0004b3ca` | 0x10 | `0004b6c4` | 0x26 | `0004b952` |
| 2 activate/wake | `0004b3e4` | 0x11 | `0004b6de` | 0x2b | `0004b96c` |
| 3 dedicated/track | `0004b410`,`0004b458` | 0x12 | `0004b72c` | 0x2e | `0004b938` |
| 4 PA-key TX frame | `0004b49a` | 0x13 | `0004ba44` | 0x2f sleep/wake | `0004ba0c`,`0004ba26` |
| 5 | `0004b4b4` | 0x14 | `0004b676` | 0x33 | `0004b5b6` |
| 7 | `0004b59c` | 0x1a | `0004b760` | 0x34 | `0004b5d0` |
| 8 | `0004b65c` | 0x1b sleep/wake | `0004b7ec` | 0x37 | `0004b91e` |
| 9 | `0004b642` | 0x1c | `0004b9a4` | 0x39 | `0004b628` |
| 0x0a | `0004b60e` | 0x1d | `0004b746` | | |
| 0x0b | `0004b6f8` | 0x1e (8-byte) | `0004b828` | | |
| 0x0c | `0004b690` | 0x1f | `0004b860` | | |
| 0x0d | `0004b6aa` | 0x21 | `0004b806` | | |
| | | 0x22 | `0004b884` | | |

**Named with confidence** (from the code, not strings): 0 (parameter record / NV),
1 (start frame clock), 2 + 0x1b + 0x2f (the DSP wake/sleep sequence,
`L1Main_ActiveDspByCmds` FUN_0002f11a), 3 (dedicated establish / cell track — two
builders, the 5-arg `0004b410` called from `FUN_000589d8` §6.7, and a variant
`0004b458`), 4 (PA-key TX frame).

**Why the other ~26 ids can't be named from this image** (verified, not for lack
of trying): this build logs L1 command activity through an *indexed trace-string
table*, not inline string prints. The format strings (`CmdQ_StartVoiceUpLoad`,
`send rach state`, `Error Band in DSPTxReq`, `SetAMRThrehAndHyst`, …) are present
in `.rodata` and appear in a table of `char*` pointers, but the emitting code
references them by **numeric index**, so there is no code→string reference to
follow — Ghidra resolves a *directly printed* string like `"DSP Assert ID:%d"`
(xref'd from `FUN_0003ab0a`) but finds no reference to any command-queue string at
any relocation base. The specific voice strings (`StartVoiceUpLoad`/`DownLoad`,
`Dedicate_Establish`, `WAKEUP_DSP`) are not in any firmware image at all (compiled
to bare ids). Naming the rest would mean reconstructing the trace-index table and
matching indices to the builders — a separate effort, not reachable by decompiling
the builders (which carry no name).

### 9.1 Raw escape hatches & lifecycle
Beyond the typed primitives, `l1_api` exposes thin passthroughs for anything not
yet wrapped: `l1_fi_post` (post a caller-built FI, re-stamping the frame number),
`l1_cmd_post` (arbitrary command), `l1_share_r16`/`l1_share_w16` (bounded raw
share access), `l1_next_fn` (the ARM's local frame counter). Lifecycle/diagnostic:
`dsp_stop` (hold reset, hand the IRAM banks back to the ARM), `dsp_hold_reset`,
`dsp_set_trace` (patch stubs into the downloaded image to trace DSP start-up),
`dsp_selftest` (download a hand-written TeakLite program to prove the DSP runs),
`dsp_probe` (the boot read-back), `nv_imei` (read the phone's own IMEI from NV).
TX calibration: `l1_tx_power_set` has four modes (OFF/MIN/NORMAL/MAX),
`l1_cal_readback` reports the APC setpoints actually sent, `l1_tx_band_caps` reads
the per-band TX-enable bytes from NV item 3.

---

## 10. Worked example: a Location Update, primitive by primitive

How the primitives above compose in time for a real registration
(`os/net/gsm_lu.c reg_lu_run`, over `l1_dm_sched`). Each step names the L1 call
and the share-memory action it drives. "cell FN" = `(local_fn + fn_offset) % (26·51·2048)`.

| # | Phase | L1 call(s) | Share-memory action |
|---|---|---|---|
| 0 | **Camp** | `cell_select`→`cell_sync`→`cell_read` | type-9 FCCH + type-2 RX bursts; learn `fn_offset`, `bsic`, LAC/RACH params; `cell_gain_set` seeds AGC |
| 1 | **Arm TX** | `l1_tx_power_set(NORMAL)` before `l1_start` | APC tables sent uncapped (§6.4); TX interlock off |
| 2 | **RACH** (CHANNEL REQUEST) | `l1_access_post(index,slot,bsic,ra)` ×≤4 | type-6 FI + TXBUFQ{0,bsic,0,ra} + cmd{4,0,1}; PA keyed 1 frame |
| 3 | **Wait grant** | `cch_read`/dm pump | type-2 RX on CCCH; match IMMEDIATE ASSIGNMENT to our RA + RACH frame ref |
| 4 | **Enter dedicated** | `l1_track_mode=2`; `l1_cell_track`; `l1_tx_ta_set(ta)` | cmd 3 {3,cell,2,rxlev,flags}; TA→`+0x3e4` + word-5 flag |
| 5 | **SABM** (carries LU REQUEST) | `l1_dm_sched(...,ul=sabm)` | 4× type-7 uplink burst at SDCCH UL window + TXBUFQ type-3 block(burst 0)/type-0 ctl; retrack before each TX |
| 6 | **Pump + re-lock** | `l1_dm_sched` each ~115 ms | type-2 RX (tag 9) at SDCCH DL window; FCCH re-read at cell `FN%51∈{0,10,20,30,40}` (§6.6); retrack after each UL |
| 7 | **UA** → link up | (RX decode) | LAPDm `ESTABLISHED`; SABM stops retransmitting |
| 8 | **CLASSMARK CHANGE** | first L3 uplink | type-7 UL block = MS Classmark 2 advertising A5/1 only (forces A5/1, §7) |
| 9 | **AUTHENTICATION REQUEST** (DL) | SIM RUN GSM ALGORITHM (off-thread) → `reg_sres` | DL block decoded; AUTH RESPONSE (SRES) sent as a type-7 UL block |
| 10 | **CIPHERING MODE COMMAND** (DL, RR 0x35) | `l1_cipher_set(kc,alg,fn_offset)` | Kc→`+0x3f8`; `cipher_w5=0xA000`; COUNT seeded at `+0x3f0` on next FI (word-1 bit 8=0) |
| 11 | **CIPHER COMPLETE** (enciphered) | `reg_cipher_complete` | first UL block with word-5 `|= 0xA000`; both directions cipher from here |
| 12 | **LOCATION UPDATING ACCEPT** (DL) | — | registration complete |

Running alongside (steps 5–12): a **SACCH MEASUREMENT REPORT** once per
102-multiframe (`l1_sacch_set`/`build_sacch`, §6.5) so the network does not time
the radio link out. It is built and wired but **currently disabled** in `reg_lu_run`
— its block posted ahead of the SABM and the DSP would not drain it, jamming the
TXBUFQ head; re-enabling it needs the pacing fix noted there.

The two standing hazards this sequence works around, both AFC-related (see the
project's "SDCCH AFC drift" notes): the dedicated scheduler reads only the SDCCH
timeslot, so without the step-6 FCCH re-reads the DSP synth free-runs and the
SDCCH CRC drifts to ~0 within a second; and every PA key-up (each uplink burst)
pulls the synth off the cell, so a `l1_cell_track` re-lock is issued immediately
before each transmit and after each completed uplink.

## 11. Function cross-reference

| Our ARM function | Stock `FUN_` | Role |
|---|---|---|
| `sm_w16`/`sm_r16` | FUN_00061040 / FUN_00061066 | share word R/W discipline |
| `post_cmd` | FUN_0005f474 | command ring + doorbell |
| `post_fi` / `fi_init` | FUN_0005ef76 / FUN_0004f5fc | FI ring + skeleton |
| `l1_ask_more` | FUN_0005f208 | request an interrupt N frames ahead |
| `l1_service` | FUN_0004ee54 / FUN_0003ab0a | drain RESP/BEP/message |
| `send_records`/`send_block` | FUN_00060650 / FUN_000606ea | parameter download |
| `l1_run` | DSP 0x5a874 / FUN_0002ea1a | start frame clock (cmd 1) |
| `l1_pm_post`/`l1_pm_read` | FUN_0004f646 / FUN_0005ed64 | power measure (type 11 / POWERMONQ) |
| `l1_fcch_post`/`l1_sch_post` | FUN_0005096e | sync search (type 9/10) |
| `l1_burst_post` | FUN_000504f8 | RX block burst (type 2) |
| `l1_rx_read` | FUN_0005f692 | drain RX ring |
| `l1_access_post` | FUN_0004fe0a | RACH (type 6) |
| `l1_uplink_post`/`_fi`/`_desc` | FUN_00050426 / FUN_0004f526 | uplink burst (type 7) + TXBUFQ |
| `tx_desc_post` | FUN_0005f332 | TXBUFQ descriptor |
| `l1_tx_level_set`/`l1_tx_ta_set` | FUN_00062cce / FUN_00062d92 | PCL / TA |
| `l1_sacch_set` + `l1_dm_sched` | stock SACCH tables | SACCH/C8 uplink schedule |
| `l1_cell_track`/`l1_dm_est` | FUN_000589d8 / FUN_0004b410 | command 3 (dedicated/track) |
| `l1_cipher_set` | (DSP dis 0x85A5 / 0x8640) | A5 Kc + mode |
| `l1_arfcn_index` | DSP FUN 0x5287 | ARFCN → flat channel index |
| `l1_cell_gain_set`/`l1_gain_for_rxlev` | FUN_0005cafe | per-cell AGC for RX word 6 |
| `l1_sleep_post` | FUN_0004fc5c | multi-frame sleep (type 1) |

## 12. The one DSP, many interfaces

The single DSP firmware image in NOR (`0xcc874`, booted per §3.5) serves **both**
the GSM layer 1 **and** the audio codec — the same running program, the same share
window at `0x10000000`, the same control block at `+0xfe0` and the same download/
interrupt mailbox. The two just use different command and ring sets, so every ARM
subsystem that "speaks to the DSP" goes through the transport in §3:

| Interface | What it does | Where it is mapped |
|---|---|---|
| Boot / download / control block | load the image, mailbox, ping/dump | §3.4, §3.5 (this doc) |
| **GSM L1 RX** | measure, sync, BCCH/CCCH, SDCCH | §4–§5 (this doc) |
| **GSM L1 TX** | RACH, uplink, power/TA, dedicated | §6 (this doc) |
| **A5 ciphering** | Kc + mode on the FI word 5 | §7 (this doc) |
| **GSM TCH / voice traffic** | codec mode, speech loop, FACCH/SACCH | §8 (this doc; unbuilt) |
| **Audio codec** (music) | MP3/AAC/AMR-NB/tone decode, record | `docs/audio-dsp-protocol.md`, `layer1_audio_codec.h` (`Audio_Sub_Cmd_E`, `LAYER1_AUDIO_CODEC_Data`) |
| **Voice effects / loopback** | magic/mix voice, PCM transfer, TCH loop | §8.6; `layer1_voicecall.h` |

Not on this DSP: the **FM tuner** is a separate RTC6211N chip over GPIO/I²C (see
`os/drivers/fm.c`), and the analog **audio codec** registers at `0x82001a00` are
driven by the ARM directly (`os/drivers/audio.c`), not through the DSP mailbox.

See also: `docs/gsm-plan.md` (full decode, confidence tags), `docs/a5-cipher-re.md`
(ciphering), `docs/l1-osmocom.md` (L1CTL primitive map and what remains to build),
`docs/audio-dsp-protocol.md` (the audio-codec side of the same DSP).
