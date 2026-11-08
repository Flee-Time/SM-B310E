# GSM networking: how the stock stack is built, and a plan

This page covers what it would take to make calls and send SMS from our own
software, using the phone's own proprietary GSM stack from its NOR. It is
a planning survey (2026-11-06, Ghidra on `e52q7a` plus the repository's
DSP/boot docs). Nothing here is implemented yet.

Tags: **[C]** read from the stock binary, **[L]** likely, **[G]** guess.
NOR addresses are offsets in the stock image.

## How the stock image is laid out

**Uncompressed kernel (NOR 0x10000–0x12e524, execute in place)** [C]
- Contains the drivers, the ARM side of layer 1, the SIM hardware driver,
  serial/USB, and NV and IMEI handling.
- The ThreadX/SCI kernel code is copied to PSRAM at boot (from about NOR
  0x1095ec to 0x04000010+).

**Compressed, demand-paged image (NOR 0x12e524)** [C]
- An LZMA image, decompressed page by page on MMU faults (`mmu_vm_bzp.c`,
  using the LZMA engine at 0x20E00000 and IRQ 30).
- Its first substream, `user_raw`, is 3.6 MB decoded and holds the whole
  protocol stack and the MMI, interleaved. The virtual base is probably
  0x08000000 [L].

**Layer 1, ARM side** (`Base_13A/Layer1`, NOR ~0x2d000–0x63500) [C]
- Thread `T_P_LAYER1` (entry 0x2e98e, priority 0x13, a 40-entry queue).
- Frame instructions go to the DSP through a 26-slot ring in DSP shared
  memory at 0x10000000. It is refilled 2 frames (~9 ms) ahead, so it is
  soft real-time.
- DSP interrupts arrive on INTC line 13.

**Layer 1, DSP side** [L]
- An LZMA blob at NOR 0xcc874 (65 KB, 267 KB decoded), downloaded by
  `layer1_dsp_download.c`.
- The DSP keeps TDMA timing and drives the radio: on-die transceiver, PA
  ramp, band/antenna switching and AFC. The ARM never touches RF
  registers. The per-band control logic lives in NV item 1.

**Protocol stack (in `user_raw`)** [C]
- RR, MM, CC, SS, SMS (SMR/SMC), LAPDm, the SIM manager and SIM toolkit.
- The MN API layer (call/SMS/phone/SIM/SS), as the MMI uses.
- An AT command interpreter (task `T_P_ATC`, 67 `+C` commands plus
  Samsung factory commands).
- Every task exists twice, one per SIM slot (dual SIM).

## What the stack needs from its OS

- **RTOS:** ThreadX behind the SCI API: threads, signal queues, events,
  timers, mutexes and pools. Hundreds of call sites in `user_raw` point at
  kernel code.
- **Demand paging,** or about 3.6 MB of RAM for the decoded stack. The
  phone has about 4 MB of PSRAM, of which stock uses about 2.9 MB.
- **NV storage:**
  - The fixed NV list is at NOR 0x680000 (`{id, len, data}` items).
  - Calibration is item 2, DSP and RF parameters item 1, classmark item 3,
    IMEI item 5.
  - Samsung production data is at 0x68c000.
  - A runtime copy lives in the EFS area.
- **DSP firmware download,** the shared-memory handshake, and IRQ 13
  latency.
- **26 MHz crystal;** AFC trim is done by the DSP.

## Options

| Option | What it means | Effort | Verdict |
|---|---|---|---|
| (a) Shim | Run the stock protocol tasks under a ThreadX/SCI shim in a new OS | 12–24+ months | Effectively hosts most of the stock kernel; the RAM budget blocks it |
| (b1) Dual boot | Use the existing "boot stock" menu entry | none | Works today, no integration |
| **(b2) Injected task** | Stock boots as the modem OS; one extra thread (loaded from SD) drives the MN APIs or feeds the AT interpreter | 3–6 months | **Most realistic** |
| (b3) MMI replacement | Replace stock's UI but keep its services | 6–12 months | The MMI also owns PS power-on, PIN, call audio and charging |
| (c) Apps inside stock | A hosted, Rockbox-like app layer on top of b2 | +4–8 months | Small apps only; maybe 1 MB free |
| (d1) Reboot to stock | Our OS reboots into stock to call or text | 1–2 weeks | Cannot receive anything |
| (d3) Open stack | OsmocomBB-style replacement | years | Needs the DSP/RF interface; unsafe to transmit with |

**Recommendation: (b2), then optionally (c).** Stock layer 1, the DSP
firmware and the calibration stay byte-identical, which avoids every
timing, calibration and transmit-safety risk. QEMU can exercise
everything up to layer 1 (AT, MN, the hook thread). Network attach, calls
and SMS need the phone, because QEMU does not run the DSP.

**Direction change (2026-11-06):** the user wants full control of the
OS, which rules out every option that keeps stock as the base. The plan
is now our own layer 1 on the stock DSP firmware and calibration (read
from NOR), with OsmocomBB's layer 2/3, inside our own OS
([os-design.md](os-design.md)). M0 is confirmed: stock calls and texts
on the user's network, so 2G is there.

**Layer-1 share-memory layout** (`sharedmem_proc.c`, base pointer
`*0x0422fb80`, cleared 0xfc0 bytes at layer-1 start, FUN_0005eca2):

| Offset | What | Index words (in/out) |
|---|---|---|
| +0x000 | frame instructions: 26 × 22 bytes. Word 0 bits 4:0 = frame type, bits 15:9 flags (0x8200/0x8400/0x8800/0xc000 set by the "ask more frames" logic); word 1 bits 15:9 + word 2 = frame number (23 bits, wraps at 0x297000); word 10 bits 15:10 = channel tag | +0x23c / +0x23e |
| +0x240 | TX data: 4 × 62 bytes (word 0 tag, word 1 type, then data byte-swapped in 16-bit words) | +0x338 / +0x33a |
| +0x33c | ARM→DSP commands: 8 × 10 bytes | +0x38c / +0x38e |
| +0x390 | DSP→ARM responses: 8 × 10 bytes | +0x3e0 / +0x3e2 |
| +0x3e4, +0x3ec | per-slot parameters (two 4-entry halfword tables) | – |
| +0x40c | received blocks: 6 × 72 bytes (bytes from +0x12 swapped pairwise) | +0x5bc / +0x5be |
| +0x5c0 | **power-monitor results** ("POWERMONQ"): 30 × 8 bytes | +0x6b0 / +0x6b2 |
| +0x6ec..+0x6f6 | a two-entry buffer pair (indexes +0x6ec/+0x6ee/+0x6f0/+0x6f2), data from +0x6f6 | |
| +0xbf6 | a second data area (pointer kept at 0x0422fb88) | |
| +0xeea | 6 extra bytes per frame instruction | |
| +0xfa0 | BEP reports: 7 × 4 bytes | +0xfbc / +0xfbe |

All rings advance "in" by the producer and "out" by the consumer. The
layer-1 thread's entry (FUN_0002e98e) turns on the share-memory clock,
clears the area, starts the DSP (`dsp_operate.c`, `layer1_dsp_download.c`),
installs the DSP interrupt handler and writes 0x8181 to 0x10000ffe.

### Milestone 4 findings: power measurement and parameter download (2026-11-07)

Ghidra (headless, read-only) on `dump_firmware.bin`. Function names are
the ones Ghidra gave. The OS side is `os/drivers/dsp.c`.

**Ring discipline** (`sharedmem_proc.c`) [C]
- Share words are written until they read back (FUN_00061040) and read
  until two reads agree (FUN_00061066); both give up after 6 tries.
- In every ring "in" is the slot written last, not the next free one.
  - The writer moves to in+1, fills that slot, then stores the new in.
  - The reader moves out+1 and reads that slot until out == in, then
    stores out.
  - So a ring is empty when in == out, and full when in+1 == out.
- Frame instructions (FUN_0005ef76): the 6 extension bytes go to
  +0xeea + slot×6 and the 22 bytes to slot×22, then in (+0x23c) moves.
- Commands (FUN_0005f474): 8 slots of 10 bytes, in at +0x38c and out at
  +0x38e. After the store the ARM rings the DSP by writing 1 to
  0x8b000160 (MCU_IRQ_SET, FUN_00067fee).
- 0x8b000160 takes plain writes (1 MCU_IRQ_SET, 4 DSP_IRQ_CLR, 8
  DSP_FRQ_CLR), not read-modify-write.
- "Ask more frames" (FUN_0005f208(3)): flags 0x8200 are ORed into word 0
  of the instruction 3 slots before in. The stock asserts that at least
  2 more frames are still queued.
- The share-clock semaphore (FUN_0005f438 / FUN_0005f402) calls
  FUN_0001a514, an empty function on this build. So no share-memory clock
  is switched here [C].

**Frame instruction format** (`frameinstrutil.c`) [C]
- Skeleton (FUN_0004f5fc / FUN_0004f628): bytes 0–11 = 0, bytes 12–21 =
  0xff, and the 6-byte extension = 0xff.
- Word 0 bits 4:0: the type.
- Frame number: word 1 bits 15:9 hold its high 7 bits, word 2 its low 16.
  It counts modulo 0x297000, the hyperframe.
- FUN_0005f6f0 reads the frame number of the instruction at out. That is
  how the ARM learns where the DSP's frame clock is [C].
