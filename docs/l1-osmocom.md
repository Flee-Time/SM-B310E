# L1 for an OsmocomBB client stack

Goal: a **stock-faithful** layer 1 on the SC6530C DSP that a full GSM client
stack (OsmocomBB layer 2/3, ported to run on the ARM in `os/`) can sit on top of,
**without excluding any L1 function the upper layers need**. The L1 is the stock
DSP (booted from NOR) plus our ARM-side driver (`os/drivers/dsp.c`,
`os/drivers/l1cell.c`); L2/3 will call it through an L1CTL-style adapter.

Design rule (2026-11-09, user): do not write reduced/test-only primitives. Mirror
the stock L1 - every frame-instruction type, every command - and expose the full
primitive set, even those nothing uses yet. Build out the missing ones faithfully
from the RE (docs/gsm-plan.md), not ad-hoc.

## Architecture

    OsmocomBB L2/3 (RR / MM / CC / SMS, host code) -- ported into os/
        |  L1CTL primitives (a thin adapter over l1_api)
    our L1 driver: dsp.c (rings, commands) + l1cell.c (cell layer)
        |  share memory at ARM 0x10000000 = DSP word 0xa800 (rings)
    stock GSM L1 (the DSP blob from NOR) + NV calibration

The DSP does the real PHY (TDMA scheduler, GMSK, channel coding, interleaving,
ciphering, measurement). Our ARM driver posts frame instructions and commands and
drains the result rings. L1CTL maps OsmocomBB's primitives to those.

## L1CTL primitive map (OsmocomBB -> our L1)

| L1CTL primitive        | our l1_api / plan                         | stock ref (RE)              | status |
|------------------------|-------------------------------------------|-----------------------------|--------|
| RESET_REQ / init       | dsp_boot + start + run                    | records, 14 NV blocks, cmd {0,16}, cmd {1,0} | done |
| FBSB_REQ (sync+BCCH)   | cell_sync (FCCH t9 + SCH t10) + cell_read | FUN_0005096e, type 10       | done |
| CCCH_MODE_REQ          | cch_read (FN mod 51 schedule)             | sharedmem_proc CCCH         | done |
| DATA_IND (DL block)    | rx_read (RX ring +0x40c) / cch_read       | FUN_0005f692                | done |
| PM_REQ / NEIGH_PM_REQ  | pm_post + pm_read / rxlev_measure         | type 11, POWERMONQ +0x5c0   | done |
| RACH_REQ               | access_post(index,slot,cal,ra)            | FUN_0004fe0a (t6), cmd {4,0,1} | done, SDR-verified |
| PARAM_REQ (TA)         | timing-advance field in the UL frame      | type-7 builder word, cmd 3  | TO BUILD |
| PARAM_REQ (TX power)   | the PCL/power path (now clamped for safety) | APC tables, band field    | TO BUILD (gated) |
| DM_EST_REQ (SDCCH/TCH) | tune + serving cell (cmd 3) + UL+DL bursts | FUN_000589d8 (cmd 3)        | TO BUILD |
| DATA_REQ (UL block)    | uplink_post (type-7 burst + type-3 block) | FUN_000504f8/50426/4f526    | built, RE-faithful, HW-gated |
| SACCH (DL/UL)          | the SACCH block schedule on the dedicated chan | stock SACCH tables     | TO BUILD |
| Ciphering (A5/1..3)    | a DSP command to set Kc + cipher mode     | find the cipher command     | TO BUILD |
| DM_REL_REQ             | stop UL+DL, back to idle/camp             | -                           | TO BUILD |
| TCH_MODE_REQ / TRAFFIC | the DSP voice codec path (FR/HR/EFR/AMR)  | DSP traffic rings           | TO BUILD |
| handover (sync/async)  | SCH on the target cell + UL on new chan   | type 10 on new cell         | TO BUILD |

## Done, stock-faithful
- Lifecycle: dsp_boot (NOR image, SPRD-LZMA), l1_start (50 records + 14 NV blocks
  incl. the APC power tables), l1_run (frame clock). RESET_REQ.
