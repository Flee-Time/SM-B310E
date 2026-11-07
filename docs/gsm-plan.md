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

**Not yet seen on the phone**
- No DSP result of any kind has been observed on hardware: the 2026-08
  diagnostic only showed DSP activity (2 of 10 share words changed).
- The OS driver boots the DSP as verified (QEMU: all 66 blocks through
  the model).
- The record stream, the frame-instruction rings and POWERMONQ are
  written from this analysis and need a phone run.

**Licensing:** never ship stock bytes. As the FM driver does with its init
tables, read or patch them from the owner's own NOR or dump.

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