- Type 0 (FUN_0004fb10) is the idle frame: the skeleton with a frame
  number. The stock fills gaps with it.

**Power-measurement instruction: type 11 (0x0b)** (FUN_0004f646) [C]
- Called from the power-sweep paths of `gsmframegen.c` (FUN_00054a3c,
  FUN_00054524) with meas_count = 6 (FUN_00034fec returns 0 on this
  build, else 3). The assert allows 1–6 (MAX_RXLEV_NUM = 6).
- Word 0 = 0x000b, and word 0 is masked to 0x1ff at the end.
- Measurements 1–4 sit in words 6–9; measurements 5–6 sit in extension
  words 0–1.
- Extension word 2 and word 10 stay 0xffff.
- Each measurement word holds the arfcn index in bits 9:0 (0x3ff means
  empty) and a 6-bit value in bits 15:10.
- The list comes from FUN_0005cefc / FUN_0005ce46 (`rxlevmon.c`).
  - That list walks the BA table and takes channels whose state byte is
    below 5.
  - The 6-bit value is the channel's byte at table+4, or a global default
    when that byte is 0. It is changed when a result comes back
    saturated, so it is the AGC gain hint [L].
  - The global default lives in RAM (0x0422f030) and is not known
    statically [G].
- The arfcn field is the stock's arfcn index, not the ARFCN.
  `arfcn_index < RF_TOTAL_CHANS` with RF_TOTAL_CHANS = 0x3cb = 971 [C].
- 971 is exactly 125 (P-GSM 0–124) + 49 (E-GSM 975–1023) + 374 (DCS
  512–885) + 299 (PCS 512–810) + 124 (GSM 850 128–251) [L].
- The order of those bands inside the index is a guess [G]. P-GSM
  first, so index = ARFCN for 0–124, is the likely one.
  `l1_arfcn_index()` uses that order.

**POWERMONQ results** (+0x5c0, 30 × 8 bytes; in +0x6b0, out +0x6b2) [C]
- FUN_0005ed64 reads them, called from FUN_0005aad0 / FUN_0005ab44 /
  FUN_00045444. The consumers are FUN_0005c8ba (camped), FUN_0005c3e2
  (power sweep) and FUN_000453ee (calibration).
- Halfword 0: the arfcn index (FUN_0005b762 maps it to a BA slot).
- Halfword 1: the level. Bits 11:4 are the rxlev, asserted ≤ 110, so the
  unit is 1/16 [C]. The power sweep rounds it with (v + 8) >> 4 [C].
  Whether 110 means −110 dBm (bigger = weaker) or the GSM RXLEV sense is
  not settled [G].
- Halfword 2: a signed value that only the calibration path sums
  (FUN_000453ee) [C]. Its meaning is unknown [G].
- Halfword 3: the status.
  - 1: a valid measurement.
  - 2 and 3: saturated; the gain hint is moved and the channel is
    measured again ("SATURATE_HI" strings at 0xc7c60 / 0xc8748) [L].
  - Which of 2 and 3 is high and which is low is a guess [G].

**`l1dsp_param_ptr` is not the RF download** [C]
- FUN_00074520 / FUN_00074658 (`layer1_dsp_download.c`) use it to pick
  the DSP image segment.
  - FUN_00074520 maps the chip variant to a "matchword".
  - FUN_00074658 walks the image's 5-entry {match, halfwords} table at
    image+0 (data from +0x28) and returns that segment's address and
    size × 2.
- On this phone segment 1 (match 1, 66861 halfwords) is the one loaded.
  Segment 2 is a byte-identical copy [C].
- The image itself (FUN_00074ad6) is NV item 4 when the fixed NV has one.
  Otherwise it is the compiled-in LZMA blob at 0xcc874. This phone's
  fixed NV has no item 4 [C].
- The image decodes to 267484 bytes; earlier notes said 262144, which
  came from a truncating tool [C].

**Parameter records sent after boot** [C]
- FUN_00074430 scans the program for 5a5a a5a5. The rest of segment 1
  after it (1577 halfwords) is the "pOrgData" stream.
- That stream is a list of records: {u16 length (bit 15 a flag), then
  length halfwords, the first an address in DSP data memory [L]}.
- SharedMemProc_StartTranferDataToDsp (FUN_00060bf4, FUN_00060650) sends
  one record per step:
  - It copies the body to share +0x6f6.
  - It posts command {0, 0, length word, 0, 0} (FUN_0004b3a4 →
    FUN_0005f474).
  - It waits for L1 event 0xc, which the DSP interrupt raises.
- At layer-1 start, FUN_0004f580 sends a list of 17 data types and
  FUN_0004f5aa sends 15 more. The type list lives in RAM (0x0422e134), so
  which types carry NV data is not resolved statically.

**RF / calibration parameters: NV items 1 and 2** [C]
- The fixed NV at 0x680000 holds item 2 (calibration, 0x1784 bytes at
  0x687958) and item 1 (0x3f4 bytes at 0x6890e0).
- NV getters: FUN_000225ee returns item 3, FUN_000225f6 item 2 and
  FUN_000225fa item 4. FUN_00022a30(id) returns an item's length.
- `downloadparam.c` reads its ADC_T (0x30 bytes) out of item 2
  (FUN_0004eaa8).
- The "download param" and "calibration param" version checks are at
  0x2f928 / 0x2f8b8. No code references were found to them statically, so
  how item 1 reaches the DSP (probably one of the data types above) is
  still open [G].

**Start-up order** (FUN_0002e98e, `layer1_main.c`) [C]
1. Share clock on (a no-op), then clear 0xfc0 bytes.
2. DSP interrupt set up (FUN_0003ab7e). Line 13 is registered as a FIQ
   (`dsp_operate.c` "fiq_regist_status"). FUN_0003abe6 sets bit 13 in
   0x80000008 (IRQ enable) and 0x80000028 (FIQ enable [L]).
3. Download and boot (FUN_00074b96), then clear again.
4. Clear again, then 0x8181 to 0x10000ffe. 0x10000fe0 is the
   runtime pointer [0x0422e598].
5. The data-type stream (FUN_0004f580).

### Is the DSP program running? (2026-11-08)

The boot handshake only proves the **boot ROM** works: it answers before
the program ever starts. The phone's 66 blocks and `ready 0001` say nothing
about the program itself.

**What the program's own code says** (Teak disassembly of the image, DSP
address = word index in the program segment)
- Share memory is DSP word address **0xa800**, so ARM offset n is
  0xa800 + n/2 [C]. Immediates for all the ring indexes fit this one base
  (the command-ring reader at 0x98ab reads 0xa9c6 / 0xa9c7, the response
  writer at 0x1773 uses 0xa9f0 / 0xa9f1, the message routine uses 0xaff8–a).
- **Correction:** the 2026-08 notes read 0x1000DC30 and 0x1000EB14 as DSP
  memory, on the assumption of a 1:1 mapping. That assumption is wrong, so
  "DLTA 02/10" was not evidence that the program ran. Only 4 KB is share
  RAM, and nothing has shown what those addresses are [L].
- After reset the program sets up its handlers (0x1907: 0x98ab for commands),
  enables interrupts and waits on a flag at 0x1063 [C].
- The message routine at 0x17df spins until 0xaffa (ARM +0xff4) is 1 [C], so
  the ARM must set that ack first (stock FUN_0003ab7e does; the 2026-08
  diagnostic and the first driver did not).
- The parameter records hold function pointers (the pointer 0x628c, the
  routine that writes 1 to 0xaff2, is inside them) [C]. So before the records
  arrive much of the program can't work, including probably the ping [L].

**Control block at +0xfe0** (pointer [0x0422e598]) [C]
| Offset | Meaning |
|---|---|
| +0x00 | ARM: 3 asks for the ping and dump |
| +0x02 | ARM: first DSP word of a dump block |
| +0x04 | DSP: 1 answers the ping; non-zero ends a dump block |
| +0x0e | ARM: words in a dump block (0x400) |
| +0x10, +0x12 | DSP→ARM message code and parameter (FUN_0003ab0a; one code is a "DSP Assert ID") |
| +0x14 | ARM sets 1 to acknowledge; the DSP clears it when it posts |
| +0x1e | ARM: 0x8181 at layer-1 start; no direct reference found in the program |

**Stock crash dump, usable as a read-back** [C]
- FUN_0001a516 is the ping: +4 = 0, +0 = 3, `0x8b000160 |= 2` (fast interrupt
  to the DSP), wait, then +4 must read 1.
- FUN_000185fc is the dump: for 64 blocks, +2 = first word, +0xe = 0x400,
  +4 = 0, wait for +4 non-zero, then read 1024 halfwords from 0x10000000.
- Which DSP memory space it copies is not settled yet [G]. The dump is compared
  with both the program and the records to find out.

**Commands and responses** (corrects the note above) [C]
- A command is 5 halfwords and word 0 is its id: 0 sends a parameter record,
  with the length word in word 2. The stock wake sequence
  (L1Main_ActiveDspByCmds, FUN_0002f11a) uses ids 2, 0x1b and 0x2f.
- Responses (FUN_0005f722, read in FUN_0004ee54) are 5 halfwords; the low byte
  of word 0 picks the handler in a jump table. For a record's answer the stock
  (0x60618) looks at word 1 and sends the next record or raises an L1 event.

**The check: `dsp_probe()`, Tools → DSP boot**