- Receive: FCCH/SCH sync, BCCH + full System Information parse (SI 1-4/13), CCCH
  paging/access-grant, cell selection (C1) and camping. The raw-block primitive is
  rx_read; the cell/cch helpers are the current test harness over it.
- Measurement: per-ARFCN power (POWERMONQ), rxlev.
- RACH_REQ: access_post posts a type-6 access burst with the stock descriptor
  {0, cal, 0, ra} (byte 3 = the 8-bit channel request) + command {4,0,1}. Confirmed
  over the air to emit spec-compliant access bursts (SDR, training sequence 87-90%).
  The min-power clamp interlock stays until full-power use is separately agreed.

## Uplink TX (DATA_REQ) - built 2026-11-09, RE-faithful, NOT HW-verified
`l1_uplink_post(index, tag, burst, tn, tsc, cell, cal, block[23])` in dsp.c. The
complete stock path, from the RE:
- the 23-byte L2 block -> TXBUFQ as a **type-3** descriptor (FUN_0004f526;
  length table[3] = 0x17 = 23). Distinct from the 4-byte RACH descriptor (type 0).
- the burst is a **type-2 rx frame rewritten to type 7** (FUN_00050426): word0 =
  7 | 0xa000, words 6 and 7 = the ARFCN index, word 10 = the cal slot, word 3's
  high nibble cleared; word1 keeps tag/burst/FN, word4 the TSC (as for rx).
- then a **{0, cal, 0, burst}** control descriptor and the arm command {4,0,1}.
- the stock does this inside a **4-burst-per-block scheduler** (FUN_000504f8) that
  interleaves the uplink burst with the three downlink rx bursts of the block, on
  the serving cell set up by **command 3** (FUN_000589d8: cell, word2, rxlev, slot).

Our l1_uplink_post posts ONE uplink burst (burst 0..3) with the channel context
passed in (from the immediate assignment), not read from a stock scell struct. It
compiles and is wired into l1_api, but it CANNOT be hardware-verified yet: a
dedicated channel only exists after a network immediate-assignment, i.e. the gated
registration flow. The exact block-vs-burst descriptor cadence and the multiframe
timing are confirmed there, against real DSP behaviour, before it is trusted.

Still to integrate for a working uplink: the serving-cell/command-3 context infra
(our RX path doesn't use command 3 today - it addresses channels directly in the
frame), the per-block 4-burst multiframe scheduler, and the HW test.

## Registration clearance (owner, 2026-11-09)
The device owner has CLEARED the device's own SIM to associate (register) with a
**Vodafone** cell at **normal power**, to be done only **when the full tool stack
to complete it exists** (not before). This lifts the registration/legal gate for
the end goal, conditional on readiness. Until the stack is complete the min-power
clamp interlock stays the default; for the cleared registration run it relaxes to
normal power, with bounded, deliberate attempts. Everything below is the path to
"all the required tools".

## Built 2026-11-09: the SDCCH scheduler
l1_dm_sched (l1cell.c): one dedicated-channel session - posts the subchannel's
downlink rx bursts and, when a uplink block is given, its uplink normal bursts,
per the SDCCH/8 schedule (DL block at FN mod 51 in [k*4, k*4+3], UL offset by 15),
and collects the DSP-decoded downlink blocks. The DATA_IND/DATA_REQ pump, on the
verified l1_burst_post / l1_uplink_post primitives. Compile-verified; the uplink
block framing, the dedicated tag (9), SACCH/C8 and fill frames are confirmed at
the cleared registration, where it is first HW-testable.

## To build (the full stack; none to be excluded)
1. **Dedicated mode (DM_EST)** - l1_dm_est posts the command-3 piece; l1_dm_sched
   is the scheduler. The
   real work is that command 3 runs inside the stock **L1 state machine** (PSRAM
   FUN_003b13c6 -> FUN_000589d8, state via FUN_0002f348 "SetState"), which drives
   RF/AFC/timing setup and the per-block dedicated scheduler - a subsystem our L1
   does not have. Serving-cell setup is command 3 (FUN_000589d8:
   cell, word2, rxlev, slot); then schedule UL+DL bursts on the assigned TN/TSC/
   subchannel. SDCCH first, then TCH.
