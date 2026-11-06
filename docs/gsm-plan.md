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
Next: the power-monitor frame instruction (`rxlevmon.c`), the result
format, and the RF parameter download (`l1dsp_param_ptr`).

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