It runs after the boot, bounded at every step. Only 0x10000000–0x10000fff is
touched, and the radio is not used (idle frames only). Results are in
`/b310e-os/dsp-report.txt`, with `dsp-share.bin` and `dsp-dump.bin` beside it.

| Step | What a running program does | Why it counts |
|---|---|---|
| Watch 100 ms | writes share words | the boot ROM's area is below 0x808 |
| Records | takes 50 commands, answers | needs the command handler at 0x98ab |
| Idle frames | consumes the ring, about 4615 µs each | needs its frame timer; the period can't be faked |
| Ping | +4 = 1 | weak alone: QEMU's model answers it too |
| Dump | returns its memory, with the records' words at their addresses | the strongest: data it can only have stored itself |

QEMU cannot show any of this, because its model runs no program. The test
checks that the probe always ends and reports "ping only", never "runs".

**First phone result (2026-11-08, three runs, all the same):** no freeze, and
no sign of a running program.

```
verdict no answer
boot: 66 blocks, 667 ms, ready 0001
at start: 0 non-zero share words
records 0 of 50, irqs 0, storms 0, status bits 00000000
commands: in 1 out 0
ping: before 0000, after 0000
```

The DSP never took the first command, raised no interrupt and did not answer
the ping. Share memory held only what the ARM wrote (first record and command).

**Where the first driver differed from the stock** (read again with objdump,
since Ghidra stops at calls into PSRAM; all [C])
- The stock's "sleep 13 ms" after BOOT_DONE was a misreading: `FUN_00010852(13)`
  disables IRQ line 13. The stock goes straight on to the restart.
- Before the restart `FUN_00060f0a` writes 0xffff to +0xfde, DSP 0xafef. The
  program compares that word with 0xc396 to pick another start path (0x4540).
  We never set it, so it held whatever the RAM contained.
- `FUN_0003ad6e` clears **8 KB** at 0x10000000 under reset, not 20 bytes.
- The ack at +0xff4 was not set (above).
- Not a difference: the stock also downloads all 66861 words. The offset is
  `(block & 0x3f) << 10`, so the last 1325 words have offsets that wrap to 0.
  What the boot ROM does with that is not known [G], but it is the same for both.
- The stock looks for the image in a NOR partition first (`FUN_000a03dc(3)`) and
  falls back to 0xcc874. Whether this phone has that partition is unchecked [G].

**Start-up trace** (`trace_program()` in `drivers/dsp.c`)

The program is sent from a heap copy, so that copy carries 13 small stubs in an
unused gap (0x5f77–0x5fff). Each writes a mark to DSP 0xafe0 + n (ARM +0xfc0 +
2n) and carries on. The NOR is not touched. `trace` in the report is a bit mask:

| Bit | Point | If it is the last one set |
|---|---|---|
| 0 | reset vector | the program runs; nothing else is proven |
| 1 | data memory cleared, main called | |
| 2, 3, 4 | main, then each init returned | stuck in the next init (0xbf2, 0x358e) |
| 5, 7 | handlers installed, about to wait | |
| 6 | fast-interrupt handler 0x3abe ran | the ping reached it |
| 8 | the wait at 0x1907 ended | |
| 9 | command handler 0x98ab ran | our doorbell reaches it |
| 10, 11, 12 | interrupt vectors 0x6, 0xe, 0x16 | which interrupts fire |

Bit 0 clear means the program never executes after the restart, so the fault is
in the restart or the download, not in anything layer 1 does.

**Not yet seen on the phone**
- The trace and the stock-order fixes have not run on hardware.
- POWERMONQ and the power-measurement instruction have never run. They can't
  until the program answers commands.

**Licensing:** never ship stock bytes. As the FM driver does with its init
tables, read or patch them from the owner's own NOR or dump.

### Next stage: sync and broadcast channels (stock reference, 2026-11-08)

This section is incomplete: it was cut short when the work moved to the
problem below. Word n means halfword n of the 22-byte instruction. Many
trace calls (thunk 0xc02dc) are marked no-return in the Ghidra project,
which cuts decompiles short; clearing that flag in a scratch script fixed
most of them.

**More frame types** (`frameinstrutil.c`) [C]
- Type 1 is a multi-frame sleep (FUN_0004fc5c / FUN_0004fd7e).
  - Word 3 bits 11:0 = the count, below 1000.
  - The frame number is start + count − 1: the instruction covers the
    whole span.
  - When the ring is flushed (FUN_000601f4), the frame clock is wound
    back by count − 1, the same as for type 9.
- Type 2 receives a 4-burst block (FUN_000504f8; tables in FUN_00050f6a /
  FUN_0005138e; CBCH in FUN_00051c0c). One instruction is posted per
  burst.
  - Word 1: bits 7:4 = RxChannelTag, bits 1:0 = burst 0–3.
  - Word 3 bits 10:8 = a 3-bit cell value; word 4 bits 10:8 = another.
    One of them is probably the TSC [G].
  - Word 6 bits 9:0 = the serving arfcn index.
  - Words 7–9 hold monitor channels (index | gain << 10; 0x3ff means
    empty).
  - Word 10: bits 15:10 = cell index, bits 9:8 = a card/band mask.
  - AGCH, BCCH and PCH use tags 7, 8 and 9 (the assert at 0xc8130).
    PCH = 9 [L]: the pch_delay path posts tag 9. Which of 7 and 8 is BCCH
    is open [G]. CBCH uses tag 6; the tables also use 3 and 5.
- Types 6 (FUN_0004fe0a) and 7 (FUN_00050426, which rewrites a type 2
  instruction) also queue a TX buffer of type 0. They are not receive-only;
  they are likely RACH [L]. Avoid them.
- **Type 9 is the FCCH search** (FUN_000506e0, FUN_0005096e). The
  instruction covers a window of frames.
  - Word 3 bits 11:0 = the window: 13 when camped
    (SEARCH_NUMBER_CAMPED), 26 before camping
    (SEARCH_NUMBER_BEFORECAMPED; FUN_0005096e, no static callers found).
  - The frame number is the window's last frame.
  - Word 1 bits 7:4 = 1.
  - Word 4 low byte = the serving cell index (0xff if none).
  - Word 5 bit 0 = a flag (FUN_0005096e only).
  - Word 6 = arfcn index | gain hint << 10 (gain from FUN_0005cafe).
  - Word 8 = a per-cell halfword (cell table +0xb0, used when +0xae is
    set). A good SCH resets it to 0xffff, so it is probably a timing or
    frequency hint from an earlier search [G].
  - Word 10 = cell index << 10 | the card mask in bits 9:8.
- **Type 10 is the SCH read** (FUN_000506e0(cell, fn, 10, 0)): one
  frame, the same fields as type 9 without words 3 and 8.
  - It is posted at the frame the cell's SCH is due. FUN_000494a6 works
    that frame out from the cell's frame offset.
  - Word 7 bit 10 is set under a condition (FUN_0002f0c2 /
    FUN_00049ab4).
- Types 12 and 13 are handled like 9 when the ring is flushed: they are
  the dedicated-mode BSIC search [L].

**Results come back on the received-blocks ring (+0x40c), not as
responses** [C]
- The response handler (`dspintisr.c`, FUN_0004ee54) only accepts
  word 0 low byte = 4.
  - Word 1 bit 0 = "more frames needed": it sets 0x0422face and wakes
    layer 1.
  - Word 1 bits 1–5 are DSP error asserts.
- Header of a received block (72 bytes, read by FUN_0005f692, swapped
  pairwise from +0x12):

| Offset | Field |
|---|---|
| +0x00 | type: 1 SCH, 7 CCCH/BCCH block, 6, 8, 9 (FUN_0004dd7c), 3/4/5/0xb/0x10–0x12 dedicated |
| +0x02 | bfi: 0 good, 1 bad (blocks with 1 are dropped) |
| +0x04 | frame number, low 16 bits |
| +0x06 | bits 7:0 frame number high; bits 12:8 cell index (word 10 of the instruction) |
| +0x08 | two bytes; hi − lo corrects the frame offset (calibration path FUN_000461e4) |
| +0x0e | signed halfword, logged: SNR or frequency error [G] |
| +0x10 | low byte logged: rxlev [G] |
| +0x12 | data: SCH 4 bytes; blocks 23 bytes (0x17 in FUN_0004c8a0) |

- SCH data: byte 0 bits 7:2 = BSIC (NCC 7:5, BCC 4:2); T1, T2 and T3'
  follow in the standard SCH bit order. FUN_00061408 rebuilds
  FN = 51·((T3 − T2) mod 26) + T3 + 1326·T1, with T3 = 10·T3' + 1.
- A good SCH in idle mode (FUN_0004ce38) clears the cell's hint words. It
  checks the BSIC against the expected one, reporting a change, and marks
  the cell.
- A block (FUN_0004d7c4 / FUN_0004d91a) with bfi ≠ 1 sends +0x12 to
  layer 2: FUN_0004bd84(4, arfcn, data, crc_ok, fn) and
  FUN_0004a87a(data, bfi, fn, cell).
- Each layer-1 state has its own reader (FUN_0005a8ac … FUN_0005aa50),
  which accepts only some of the types.

**Timing and AFC** [L]
- Each cell's frame number is the serving frame number plus a per-cell
  offset (cell table 0x042841e8, 32 × 0xb4, offset at +0x1c;
  FUN_000484e4).