3. **SACCH** - the slow associated control channel schedule (DL measurement orders,
   UL measurement reports).
4. **Ciphering** - the DSP command that loads Kc and selects A5/x. Find it in the
   command handlers (group/type), mirror it.
5. **Power control (PCL)** - the real TX power path, currently pinned to the
   calibrated minimum by the clamp. For registration/traffic the network commands
   the power; wire PARAM_REQ to the band/APC selection. GATED: full-power TX needs
   the legal/safety step agreed first ([[b310e-gsm-registration]]).
6. **TCH / voice traffic** - the DSP's traffic rings and codec modes.
7. **Timing advance, handover, measurement-report assembly.**

## DM_EST establishment sequence (RE 2026-11-09, the implementation foundation)
The stock L1 state machine routes command 3 through FUN_000589d8 (low region,
decompiles cleanly; the PSRAM dispatcher FUN_003b13c6 is only the router and
disassembles poorly - we don't need it). FUN_000589d8 does, for a dedicated
channel (its arg has a channel-mode field, 0/1/2):
1. post command 3 to the DSP (FUN_0004b410: word0=3, mode selector, mapped gain,
   frame, aux) - l1_dm_est covers this.
2. RF/analog register setup: FUN_00062dc2 (a single reg write), FUN_00062d78 (a
   double-buffered reg: shadow[1]=v, live=shadow, dirty=1), FUN_00062dce
   (reg[6]=computed, then FUN_00063000). These program the channel's RF params
   (gain from scell[+8], channel config). <- the RF-control layer to replicate.
3. set the L1 state: FUN_0002f348 (state 4, or 6 when mode==2) - "SetState normal".
4. reset FUN_0005c064 (zeros a small state pair).
5. rework the 26-slot frame-instruction ring (FUN_0005f85c, ~1.2 KB): walk the
   ring from the DSP's current "out" to "in", clearing/re-marking entries so the
   dedicated channel's rx(+uplink) bursts are scheduled. <- the scheduler.

So a working DM_EST = l1_dm_est (have) + the RF-control writes (step 2, addresses
to map) + the L1-state set + the ring rework/scheduler (step 5). All in the
decompilable region, so RE-able; untestable until the (now cleared) registration.

### Refinement (RE 2026-11-09): most of step 2 is stock-internal, skip it
The step-2 RF-control writes target SHADOW STRUCTS in the stock's PSRAM data
(DAT_00062ec8 = 0x0428b8c0, DAT_00062e74 = 0x0422fbd8; FUN_00063000/FUN_0002ee34
set mode/"PGPARAM" fields in them). They are the stock's own ARM-side bookkeeping
at fixed stock addresses - NOT hardware and NOT our memory map. The DSP gets the
real per-channel config through the SHARE-MEMORY protocol we already drive: the NV
cal (sent at l1_start), command 3 (serving cell -> l1_dm_est), and the frame
instructions' cal-slot / ARFCN / TSC fields (l1_burst_post rx, l1_uplink_post tx).
So our L1 does NOT replicate the stock shadow structs. Dedicated mode for us is:
  command 3 (have) + the per-block SDCCH/TCH multiframe SCHEDULER.
The scheduler is pure ARM-side GSM TDMA (GSM 05.02): on the assigned timeslot,
post the downlink rx bursts and the uplink tx bursts at the subchannel's frames of
the 51-multiframe (SDCCH/8) or 26-multiframe (TCH), collect rx blocks (DATA_IND),
send uplink blocks (DATA_REQ). It reuses the verified rx/tx primitives and needs
no new RF RE - the tractable next build. HW-testable only at registration.

## Notes
- L1CTL is a thin adapter: it should not re-implement PHY; it marshals primitives
  to l1_api and the result rings to DATA_IND/confirms. Keep l1_api the single
  stock-faithful surface; the adapter is mechanical.
- Safety interlock: access_post (and any future keyed-TX primitive) refuses unless
  the min-power clamp is armed, until full-power operation is agreed. The adapter
  must preserve this until then.
- Privacy of the test harness (not storing other phones' paging) is a harness
  choice; the owner's own stack processes its own paging normally.