- No static writer of that offset was found, apart from ±1 helpers with no
  callers (FUN_00048b1c / FUN_00048b56).
- No command for timing or AFC was found: the only ids seen are 0 and
  2/0x1b/0x2f.
- So the DSP most likely keeps timing and frequency itself after FCCH and
  SCH. Which ARM path sets the offset is still open.

**Cell states and order** (`cellsyncinfo.c`, `gsmframegen.c`) [C]
- Cell states (FUN_0004806e): 1 free; 2 FCCH needed; 4 after a type 9 is
  posted; 0x20 after a type 10. When the ring is flushed, 9 goes back to
  2 and 10 to 0x10. 8, 0x10 and 0x80 are synced states [L].
- PLMN search (FUN_00054c8e): up to 8 type-11 power frames, then the
  PCH/BCCH plan.
- Camped idle (FUN_00054572), when spare frames allow:
  1. Type 10 for neighbours whose SCH is due.
  2. With more than 15 frames spare: 2 idle frames, type 9 (13 frames)
     for the best state-2 cell (FUN_00048802), then 1 idle frame.
- BCCH (FUN_00055534 / FUN_0005538a): type 2 bursts at the computed
  delay.
- The very first FCCH search before camping (window 26) was not traced
  to its caller.

**Needs the phone:** whether types 9 and 10 return anything except rx type
1; what +0x08, +0x0e and +0x10 mean; and which tag (7 or 8) is BCCH.

### Why the program may not start (stock comparison, 2026-11-08)

On the phone: all 66 blocks went down and the ready word was 0001. After
that the program wrote no share words, took no command (in 1, out 0),
raised no IRQ 13 and did not answer the ping. Below, the stock path
(FUN_0002e98e → FUN_00074b96 → FUN_0003aa76) is compared with
`dsp_boot()` / `download()` / `l1_start()`. Read from the binary
throughout [C].

**Stock boot, in order** (FUN_0003aa76, FUN_0003ad16, FUN_0003a92a,
FUN_0003a810; register helpers FUN_00067fc8 / FUN_00067fda / FUN_0006802a /
FUN_00068042 / FUN_00067ffc)
1. 0x8b000060 = 0x10000 (reset held). The IRQ 13 handler 0x3aa61 is
   registered for the download: it sets a flag and writes 4 and then 8
   to 0x8b000160.
2. 400 busy loops; share memory cleared (FUN_0003ad6e); arm_ctl (+0) =
   1 until it reads back.
3. 0x8b000060 = 0x10000.
4. 0x8b0001c0 bits 7:3 = 4.
5. 0x8b0001a0 bits 15:0 = 0xa800. FUN_0003a9ec picks 0xa000 for chip ids
   0 and 0x30001 and 0xa800 otherwise.
6. 0x8b0001a0 |= 0x10000.
7. 400 loops; arm_ctl = 1.
8. 0x8b000064 = 0x10000.
9. 0x8b000160 = 4; 0x80000008 |= 0x2000.
10. Wait for dsp_ctl bit 0, then the blocks:
    - Each block: dl_offset, size and data; arm_ctl = 3; wait for bit 1;
      clear arm_ctl bit 1; set bit 2; wait for bit 2; wait for the
      interrupt (event, or a poll of 0x8b000140 bit 2); 0x8b000160 = 4,
      then = 8; clear arm_ctl bit 2.
11. arm_ctl = 8.
12. The handler is removed; +0xfde = 0xffff (FUN_00060f0a).
13. 0x8b0001c0 bits 7:3 = 0.
14. 0x8b0001a0 &= ~0x10000.
15. 0x8b000060 = 0x10000; 99 busy loops; 0x8b000064 = 0x10000.

`download()` makes the same register writes in the same order. The only
register writes it leaves out are 0x80000008 bit 13 during the download
(it polls instead) and the IRQ-handler registration.

**The difference that matters: block offsets wrap at 64 K words** [C]
- FUN_0003a92a computes each block's offset as `(block & 0x3f) << 10`.
  It uses that one value both as dl_offset and as the source offset
  into the program.
- For 66861 halfwords (66 blocks) the stock sends:
  - blocks 0–63 normally;
  - block 64: dl_offset 0, 0x400 halfwords = program words 0–1023
    again;
  - block 65: dl_offset 0x400, 0x12d (301) halfwords = words
    1024–1324 again.
- So the boot ROM never receives words 65536–66860. Those are the
  parameter-record tail (the 5a5a a5a5 mark is at word 65282) and go
  later by command 0.
- `download()` uses `off = b * 1024`. It writes `(uint16_t)off` (0 and
  0x400) as dl_offset but copies `prog[off + i]`, that is words
  65536–66860.
- So **DSP program words 0–1324 are overwritten with record data**,
  including the reset vector (word 0 should be 0x4180 br) and the trace
  stub hooks at 0/6/0xe/0x16. That explains a program that never
  executes an instruction of its own, and trace mask 0.

**Other differences, in falling order of likelihood**
1. **[C] the offset wrap above.** The fix: download only the first
   65536 words, or copy stock and use `(b & 63) << 10` for both offsets.
   Either way the records then go by command 0, as now.
2. **[L] the trace stubs patch the program** (word 1 of the reset vector
   and 12 other sites); the stock never does this. Once item 1 is fixed,
   run once with the stubs off.
3. **[G] timing:**
   - The stock waits 400 busy loops (a few µs) after setting BOOT_EN
     and before arm_ctl = 1 and the release. The driver goes straight
     on.
   - The final reset pulse is 99 loops in the stock and 10 µs in the
     driver.
   - The driver clears 4 and 8 with one write of 0xc; the stock writes
     4 and then 8.
   - None of these should stop the program.
4. **[G] the boot vector:** the stock chooses 0xa000 or 0xa800 from the
   chip id at 0x0422ca1c. 0xa800 fits the program's own immediates and
   the handshake works, so this is unlikely.
5. **Layer-1 start matches:** clear 0xfc0; FUN_0003abe6 (0x80000008 |=
   0x2000, 0x80000028 |= 0x2000); FUN_0003ab7e (clear +0xfe0..+0xffd,
   +0xff4 = 1); clear 0xfc0; +0xffe = 0x8181; then the records.

**The state before the bringup differs too (2026-11-08).** The functions above
match, so everything the stock does *before* them was compared: QEMU ran the
stock image and every write up to the DSP's release was listed
(`capture-stock.py`). [C] for what the stock writes; what the bits mean is [L].

| Register | Stock when its DSP starts | Our OS on the phone |
|---|---|---|
| 0x8b0001a0 bits 21:19 | **clear** (PBL sets them, FUN_0001a7c0 clears them for good) | **set** (entry code, from fpdoom) |
| 0x8b0000a8, APB gates | opens 0xc7ebefda | 0x17e982c0 |
| 0x20500080, AHB gates | opens 0x1ddef | 0x000015e1 |
| 0x205000c0 | 0x60 | not set |

- Bits 21:19 are the main suspect. With them clear, IRAM above 0x4000c000 does
  not hold ARM data (`sdboot.md`), and the stock dumper has an "IRAM Dsp"
  region: those banks may be the DSP's memory [L].
- 0x8b000040 also differs (stock 0x500c0005). Its extra bits belong to the
  stock's clock-by-name functions, whose names are peripheral sources (26M_XTL,
  CLK_104M ...), not a DSP clock [L]. Not changed.
- The whole chip init is not replayed on purpose: its pinmap half is the known
  0x8c landmine, so each difference is taken one at a time.
- Calling the stock's own functions from NOR isn't practical: they use the stock
  kernel in PSRAM (the `blx 0xc0xxx` thunks) and its RAM globals (0x0422e590 ...).

**The SD bounce buffer moved** to 0x40004000 (was 0x4000b000), below the
0x4000c000 limit, so that clearing the bits can't corrupt the card. Bits 21:19
are clear only while the DSP runs; `dsp_stop()` sets them again.

**Own-program self-test (`dsp_selftest()`, Tools → DSP boot).** About 60 words of
hand-assembled TeakLite, nothing from the stock program, go through the same
download. It writes marks, then loops writing a counter and echoing a word the
ARM sets. It runs three ways: IRAM bits set; clear; clear with the stock's gates.

| Result | Meaning |
|---|---|
| No marks in any case | the restart or the DSP's clock is at fault, not the stock program |
| Marks in one case only | that case's state is what the DSP needs |
| `mark0` but not `mark1` | the stock's first register writes (0xbd46..) break it |
| Count moving | it runs; about 266 DSP cycles per count, so count / 20 ms gives its clock |
| `idle` far below `busy` | ARM idle slows or stops the DSP |
| Echo 0x1234 | it reads share memory as well as writing it |

After that the stock program runs twice: first with start-up marks (`stock with
marks` in the report), then unpatched with the full probe.

### The DSP program runs (phone, 2026-11-08)

**Cause: `0x8b0001a0` bits 21:19 were set.** Our entry code sets them (from
fpdoom) and the stock clears them. With them clear the DSP executes; with them
set it does not.

```
own program, IRAM bits set:   marks 0000 0000, echo 0000, count 0 busy 0 idle
own program, IRAM bits clear: marks 5a01 5a02, echo 1234, count 1919 busy 1922 idle
stock with marks: trace 00bf at start, 12bf after records
records 50 of 50, replies 50 (each 000f 0000 0000 0000 0000)
ping: before 0000, after 0001, 1 ms
dump: 64 blocks, 37934 non-zero words, record words 1477 of 1477
idle frames: 20 posted, 0 consumed
```

What this shows:
- **The memory dump is DSP data memory** [C]: every one of the 1477 record words
  is at the address its record names, checked again on a PC from
  `dsp-dump.bin`. Only the program could have put them there and read them out.
- **DSP clock about 26 MHz** [L]: 1919 counts per 20 ms at about 270 cycles each.
  That is right after reset, before the program sets anything.
- **ARM idle doesn't slow the DSP** [C]: the count is the same busy and idle.
- **The stock's clock gates are not needed** [C]: the own program and the 50
  records work without them (a Radio status run in the same log took the records
  with the gates shut). `0x8b000040` was left alone too. So the gate test was
  removed.
- **The moved SD buffer works** on the phone: self-test ok, the dump was saved.
- **Trace:** 0x00bf is points 0–5 and 7: reset, main, both inits, the handlers, and
  the wait at 0x1907. 0x12bf adds 9 (command handler) and 12 (vector 0x16). Point
  8, the wait ending, never came, which is why no frames were taken.
- Trace reads 0 after the dump; why is not known. It doesn't affect the above.

**What ends the wait: the start command** [C]
- A command is word 0 = group, word 1 = type, word 2 = parameter. This corrects
  the note above. Handler 0x9961 takes group 0.
- Type 0 is a record. **Type 0x10 sets 0x1063 = 1, sets 0xbf41 bit 9 and enables
  DSP interrupt 9.** Then 0x1907 returns and the rest of main runs.
- The stock's list (NOR 0xc800c, sent by FUN_0004f580) is **0, 0x10, 1, 3, 4 … 15,
  0x10**: the records, the start, the parameter blocks, the start again. A second
  list at +0x44 repeats 1, 3 … 15, 0x10.

**How the RF parameters reach the DSP** [C for the mechanism; what each block
holds is open]
- FUN_000606ea is a switch on the type. Each type copies a block to share memory
  and sends `{0, type, byte, 0, 0}`, the byte coming from 0x0422fb75.
- One block is 1012 bytes (0x3f4), the exact length of NV item 1, from FUN_0004e9e4.
- Others go to +0x6f6 or +0xcf6 with sizes 0x25c, 0x7e, 0x46, 0x96, 0x78, 0x32.
- Types 4–7 are conditional on flags. FUN_00060650 is type 0, the records.

**Start command on the phone (2026-11-08):** taken. Trace 0x13bf adds point 8, the
wait ended; the dump shows 0x1063 = 1, 0xbf43 = 0x2016, 0xbf45 = 0x40e8, so main went
on through its interrupt set-up (0x4b3–0x4bb). But still **0 of 20 idle frames were
taken**, and vectors 0x6 and 0xe (points 10, 11) never fired in 400 ms; only the
doorbell vector 0x16 did. So its frame interrupt isn't running yet.

**The whole parameter list, decoded** (jump table at FUN_000606ea) [C]

Type 1 is all of NV item 1. The others are in NV item 2, behind its 2-byte version
word (FUN_0004e9ee). Offsets below count from after that word; they fit end to end.
Each goes to +0x6f6, then command `{0, type, byte, 0, 0}`:

| Type | Offset | Bytes | Sent if |
|---|---|---|---|
| 3 | 0x0000 | 126 | always |
| 4 | 0x007e | 792 | NV item 3 byte 42 |
| 12 | 0x0396 | 640 | always |
| 5 | 0x0616 | 792 | NV item 3 byte 44 |
| 13 | 0x092e | 640 | always |
| 6 | 0x0bae | 792 | NV item 3 byte 45 |
| 14 | 0x0ec6 | 640 | always |
| 7 | 0x1146 | 792 | NV item 3 byte 46 |
| 15 | 0x145e | 640 | always |
| 8, 9, 10, 11 | 0x300, 0x898, 0xe30, 0x13c8 | 70, 150, 120, 50 | always; they lie inside blocks above |

- Type 2 (604 bytes to +0xcf6) is not in the start-up list.
- On this phone all four flags are 1.
- `byte` is 0x0422fb75. Its value at this point isn't known; 0 is sent [G].
- `l1_start()` now sends the stock's list in order, read at run time from this
  phone's fixed NV (0x680000). The stock reads NV through a helper that could return
  another copy (the EFS area); whether the two differ is unchecked [G].

**Wake commands** [C for the bytes, L for meaning]
- L1Main_ActiveDspByCmds (FUN_0002f11a) sends `{2, 0xff}`, `{0x1b}`, waits for L1
  event flag 2, then `{0x2f}`.
- Command 2's handler (0x9a52) keeps a flag per bit of its parameter, at 0x1bf6–9.
  Command 0x1b (0xa13b) calls through 0x116d and compares 0xfcce with 0x530c / 0x5310,
  which look like RF chip ids.
- `dsp_probe()` tries these only if idle frames aren't taken, then looks again.
- From the stock's start-up: the default gain hint at 0x0422f030 is NV item 1's byte
  at +0x58, halved (FUN_0004e8f4). The note on the power-measurement instruction above
  calls this unknown.

**Parameter list on the phone (2026-11-08):** all 14 blocks were taken, with the
second start.
- **Frames moved:** the saved share memory shows the ring at in 20, out 20. The
  report's "0 consumed" was a bug in the probe: it took its baseline after posting,
  so frames taken before that were not counted. When and how fast they went is not
  known, and they may have been flushed rather than run. The next run measures it,
  with the ring kept topped up.
- **Acks now carry data:** `000f <type> 530c 1345 f001`. Word 1 is the type answered.
  0x530c is also in DSP memory at 0xfcce, which 0xa13b compares with 0x530c / 0x5310,
  so it's probably an RF chip id [L]. 0x1345 and 0xf001 are unknown [G]. Before the
  parameters, all these words were 0.
- **Records:** 1461 of 1477 record words still match, against 1477 of 1477 before. The
  program now changes some of its own data, which is expected of a running one [L].
- **Interrupts:** 0xbf45 is 0x42e8 (bit 9 enabled), and 0xbf40 reads 0x200 at dump
  time, so its interrupt 9 is pending [C]. The stub's run shows no vector 0x6 or 0xe
  by the end of the frame phase, so no frame interrupt has been seen.
- **One reply isn't an ack:** word 0 = 0, all words zero. Its meaning is unknown [G].
- **Wake commands:** all three taken. 0x1bf6–9 and 0xebe2 read 0x32.

**Measured on the phone (2026-11-08):** 0 frames taken in 400 ms, then 20 within
the 40 ms after `{2, 0xff}`, then none, while 21 more sat in the ring. 20 frames in
under 40 ms is faster than GSM timing (92 ms), so **command 2 flushes the ring; the
frame clock was not running** [L]. The wake sequence was the wrong idea and is gone.

**What starts the frame clock** [C] (0x5a874, also 0x2ea1a)
1. Frame number 0.
2. Four idle frames (FUN_0004fb10).
3. FUN_0005f208(2): flag 0x8200 on the instruction 2 slots back from the last
   written. Only one at a time: the first driver flagged many.
4. Command `{1, 0}`. Its handler (0x9a20) sets bit 2 of 0xff72. `{1, 1}` is what the
   stock sends elsewhere (it sets 0x193 instead).

`l1_run()` does this. `l1_start()` does not call it, so that a caller has its first
frames ready. Tools → Radio status does not call `l1_run()` yet.

### The frame clock runs (phone, 2026-11-08)

```
run command taken; idle frames: 111 posted, 90 taken in 400 ms, 4617 us each (GSM: 4615)
replies: 0004 0041 0000 0000 <frame number>
```

- **The DSP runs GSM frame timing on the phone** [C]: 90 instructions taken at
  4617 µs each. The time comes from a 1 ms tick over the whole run, so that is
  good to about 0.3%, and it can't be told from 4615. Only idle frames were posted.
- **0xff72 reads 4** in the dump, the bit command `{1, 0}` sets [C].
- **Replies report frames** [C for the words]: word 0 low byte = 4, word 1 = 0x41,
  word 4 = a frame number, about 5 apart, following our ask-more flags. Word 1 bit 0
  means "more frames needed" (above); bit 6 is unknown [G].
- Points 10 and 11 (vectors 0x6, 0xe) still don't show in the trace, so frames do not
  need them; point 12 (vector 0x16) does fire [C]. Which interrupt carries the frame
  tick is not settled [G].

**DSP assert 3: the response ring was full** [C for the site, L for the cause]
- That run also got message `ffff0003`. FUN_0003ab0a treats code 0xffff as "DSP Assert
  ID", and the stock would stop on it.
- The DSP's response sender (0x171d) compares the next slot with "out" at 0x1788; if
  equal it calls the assert function with 3 (0x178a, pointer 0x11cd = 0x1810).
- The driver only drained the ring when polling, and it stops polling while it saves a
  file, so the ring probably filled then. It is drained in the interrupt now, as the
  stock does (FUN_0004ee54). The report says which phase the first message came in.

**Confirmed on the phone:** with the ring drained in the interrupt, two runs gave
`messages 0`, with frames at 4617 and 4611 µs (90 and 91 in 400 ms), so draining
the ring did stop it [C]. Replies rose from 87 to 94 in the same test: none lost now.

**Next** [L]
- Layer 1 proper needs the frame clock kept fed by the "more frames" reply rather than
  by a poll loop.
- Still open: `byte` in the parameter commands (0 sent), and which NV copy the stock
  reads.
   - The driver does not set 0x80000028 bit 13 (FIQ). That changes only
     how the interrupt is delivered, not whether the program runs.
   - The stock also calls FUN_0003ab7e before the download; the driver's
     fe0_init() does the same.

No other clock, reset, ADI, remap or arbiter write was found on this
path. The 0x8b00xxxx users are only the helpers listed above.

**Fixed (2026-11-08):** `download()` now uses `(b & 63) << 10` for both
the dl_offset and the source, as the stock does. Not yet run on the phone.

### Tools / Radio status (2026-11-08)

`os/apps/gsmstatus.c`, receive only. It checks:
- each SIM (ready / PIN / none; nothing from the card is kept);
- the DSP boot and `l1_start()`;
- the frame clock (20 idle frames);
- three power sweeps over all 971 arfcn indexes (type 11).

Then it holds the DSP in reset again and writes
`/b310e-os/radio-report.txt` (a summary, plus a CSV of every channel that
answered). QEMU test: `os/tests/test-radio.py [--nor dump]`.

The first phone run, before the download fix, gave: SIM 1 ready at 1.8 V;
the DSP downloaded but took no command, so there was no frame clock and no
scan. Compare a scan with the stock firmware's field-test screen (serving
ARFCN and RXLEV) to settle the band order and the rxlev sense.

**First scan with the DSP running (phone, 2026-11-08)** [C]
- 90 frames at 4617 µs; 486 power-measurement frames, 2913 results of
  2916 asked.
- **Every result was status 3, on all 971 indexes.** The 6-bit field was
  sent as 32, raised by 8 on each status 3 (to 56).

**The 6-bit field is one of three stock settings** [C]
- RAM 0x0422f030 / 31 / 32 hold **10 / 25 / 40**. They were read live from
  stock running in QEMU, and are the same from 5 s to 60 s. The next three
  bytes are 20 / 50 / 80; what they are for is unknown.
- FUN_0005c0b8 starts every channel at f031 (25).
- FUN_0005c3e2 (power sweep) and FUN_0005c8ba (camped):
  - status 2: measure again at f032 (40); status 2 at 40 is an error;
  - status 3: measure again at f030 (10); status 3 at 10 is taken as
    rxlev 0, meaning nothing is there.
- So status 3 is "too weak", and the first scan's 32→56 moved the wrong
  way, outside the stock's values.
- Radio status now uses the stock rule: start at 25, 3 → 10, 2 → 40.
  Results of status 3 at 10 are counted as `floor`.

**Second scan, with the stock rule (phone, 2026-11-08)** [C for the numbers]
- 2913 results: 1713 valid, 969 status 3 (measured again at 10), 231
  status 3 at 10 (rxlev 0), no status 2. 863 indexes gave a valid reading.
- Per band, by the [G] index order: 900 median 26, max 39; 1800 median 15,
  max 38; **1900 median 0, max 15; 850 median 1, max 14**.
- 1900 and 850 are not used here and read at the floor. That settles two
  guesses [L, close to C]:
  - the band order of the arfcn index (`l1_arfcn_index()`) is right;
  - bits 11:4 are rxlev in the GSM sense: bigger is stronger, and
    rxlev = dBm + 110.
- Strongest on 900: ARFCN 122 (39), 54 and 56 (38, valid at the default
  setting 25), then 70, 82, 85 and 87 (34–35). These are the likely GSM
  cells [L].
- 1800: ARFCN 844–883 read a flat 34–36 across about 40 channels. That is
  wideband, most likely an LTE carrier at 1872–1879 MHz, not GSM [G].
- Confirming a GSM cell needs the FCCH/SCH stage (types 9 and 10, above),
  or the stock firmware's field-test screen at the same spot.

### First cell sync (phone, 2026-11-08)

`l1_fcch_post()` / `l1_sch_post()` / `l1_rx_read()` in dsp.c. They build
types 9 and 10 as FUN_0005096e does (before camping):
- word 1 |= 0x10; word 3 = window (26); word 4 = 0x00ff;
- word 6 = index | setting << 10;
- word 10 = cell << 10 | 0x3ff, and for type 10 bits 9:8 = 1 over a low
  byte of 0xff (FUN_0006188a returns 1 with 0x0422f036 = 2).

The frame number of a type 9 is its window's last frame. The rx ring is read
as FUN_0005f692 does; data from +0x12 is swapped, so each halfword is high
byte first.

The test (Radio status, "syncing"): for the 3 strongest 900 channels, one
type 9 over 26 frames, then a type 10 in each of the next 51 frames.

**Result** [C]
- **ARFCN 56: one good SCH** (rx type 1, bfi 0), raw data `69 80 45 af`.
- That decodes to **BSIC 26 (NCC 3, BCC 2)**, T1 768, T2 17, T3' 3, so
  **FN 1019113**. The decode is consistent: FN mod 26 = 17 = T2, and
  FN mod 51 = 31, an SCH frame.
- So the OsmocomBB bit order for SCH data is right, and the SCH arrives
  after a type 9 without any timing command from the ARM: the DSP aligns
  itself after the FCCH [L].
- Header of that block: `0001 0000 0271 0000 1a03 0000 0000 0000 00ff`.
  - hdr[2] = 0x271 is our own frame count (625), not the cell's.
  - hdr[4] = 0x1a03: its high byte is 26, the BSIC [G].
- ARFCN 122 and 73 gave no SCH. 122 reads strong, so it may be a non-GSM
  carrier, or it needs a second try [G].
- **153 DSP messages during sync = 3 x 51, one per type 10.** The DSP kept
  running. The codes were not saved; log `msg_first` / `msg_last` next
  time [G: a per-SCH report].
- Only 1 rx block in the whole step: a type 9 itself returns nothing on
  the rx ring [L].

**Next:** read BCCH (type 2 bursts, tag 7 or 8) on ARFCN 56 at the SCH-
derived frame offsets (BCCH is at FN mod 51 = 2..5). That gives System
Information: MCC/MNC, LAC and cell id.

### First BCCH tries (phone, 2026-11-08)

- **Sync repeats** [C]: ARFCN 54 gives BSIC 36 (NCC 4, BCC 4), ARFCN 120
  BSIC 30 (NCC 3, BCC 6), each with a consistent FN in two runs.
- **DSP messages:** code 5 (param 0x258) comes once per type 10 and is not
  an assert. FUN_0003ab0a only stops on code 0xffff (assert) [C].
- **Rx types** [C]: 7 goes to the BCCH/CCCH handler (FUN_0004d5b4). It
  converts our frame count to the cell's with the per-cell offset
  (FUN_000484e4). 8 goes to the paging handler (FUN_0004dad0).
- **Type 2** (`l1_burst_post`, as FUN_000504f8 builds it): tag 7 gave rx
  type 7, tag 9 gave rx type 8, tag 8 gave nothing [L].
- The type-7 block came back exactly at cell FN mod 51 = 5, the block's last
  burst, so the frame offset from the SCH is right [C].
- **Both blocks had bfi 1** (CRC failed). This happened with the BCC in
  words 3 and 4, and with it in word 3 only.
- **Then the DSP asserted: message `ffff1900`.** It stopped taking frames
  749 ms into the step.

**Words 3 and 4 are the timeslot and the TSC** [L] (stock, read again)
- FUN_000504f8 starts from FUN_0004f5fc: words 0-5 zero, words 6-10
  0xffff. With no monitor cells, FUN_00050074 changes nothing. So words 0-2
  and 5-10 of our type 2 already match the stock (for word 10, if
  FUN_0006188a returns 1, the non-sawless case).
- Word 3 bits 10:8 come from a per-card byte (0x4283ee0 + card · 0x16c).
  Word 4 bits 11:8 come from the serving cell's byte +0x20 & 7, the low 3
  bits of the BSIC: the BCC.
- The CBCH tables (FUN_00050f6a / FUN_0005138e) fill the same two fields
  from a channel description: +5 (3 bits) into word 3 and +6 (4 bits) into
  word 4. TN and TSC fit, and CBCH can sit on timeslots 0-3.
- The variants ran in order: BCC in both words, word 3 only, word 4 only.
  The two bad blocks were the two that put 4 in word 3, timeslot 4: the
  wrong slot, so a CRC failure is expected. The third block (timeslot 0,
  TSC 4, the stock layout) was posted but did not come back before the
  assert.
- Run with timeslot 0 and TSC = BCC (ARFCN 120, BSIC 30): the same
  pattern, 2 blocks with bfi 1, then `ffff1900` at the third. So the
  timeslot was not the cause of the assert [C].

**Assert 0x1900: the BEP report ring was full** [C for the site]
- Disassembled with teakra (a small front end around its
  `Disassembler::Do`, built with zig c++). Each assert site loads the
  routine from DSP 0x11cd, the id into a0, then jumps.
- The function at DSP 0xfacc adds one 2-word report to the ring at DSP
  0xafd0 (ARM +0xfa0, 7 slots). Write index 0xafde (+0xfbc), read index
  0xafdf (+0xfbe). The first word is bits 15:14 = [0x0af1] & 3, bits 13:0
  = [0x0062]; the second is the caller's argument.
- When the ring is full it stores the report in an overflow list (DSP
  0xefec, count at 0x0af7), and asserts 0x1900 once the count passes 5
  (0xfbcf). Sites 0x1901 and 0x1903 in the same function check the flush
  of that list.
- Our driver never moved +0xfbe, so the DSP stopped after 12 reports. It
  is now drained in the interrupt, as the response ring is. The report
  line `bcch: BEP reports` gives the count and the last entry.

**With the ring drained (phone, ARFCN 120)** [C]
- No assert: 9 blocks posted, 9 received in 2 s, 36 BEP reports (one per
  burst). Tag 7 came back as rx type 7, tag 8 as type 8. All bfi 1.

**The SCH frame offset was wrong: header +0x08** [C from two blocks]
- FUN_000461e4 (calibration) takes cell FN offset = SCH FN − reported FN
  + (hi − lo of +0x08). We left that term out.
- An earlier run had two good SCH blocks: local 0x270 with +0x08 = 0x1a07
  (FN 1651605) and local 0x272 with 0 (FN 1651625). The second gives
  offset 1650999, the first 1650981 uncorrected: 18 frames out. A
  single-frame read can only return its own frame, so the second is exact,
  and the first needs hi − lo − 1 = 18 (the stock formula gives 19) [L].
- So every BCCH read so far was about 20 frames off, which straddles
  two CCCH blocks: bfi 1 is expected.
- Now: the sync step corrects by hi − lo − 1 and prefers a block with
  +0x08 = 0. The tag-7 multiframes try offsets 0, −1 and +1 in turn.

**Corrected offset on the phone: still bfi 1** [C]
- ARFCN 56 (corrected, 15 back) and ARFCN 54 (exact) gave the same
  offset, 1926007, so they are one site and the correction holds. Their
  second pair (local 0x2d0 with 0x1a07, 0x2d3 with 0) again needs
  hi − lo − 1.
- Reading ARFCN 56: all 9 blocks bfi 1, at offsets 0, −1 and +1 and with
  tag 8.
- In every run the BCCH cell was synced first, then the other cells were
  searched (ARFCN 120 is another site). If the DSP keeps the slot timing
  and frequency of its last search, the reads were misaligned [G]. The
  BCCH step now syncs its cell again just before reading.
- Phone: the re-sync found an SCH (ARFCN 122, BSIC 37) and the BCCH read
  right after it was still all bfi 1 at every offset. The search order
  was not the cause [C].

**Live experiments: the Radio lab** (`os/modules/radiolab`, API 2.11)
- Apps get layer 1 through `os_api_t.l1` (`l1_api_t` in os/dsp.h): the
  posts, the rings, a raw frame instruction (`l1_fi_post`), raw share
  memory and commands. The shell's `run APP "ARG"` passes an argument.
- With the phone on the USB cable (the shell comes up by itself):
  `python os/tools/b310e-shell.py run build/os/modules/radiolab.app
  --arg "arfcn=56 | tag=8 | w6=0xfc36" --follow`. Each '|' segment is a
  variant (sync, then BCCH reads), keys in the file's header. Results
  go to the log and to /b310e-os/radiolab.txt.

### Live results over USB (Radio lab, 2026-11-08)

All on ARFCN 54 (BSIC 36), with the phone on the USB shell [C]:
- **Type 2 changes that did nothing** (every block bfi 1): w6 gain bits
  0, 10, 25, 40; w10 = 0x0001, 0x01ff, 0x03ff, 0x0101; cell index 1 and 5
  instead of 0; command 3 (gain 30, 50, 60, slot 0x0101) after the sync;
  command 2 `{2, 0x000f}` before the frame clock; offsets 0, -1, +1.
- **24 consecutive blocks:** blocks in the same SI slot (TC) differ in
  about 80 of 184 bits, so they are noise, not a near miss.
- **BEP reports** name the right channel (word 0 = 0x4036, index 54).
  Word 1 changes with the gain settings.
- **Each sync gets one good SCH, and it comes from inside the FCCH
  window** (type 9): for example burst at our frame 76, window 70-95.
- **Type 10 SCH reads on their own return nothing at all**: no good and
  no bad block, posted only at cell FN mod 51 = 1, 11, 21, 31, 41 with
  idle frames between, at offsets -4..+4. The handler (0x8ee6) runs and
  schedules a receive, so the decode fails.
- So no receive works once the FCCH search window has ended: normal
  bursts (type 2) and SCH (type 10) alike. Timing or frequency from the
  search is not kept for later frames [L]. The search shifts the frame
  timer itself (0xdb85 adds to the adjust 0xecb9).
- Some "exact" SCH blocks (+0x08 = 0) are false decodes: BSIC 41 or 58
  with a wild offset on ARFCN 54. The sync must also check the BSIC.

### DSP type-2 receive path (2026-11-08)

Static reading of the DSP program (teakra disassembly). Addresses are DSP
words. Function pointers live in data RAM and come from the parameter
records that follow 5a5a a5a5 in the image (record = length, DSP address,
data). That is how the tables below were resolved.

**Decode and dispatch** [C]
- 0x971b runs once per frame while 0xff72 bit 2 is set. It calls the
  decoder 0x7c4f (reader at 0x7c67), then sets the frame's cell [0xbb] from
  word 10 (0x9743).
- The decoder takes slot `out+1` (0xa800 + 11·slot) and unpacks it into
  0xec1a..0xec52:

| Word | Field | Where it goes |
|---|---|---|
| w0 | bits 4:0 type → 0xec50; bits 15:9 → 0xec1a (flags, 0 for us) | |
| w1 | bits 1:0 burst → 0xec1e → [0xa2]; 3:2 → 0xec1d; 7:4 tag → 0xec1c, 0x1d40; 15:9 FN high → 0xec20 | bit 8 = 0 copies share +0x3f0..+0x3f6 (T1/T2/T3 counters for the cipher unit) |
| w2 | FN low → 0xec1f | |
| w3 | 10:8 timeslot → 0xec25 → [0xc0] → [0x7d] (×625 quarter bits); 12:11 → 0x189; 15:12 → 0xec21 | |
| w4 | 11:8 TSC → 0xec52 → [0xbf] → [0x7c] (TSC table at 0x4578+tsc, 0xd3b7); bit 12 → 0xec2d; 13/14/15 → 0xec29/28/27 | |
| w5 | bits 0..3 = half-step LSB of the gains of w6..w9; 4,5,6,9,12,15 flags; 14:13 → 0xec83 (cipher mode, [L]) | 0 for us; flags pull in share +0x3e4/+0x3e8/+0x3ec/+0x3f8 (dedicated mode) |
| w6 | 9:0 arfcn index → 0xec3c → [0xa5]; 15:10 → **gain 0xec37 = (w6>>10)·2 \| w5.0** → [0xb1] | |
| w7..w9 | monitor channels, same layout (0x3ff = none) | |
| w10 | 15:10 cell → 0xec1b (via 0xf0eb); 9:0 → 0xec40 | |

- Handler = table at data **0x10dd + type** (record): 0 idle 0x867c,
  1 0x8724, **2 0x8771**, 3 0x88b2, 9 0x4e7d, **10 0x8ee6**, 11 0x8f6a.
  Command handlers are at 0x10fd + id (1 = 0x9a20, 2 = 0x9a52,
  3 = 0x1257 → 0x9ab4).

**Type 2 (0x8771)** [C]
- If the cell (w10>>10) is not in the tracked list 0x1bf6..0x1bf9
  (0x4058), it does gain += [0xeafe]. That word has no direct writer, so it
  is probably 0.
- Setup 0x87b0: arfcn → band/channel ([0x1237] = 0x5287) → 0xed75;
  [0x93] = 0 (the block-completion case: CCCH), [0x96] = 4 (normal burst;
  tag 1 gives 8), [0xa2] = burst, [0xa5] = arfcn, [0xb1] = gain,
  0xecba = 2. Then 0x970c builds the receive list (monitor entries with
  arfcn 0x3ff are skipped) and [0x1167] schedules it.
- Tag 9 with burst 0 and 0xec2a = 0x7f, and tag 9 with burst 3, have extra
  paging paths. Tags 7 and 8 have none.
- No check skips demodulation for a normal type 2. The bursts are
  received, and the BEP ring gets one report per burst. A report's first
  word, bits 13:0, is [0x62], the snapshot of that frame's arfcn, so it
  shows which channel each burst was taken on.

**The empty header is normal for CCCH** [C]
- At the end of a block, [0x50] (the frame's [0x93]) indexes the jump
  table at program 0xc3. Case 0 (type 2) posts via [0x116f] = 0x11e1 →
  0x8242, with a0 = b0 = b1 = 0 (0xd02c). The header words come out as:
  - +0x08 = +0x0c = +0x0e = 0;
  - **+0x0a = [0xff0f] << 12**, the frame's tracked-slot mask
    (0x1000 = slot 0);
  - +0x10 = 0, except for rx type 9.
- So bfi 1 with a zero header does not mean the bursts were skipped. SCH
  blocks use another writer ([0x1171] = 0x8407), which fills those words.

**Gain: type 2 runs at the bottom of the gain table** [C for the mechanism]
- [0xb1] → snapshot [0x6e] → 0x6b0b → [0x1231] = 0xb60c. It computes
  idx = gain − [0x0f01] (skipped if 0xec60 ≠ 0), clamps idx to 0..71, and
  looks it up in the record tables 0x0e59 (RF word) and 0x0ea1 (dB·16).
  Index 0 is +46 dB and index 71 is −23 dB, 1 dB per index: **a bigger
  value means less gain.**
- Type 10 (0x8ee6) uses gain = 2·setting − [0x0187], clamped at ≥ 0. With
  setting 25 that is index ≈ 50, about −2 dB.
- Our type 2 has w6 bits 15:10 = 0x3f, so gain = 126, clamped to 71: about
  21 dB less gain than the SCH that worked. This is the likeliest reason
  for bfi 1 [L]. The stock ARM may OR a gain into w6 after
  FUN_000504f8 (as FUN_0005cafe does for type 9); that is unchecked [G].

**Timing: per-cell table and the frame cell** [C for the code, L for the effect]
- 0xeb22 + 4·cell (48 cells; 0x32 = none) holds: +0 valid, +1 timing in
  quarter bits (normalised to −1755..3245 by 0x1275), +2 the arfcn
  (0xeb1e).
  - A good SCH sets it through [0x1158] = 0xd75a, with the cell in 0xeb11
    (type 10's w10>>10). The value is the measured offset + T([0x78]) +
    slot·625 (0xd920-0xd959).
  - [0x123b] = 0xd720 reads it, returning 0 for invalid or 0x32.
- Each frame, 0x74fc compares [0xbb] (this frame's cell, from w10) with
  [0x78] (the last frame's). When they differ it adds T(new) − T(old) to
  the frame-timer adjust 0xecb9. That is the only place a cell's SCH
  timing reaches later receive windows.
- Idle and power frames set the frame cell to 0x32 (0x7cd5), and the FCCH
  search realigns the timer itself (0xdb85).
- With the same cell index for the SCH and the read, the read window ends
  up where the FCCH left it. The SCH's fine offset is cancelled out (the
  −T on leaving the cell is undone by the +T on returning). A stale T from
  an earlier sync of another ARFCN on cell 0 adds to the confusion [L].
- [0x78] and [0xbb] are set to 0x32 only by L1 init 0xbfb2. Only command
  2 reaches it, with word 1 & 0xf = 0xf, which frees all 4 slots; it also
  clears 0xff72 bit 2 and empties the frame ring. Our driver never sends
  it, so both start at 0 (bss). Startup (0x39ae via [0x1163]) does clear
  0xeb22 and set 0x1bf6..9 and 0xebe2 to 0x32.
- Share mirror [G]: 0xcd0 adds the same delta to 0xeb23 + 4c and to
  0xae37 + 4c (ARM +0xc6e + 8·cell).

**Command 3 (0x9ab4)** [C for the fields, L for the meaning]
- It puts a cell into the tracked slots: word 1 = cell; word 2 = gain
  index 0..71 ([0x119b] = 0xb915); word 4 bit 0 = use the current slot;
  word 4 bits 11:8 = a slot mask.
- A tracked cell skips the +[0xeafe] gain offset and gets its own slot
  context (0xff0e/0xff0f).
- It looks like dedicated-mode or serving-cell AGC. Idle BCCH should not
  need it.

**Tests, best first** (one change each, ARFCN 56, tag 7)
1. [L] w6 = setting << 10 | index, with setting 25 (0x6400 | idx), then
   10 and 40. Keep everything else.
2. [L] Use a different cell index per synced ARFCN: 1, 2, … (never 0 and
   never 0x32), the same in its type 9, type 10 and type 2. Post no other
   cell between them.
3. [L] Send command 2 `{2, 0x000f, 0, 0, 0}` before l1_run's command 1
   (the stock wake also sends id 2). Do it with the ring empty, then
   restart the frames with command 1.
4. [G] Command 3 `{3, cell, 50, 0, 0x0101}` after the SCH, then type 2
   with w6 gain 25.
5. Check: log the BEP reports for each variant. Word 0 bits 13:0 must
   equal the arfcn index; word 1 is a quality value, min-merged at 0x1131
   [G].

### Handoff for the next agent (2026-11-08)

**Where GSM stands.** Each step is verified on the phone unless marked.

| Step | State |
|---|---|
| SIM read (ready / PIN / none) | works |
| DSP boot, parameter records, frame clock (4615 µs) | works (0867fbc) |
| Power scan, type 11, stock gain rule 25 → 10 / 40 | works: real levels, band order and rxlev sense settled |
| FCCH (type 9) then SCH (type 10) | works: one good SCH per search, BSIC and FN decode |
| BCCH (type 2) | **blocked**: every block bfi 1. The likeliest fix is the w6 gain ("Tests, best first" above) |
| System Information, idle mode, paging | not started |
| Layers 2/3 (OsmocomBB), calls, SMS, transmit | not started; transmit only on a shielded test network (os-design.md) |

**Code map**
- `os/drivers/dsp.c` / `os/include/os/dsp.h`: boot, `l1_start`, `l1_run`,
  `l1_poll`, the posts (`l1_idle_post`, `l1_pm_post`, `l1_fcch_post`,
  `l1_sch_post`, `l1_burst_post`), `l1_ask_more`, `l1_pm_read`,
  `l1_rx_read`, and `l1_next_fn` (our own frame count).
- `os/apps/gsmstatus.c` (Tools → Radio status): runs every step above in
  one bounded thread and writes `/b310e-os/radio-report.txt`. The BCCH step
  decodes SI 3 and SI 4 (MCC, MNC, LAC, cell id), so a `network:` line
  appears in the report once a block passes its CRC.
- `os/modules/radiolab` (live over USB, see above): the faster loop for
  one-change experiments.
- QEMU: `os/tests/test-radio.py --nor <dump>` and `test-dsp.py --nor`.
  QEMU runs no DSP program, so they only check that the plumbing works and
  that nothing is reported that did not happen. The phone is the only
  test of the radio.

**Rules that cost time**
- **Once `l1_run()` starts the frame clock, never let the ring run dry.**
  Keep about 8 instructions queued and set `l1_ask_more(3)` after each
  batch. Count frames taken as posted minus queued (`gsmstatus.c`
  `consumed()`).
- **Drain every DSP-written ring.** That means responses, BEP reports and
  rx blocks. A full ring makes the DSP assert (ids 3, 0x1900).
- The 6-bit gain field only takes the stock's three settings: 25 to start,
  10 after status 3, 40 after status 2. Status 3 at 10 means nothing is
  there.
- Allocate per-channel tables before the scan and clear them at once. A
  report printed from an unset table looked like measurements.
- Tools → DSP boot and Radio status share the DSP. Radio status refuses
  to start while DSP boot runs, and the QEMU test waits for it.
- When the phase enum in `gsmstatus.c` changes, update `R_DONE` in
  `test-radio.py`.
- The card loop: copy only `payloads/b310e-os.bin` to `E:\payloads\` and
  `cmp` it. Apps live in category folders on the card, so never copy them
  flat. Check the `boot: ... built` line in `log.txt` before trusting a
  report: the phone may not have booted the new file.
- Commits: git has no identity in the agent shell. Pass the user's identity
  per command (`-c user.name=... -c user.email=...`, as in the log); never
  write it to the config.

## Milestones

1. **M0:** confirm the carrier still runs 2G, and that stock calls and
   texts on this phone.
2. **M1:**
   - Load `user_raw` as a second Ghidra program at 0x08000000.
   - Map the task table, the MN API entry points and the AT/serial input
     (`SIO_RECV_NEW_AT`).
   - Trace the MMI → MN flow for PS power-on, PIN, SMS send, dial, answer,
     hang-up and incoming indications.
3. **M2:** boot stock from SD with a patch table (4 KB MMU aliases, no NOR
   writes). First, fix the known LCD `MEM_REMAP` problem of RAM-booted
   stock.
4. **M3:**
   - A hook thread created at stock init and running from spare PSRAM.
   - Prove it with an SD-card log written through stock's filesystem.
5. **M4:** drive the stack: inject `AT+CSQ` / `AT+CREG?` (QEMU), then
   `AT+CMGS` / `ATD` on the phone, or call the MN APIs directly.
6. **M5:** our own UI, suppressing stock windows while keeping its
   services alive.
7. **M6 (optional):** hosted apps, if measured free RAM allows.

## Patching constraints

- Patches cannot go inside paged code until there is an encoder for the
  SPRD LZMA variant. Hook the uncompressed kernel, or the pager after
  decompression.
- **Delivery:**
  - Preferably RAM-boot stock from SD.
  - Patching the NOR needs USB download mode working and is a recovery
    risk on this phone.
- **UART is impractical for AT access:** pin `0x8c0002a4` is the known
  pinmux landmine.

## Known unknowns

- `user_raw`'s exact base and page size, and the base of substream 1.
- Free PSRAM above stock's high-water mark (about 0x042e0c60). Measure it
  in QEMU after a long boot.
- Which serial port the AT interpreter listens on by default, and whether
  stock's USB CDC exposes it.
- Whether the MMI can be cut without breaking PS power-on, SIM or call
  audio.
- The SIM controller base (`sim_drv.c` reaches it through RAM pointers;
  see the SIM work).
- The transceiver/PA identity. The firmware's NV names an "SC6500 Eider
  7182" configuration; the service manual shows an RF7198-class part.
- Whether the local network still has 2G.
