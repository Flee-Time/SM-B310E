# B310E DSP + Audio Route — complete signal-path map

**Purpose:** the ONE authoritative map of how sound gets produced on the SM-B310E
(SC6530C) — from the DSP firmware in NOR to the speaker — and what the custom
OS (`os-dsp-boot.bin`) must do to reproduce it. Every register and sequence is
**Ghidra-verified against `dump_firmware.bin`** (import base 0x0, file offset ==
address) or read from the vendor SDK (leaked internal SDK — **never committed**;
read-only reference, semantics only). Deep-dive: `docs/audio-dsp-protocol.md`.


## 1. The hardware blocks (topology)

```
                    ┌──────────────────── SC6530C ────────────────────┐
 [ARM926EJ-S @208 MHz]          [TeakLite DSP core (SC6600M3)]       │
      │  0x10000000 ── share-mem (DSP_DL_CTL_T + data) ──┐           │
      │                         boot vector 0xA800 ◄──────┘          │
      │  0x8B000140/160 DSP IRQ mailbox ◄─────────────────►          │
      │  0x82003000 VBDAL / 0x82003004 VBDAR  (16-bit DA data port)  │
      │  0x82003010 VBBUFFERSIZE (DA [15:8], AD [7:0], 160-1=0x9F)   │
      │  0x82003018 VBDABUFFDTA: b9 VBRAMSW_NUM · b10 VBRAMSW_EN     │
      │        b11/12 AD-DMA-EN · b13/14 DA-DMA-EN · b15 VBENABLE    │
      │  0x8200303C VBIISSEL · 0x82003040 DAPATHCTL · 0x82003048 HPF │
      │            └──────────► 8 kHz PCM → codec DAC (IIS)          │
      ▼                                                              │
 [DP DAC 0x8A002000]  (0x00 top_ctl DAC_EN_L/R · 0x0C dac_ctl:       │
      │    bits 0-3 DAC_FS_MODE, b14 DAC_EN, b15 DAC_MUTE_EN)        │
      ▼                                                              │
 [ANA codec @0x82001280]  (audif_enb·ccr·dacr·daocr2·dcr1·dcgr1-3·    │
      │    power 0x820012C0, PA-mute 0x82001290, LDO rails 0x82001164)│
      ▼                                                              │
 [PA: 0x82001450/54, 0x82001440/44, 0x8B000060/64 b21, 0x8B0000A0/A4 b28]
      ▼                                                              │
 [NCP2817BFC amp — AMP_EN = product GPIO (18/39/0 candidates)]        │
      ▼                                                              │
 [EAR_L/R → speaker (SPK_P/N) + 3.5 mm jack + earpiece EAR_P/N]      │
 └────────────────────────────────────────────────────────────────────┘
```

**Ownership mux** `0x8B0001C4` (APB_PERI_CTL0): b9 `CLK_AUD_ARM_CTRL` + b10
`AUD_CTRL_SEL` + b2 `ARM_VB_ACC` = **0x604** = "ARM owns codec + VBC data path"
— what the custom OS must assert (the stock keepalive asserts b2).


## 2. DSP boot (the verified sequence — works on HW)

The DSP program ships **inside the PS XIP image** (dump 0xCC874, LZMA-compressed
`dsp_pdata`; decompressed to 0x40000; seg0 = 66861 words @ +0x28). The SC6530C
DSP boot uses **strap + BOOT_EN**, NOT the SC6531EFM APB_RST0 (0x8B001068/
0x2068 — 0 literal hits; the SC6530C reset SET/CLR pair is b16 of
**0x8B000060/0x64** via `FUN_00067fc8`).

**Registers:** `0x8B000060`/`0x8B000064` = DSP reset hold/release b16 (0x10000);
`0x8B0001C0` = strap [7:3] (USER_RST_MODE=4 → EXT_PROG_MODE=0); `0x8B0001A0` =
boot vector [15:0] + ARM_BOOT_EN b16 (0xA800 / +0x10000); `0x8B000140` =
INT_STS0 DSP_IRQ b2; `0x8B000160` = INT_SET_CLR0 DSP_IRQ_CLR b2 / DSP_FRQ_CLR
b3 (write 4\|8) / MCU_FRQ_SET b1; `0x80000008` = INT_IRQ_EN line 13 (|= 0x2000);
**`0x10000000`** = share-mem base.

**Share-mem control struct (u16 halfwords; unit = HALFWORDS):**
```
+0 arm_control_status READY=1·DATA_READY=2·START_COPY=4·BOOT_DONE=8
+2 dsp_control_status READY=1·READY_TO_COPY=2·COPY_DONE=4·RUN=8
+4 dl_offset (u16) · +6 dl_block_size (words) · +8 DATA area (16-BIT stores)
```

**Sequence (dump FUN_0003aa76 + FUN_0003ad16 + FUN_0003a92a, byte-verified):**
1. `[0x8B000060] = 0x10000` — hold DSP reset; clear the 20-byte control block;
   `arm_ctl = 1` (ARM_READY).
2. `[0x8B0001C0] = (v & ~0xF8) | 0x20` — strap USER_RST_MODE (4);
   `[0x8B0001A0] = (v & 0xFFFF0000) | 0xA800` — boot vector; `|= 0x10000` —
   ARM_BOOT_EN (DSP boot ROM starts).
3. `[0x8B000064] = 0x10000` — release reset; wait `dsp_ctl & READY` (keepalive
   re-asserts ARM_READY every 10k polls).
4. Per 1 KB-word block (`dl_offset = (block & 0x3F) << 10`, `size = 0x400`):
   write `dl_offset`/`dl_block_size`; **copy `size*2` bytes as 16-bit stores** to
   `share+8`; `arm_ctl = READY|DATA_READY` (3) → wait `dsp_ctl & READY_TO_COPY`
   (2); `arm_ctl &= ~DATA_READY`; `arm_ctl |= START_COPY` (4) → wait `dsp_ctl &
   COPY_DONE` (4); wait `INT_STS0 & 0x4` (DSP_IRQ); `[0x8B000160] = 4|8`;
   `arm_ctl &= ~START_COPY`.
5. Last block: `arm_ctl = BOOT_DONE` (8); then strap EXT_PROG_MODE
   (`[0x8B0001C0] &= ~0xF8`), `[0x8B0001A0] &= ~0x10000`, hold+release reset →
   DSP runs. **HW-verified (2026-08-27)**: stages 04 (DSP_READY) + 05 (all 66
   blocks) PASS; post-boot the DSP **is running** (writes its working state +
   message area, DLTA 02/10) but sends no boot-time 0x1111/0x2222 messages
   (those are command-response, not init).


## 3. The audio route after the DSP is up

Sound = **ARM streams 16-bit samples into VBDAL/VBDAR, the VBC converts them to
the codec DAC, the codec analog path amplifies them**. The stock boots the DSP
(§2) and the ARM owns the codec + VBC (0x604); there is **no per-tone "start
audio" DSP command** (§6) and `is_dsp_ctl==0` = ARM-controlled codec.

### The ordered activation chain (what os-dsp-boot does — all dump/SDK-verified)
1. **Subsystem power (P2-P6, emulator capture)**: AHB gate `0x205000C0 |= 0x60`;
   23-bit APB ladder on `0x8B0000A0`; **ANA clock block `0x820010E0/0x10E4/0x1040`**
   (15 single-bit writes — D1, the #1 former gap); 14-bit AHB ladder on
   `0x20500060`; ANA pads `0x82001850..0x1884` (6 writes; the 0x8c pinmap half
   stays BANNED on HW).
2. **Codec power**: `0x820012C0` bits 0x02/08/80/40/20/10 **+ companion LDO
   rails on `0x82001164`** (the no-click root cause: ids 31→0x10, 30→0x20,
   29→0x40, 2→0x80, 28→0x100 — NOT 0x8A GPIOs).
3. **DAC path (device-1 ON, stock 0x06A14C/0x06A2A4)**: IF1 `0x820011A0 |= 0x10,
   |= 0x8000` (IF0 `0x82001180` is **GATED — stalls the AHB on this silicon**);
   GRP0/GRP1 `0x820012A0/0xA4` mux; GRP08 `0x820012A0 |= 8`; LDO `0x82001014 b14`;
   mux `0x82001288 |= 0x400 &= ~2`, `0x820012A4 |= 0x4000`, `0x8200116C |= 0x400`.
4. **DAC cores**: ccr `0x8200132C |= 0x18` → audif_enb `0x82001280 |= 0x05` →
   dacr `0x820012F4 |= 0xC0` → routing `0x820012FC |= 0x84`, `0x82001304 |= 0xD0`
   → gains `0x82001314/18/1C = 0x44/0x40/0x44` (0xF = mute).
5. **DP DAC `0x8A002000`** — the **2026-08-28 fixes**: `dac_ctl` bits 0-3
   **DAC_FS_MODE = 10 (0xA) for 8 kHz** (0x8123E table; the old `fs=9` = 9600 Hz
   = FS mismatch = silence); **bit 14 (0x4000) = DP DAC ENABLE** (the stock's
   DAC-on `FUN_0008131e(1)` always sets it; the custom OS never did); bit 15
   clear (DAC_MUTE_EN off); `top_ctl |= 0x05` (DAC_EN_L/R).
6. **PA**: `0x8B000060/64 |= 0x200000` (b21), `0x82001450/54 = 1`, `0x8B0000A0/A4
   |= 0x10000000` (b28), `0x82001440/44 = 4`; unmute: `0x8B0001C4 |= 0x604`
   (full ARM ownership), `0x82001290 bit0 clear`.
7. **VBC**: power pulse `0x8B000060 = 0x40000` → delay(10) → `0x8B000064 =
   0x40000` (b18); `0x8B0001C4 |= 0x100` (ARM_VB_ANAON); clear both DA buffers
   through the RAM switch (VBRAMSW_EN b10 on, VBRAMSW_NUM b9 toggle, 160 zeros
   to VBDAL/VBDAR per buffer — **VBENABLE must be LOW during buffer access**);
   `0x82003010 = 0x9F9F` (160-1 for DA+AD).
8. **Feed + enable**: samples to VBDAL/VBDAR at the 8 kHz rate; VBENABLE b15
   last.

**VBC feed-mode question:** the SDK DAI (`audio_dai_vbc.c`) plays **exclusively
via the system DMA** (`_VBC_DMA_DAC_Send` → DMA_HAL + `VBDALDMA_EN/VBDARDMA_EN`
b13/b14); the CPU `__vbc_write` has zero DAI callers. The SC6530C DMA base is
NOT statically pinnable (the DMA HAL is PSRAM-resident), but the VBC PHY *does*
accept CPU writes. Three feed modes are tested: **DIRECT** (DMA-EN off, VBRAMSW
off, raw VBDAL/VBDAR), **VRAMSW** (VBENABLE low → fill via VBRAMSW → VBENABLE
high), **DMAEN** (b13/b14 set + direct writes).


## 4. Stock sound activation — the answer in one table

The missing piece after DSP load + power + clocks was the **VBC data-path
activation**. The stock writes, Ghidra-verified:

| # | What the stock OS writes | Register | Value | Our `audio_beep` |
|---|---|---|---|---|
| 1 | **Full ARM audio ownership** | `0x8B0001C4` (APB_PERI_CTL0) | **0x604** = b9 `CLK_AUD_ARM_CTRL` \| b10 `AUD_CTRL_SEL` \| b2 `ARM_VB_ACC` | only 0x200 (b9); beep ORs 0x160 (b5/b6/b8) |
| 2 | **VBC DA DMA-enable** | `0x82003018` (VBDABUFFDTA) | **0x6000** = 1<<14 `VBDALDMA_EN` \| 1<<13 `VBDARDMA_EN` | never set (only VBENABLE 1<<15) — the single most likely "no audio" cause |
| 3 | **VBC device power pulse** | `0x8B000060` then `0x8B000064` | **1<<18** each (set → delay(10) → clear) | never set |
| 4 | **MCU access to the DA ping-pong buffer** | `0x82003018` | b10 `VBRAMSW_EN` (=0x400) around direct VBDAL/VBDAR writes; b9 `VBRAMSW_NUMBER` (=0x200) selects buffer 0/1 | samples written with neither bit |
| 5 | **VBC buffer sizes** | `0x82003010` (VBBUFFERSIZE) | **0x9F9F** = (160-1)<<8 \| (160-1) | low byte only |
| 6 | **DA clock bits** | `0x8B0001C4` | b5 `ARM_VB_DA0ON` \| b6 `ARM_VB_DA1ON` (0x60) + b8 `ARM_VB_ANAON` (0x100) | already set |
| 7 | **VBENABLE last** | `0x82003018` | **1<<15** (0x8000) | set, but alone |

**Bottom line:** add #1 (b2+b10), #2, #3, #4, #5 to `audio_beep` — combined
with the emulator finding **D1** (ANA clock block `0x820010e0`/`0x10e4`/`0x1040`,
§7) this is the complete delta between a powered codec and an audible beep.

## 5. The exact ordered VBC activation sequence (for the hardware session)

Minimal, Ghidra-evidenced, in the stock driver's order. Apply after the codec
power/DAC/PA chain already in `audio_beep` (audio.c:372-377) and the D1 ANA
clock block:

```
MEM4(0x8B0001C4) = MEM4(0x8B0001C4) | 0x604;   /* 1. ownership: b9|b10 codec + b2 VBC */
MEM4(0x8B0001C4) = MEM4(0x8B0001C4) | 0x160;   /* 2. VBC clocks: b8 ANAON | b5|b6 DA0/1ON */
MEM4(0x8B000060) = 0x40000; delay_ms(10);      /* 3. VBC power pulse 1<<18 set  */
MEM4(0x8B000064) = 0x40000;                    /*    ...clear */
MEM4(0x82003018) = MEM4(0x82003018) | 0x400;   /* 4. __vbc_clear_da_buffer: VBRAMSW_EN b10 */
MEM4(0x82003010) = 0x9F00;                     /*    set_buffer_size(0xa0, 0) */
MEM4(0x82003018) = MEM4(0x82003018) | 0x200;   /*    VBRAMSW_NUMBER b9 — buffer 1 */
for (i = 0; i < 160; i++) { MEM4(0x82003000) = 0; MEM4(0x82003004) = 0; }
MEM4(0x82003018) = MEM4(0x82003018) & ~0x200;  /*    buffer 0 */
for (i = 0; i < 160; i++) { MEM4(0x82003000) = 0; MEM4(0x82003004) = 0; }
MEM4(0x82003010) = 0x9F9F;                     /* 5. set_buffer_size(0xa0, 0xa0) */
MEM4(0x8B0001C4) = MEM4(0x8B0001C4) | 0x60;   /* 6. __vbc_da_enable(1, ALL): b5|b6 */
MEM4(0x82003018) = MEM4(0x82003018) | 0x6000; /*    __vbc_dma_da_chn_en(1): 1<<14|1<<13 */
for (i = 0; i < 160; i++) {                    /* 7. feed 440 Hz square wave */
    int16_t s = ((i / 9) & 1) ? 0x1800 : -0x1800;
    MEM4(0x82003000) = (uint32_t)(uint16_t)s;  /*    VBDAL */
    MEM4(0x82003004) = (uint32_t)(uint16_t)s;  /*    VBDAR */
}
MEM4(0x82003018) = MEM4(0x82003018) | 0x8000; /* 8. VBENABLE — start playback (LAST) */
/* ... play ... */
MEM4(0x82003018) = MEM4(0x82003018) & ~0x8000; /* 9. stop: VBENABLE off */
MEM4(0x82003018) = MEM4(0x82003018) & ~0x400;  /*    VBRAMSW_EN off */
MEM4(0x8B0001C4) = MEM4(0x8B0001C4) & ~0x160;  /*    VBC clocks off */
MEM4(0x8B0001C4) = MEM4(0x8B0001C4) & ~0x604;  /*    ownership back */
```

The stock driver instead streams via the **DMA controller** (halfword, dest
fixed = VBDAL/VBDAR, src INCR4, block = 2×160 bytes) — keep the CPU loop for
the first HW pass. **Failure triage:** (a) D1 ANA clock block (§7) first;
(b) PA/`AMP_EN` GPIO (candidates 18/39); (c) the 0x82001288 DAC_DATA_TX_ADDR
probe already in `audio_beep`; (d) verify `VBDABUFFDTA` reads back 0x7000
(0x6000|0x1000) after step 6+8.


## 6. No audio-start DSP command — and the tone path

There is **no DSP command that starts the audio data path** on the ARM path.
Three pieces of evidence:
1. **The only ARM→DSP mailbox traffic is a periodic keepalive.** Dump `0x1A516`
   (literal `[0x1A758] = 0x8B000140`, so `[0x8B000160]`):
   ```
   [sharemem+4] = 0        ; ack word  (sharemem = *(0x0422E598), runtime base)
   [sharemem+0] = 3        ; command word = 3
   [0x8B000160] |= 2       ; pulse DSP IRQ (MCU_FRQ_SET b1)
   delay ~30 ticks
   wait [sharemem+4] == 1  ; DSP ack
   ```
   Command **3** = the ping/keepalive the emulator captured (`0x8b000160 = 0x2`,
   `0x8b0001c4 = 0x4`, `0x82001a44` reads/writes — driver-cycle rows 1-46): the
   ARM↔DSP health handshake of the old-gen audio DSP driver
   (`0x1a4e0`-`0x1a680`), **not** an audio-start command.
2. **The `audio_dsp_info` tokens are config, not commands.** The walker
   `AUDIONVDSP_GetAudioDspCodecInfoFromRam` @ `0x87764` returns the
   `0x0230010x`-family tokens from the mode-list head `0x0423572C` (NV blocks
   shipped at `0x680000`) — **DSP-codec mode parameters**, consumed once at
   codec setup, never per-tone (audio-dsp-protocol.md §NV).
3. **`is_dsp_ctl == 0`** — the codec is ARM-controlled (audio-dsp-protocol
   §Codec-driver §6); the only ownership flip is `0x8B0001C4` (§4 #1), and the
   VBC keepalive explicitly **sets b2** (`0x1A55E`: `[0x8B0001C4] |= 4`). So the
   samples must come from ARM.

**Tone path:** the SDK's beep is `AUDIO_HAL_Open(AUDIO_HAL_TONE)` where the
**DAI** produces the tone (e.g. the AIF hardware tonegen); on the SC6530C that
tonegen **does not exist** (audio-dsp-protocol §Codec-driver §8). The dump
confirms the replacement: the **VBC DA buffer is the tone source** —
`_VBC_DMA_Start_Chan`/`_VBC_Trigger` (0x0A7D0C/0x0A7806) arm the DA path,
`__vbc_dma_dac_chan_en` (0x0A7844) streams the samples; no frequency table,
tone-enable register, or PWM path exists. For a 440 Hz beep the "tone data" is
the ARM-generated square wave already in `audio_beep` — it just needs the
enable bits of §5.

## 7. Emulator capture — QEMU Wave-6 bring-up (P1-P7) + keepalive

The stock OS (NOR `FUN_00034ed4`) brings up the audio subsystem in seven phases
before any codec register write. Captured from `tools/qemu-b310e/logs/w6/
stock-sound.csv` (231 rows; boot-mode=warm, NOR = `tools/spd_dump/
full-backup.bin`); compared against `drivers/audio.c`.

| Phase | What the stock does | Key registers / values |
|---|---|---|
| **P1** | codec die-ID read via the ADI mailbox (`FUN_0001a33e`) | `0x82000018=0` → read `0x82001000` = 0x0; `0x82000018=4` → read `0x82001004` = **0x40000**; die ID = 0x00040000; the check returns 0 so the config dispatcher always runs |
| **P2** | AHB power gate (`FUN_0001a4e4`) | `0x205000c0 \|= 0x60` |
| **P3** | APB power ladder | 23 SINGLE-BIT absolute writes on `0x8b0000a0` (bits 1,3,4,6,7,8,9,10,11,13,14,15,16,17,19,21,22,23,24,25,26,30,31; accumulated **0xC7EBF3FA**). **bit 28 (0x10000000) is NOT in this ladder** — it belongs to the later PA-type-2 step `FUN_00081562` |
| **P4** | ANA clock/reset/config block | 15 absolute single-bit ADI writes on `0x820010e0`/`0x10e4`/`0x1040`; final `0x10e0=0x100` (accum 0x1FE, bits 1-8), `0x10e4=0x20` (0x3E), `0x1040=0x1` (0x3). The **ANA ARM-clock-enable pair** (`ANA_ARM_CLK_EN0`/`ANA_SOFT_RST0` analogue) — the same bits the EIC path uses. **D1** |
| **P5** | AHB device gates | 14 single-bit writes on `0x20500060` (0x4000, 0x2, 0x400, 0x100, 0x1, 0x4, 0x8, 0x800, 0x10000, 0x8000, 0x20, 0x1000, 0x40, 0x80) |
| **P6** | pinmap replay (`FUN_00035c98`) | 149-pair table @ **0x000c6ef0**: 143 direct `0x8c0000xx` writes (**incl. the BANNED `0x8c0002a4 = 0x231`** row 215 + SDIO pins 0x8c000250-264 = 0x100/0x240) + 6 ANA pad writes `0x82001874=0x18a`, `0x82001850=0x18a`, `0x82001878=0x10a`, `0x8200187c=0x100`, `0x82001884=0x100`, `0x82001880=0x101` |
| **P7** | APB GPIO | `0x8a000000=0`, `0x8a000004=1`, `0x8a000008=1`, `0x8a000018=0` (bank data/DMSK/DIR/IE) |

**The periodic DSP keepalive cycle** (`stock-sound-driver-cycle.csv`, 46 rows,
repeats every ~4484 raw lines): `0x8b0001c4 = 0x4` (**b2 ARM_VB_ACC** — the
ARM-vs-DSP VBC data-path switch), `0x8b000160 = 0x2` (**MCU_IRQ_SET** to the
DSP), ADI read+write `0x82001a44 = 0` (codec RAM-shadow keepalive, role UNKNOWN
— D6), pinmux `0x8c0001f8..208 = 0x188/0x188/0x108/0x188/0x188`, bootready
`0x0425de8c = 1`.


## 8. The 10-divergence diff table (D1-D10)

Stock columns cite stock-sound.csv / stock-sound-driver-cycle.csv rows; our
columns cite drivers/audio.c. Verdicts per the Wave-6 diff.

| # | Register | Stock (value / rows) | Ours (audio.c) | Verdict |
|---|---|---|---|---|
| **D1** | ANA clock block `0x820010e0` / `0x10e4` / `0x1040` | 13-write absolute sequence (rows 8-34, 54) | **never written** (nearest is `audio_dac_on`'s ccr, audio.c:234-236) | **MISSING - the #1 step.** Add at the top of `audio_power_on` (audio.c:103), before the 0x820012C0 power bits, in the captured order |
| **D2** | APB power ladder `0x8b0000a0` | 23 single-bit writes (rows 6, 35-58) | only bit 28 = 0x10000000 (audio.c:308) — that is the LATER PA-type-2 step, not in the captured ladder | **MISSING** - add the 23-bit subsystem APB enable (bits 1,3,4,6,7,8,9,10,11,13,14,15,16,17,19,21,22,23,24,25,26,30,31) |
| **D3** | AHB gates `0x205000c0` + `0x20500060` | 0x205000c0 |= 0x60 (row 5) + 14 writes (rows 59-72) | **never written** (sdio.c:186 writes 0x20500060=0x400 for SDIO only) | **MISSING** - add `0x205000c0 |= 0x60` + the 14-bit 0x20500060 ladder |
| **D4** | pinmap `0x8c0000xx` + ANA pads `0x82001850..0x1884` | 143 direct 0x8c writes (rows 73-215, incl. banned 0x8c0002a4=0x231 row 215) + 6 ANA pad writes (rows 216-227) | **never written** (no pinmux in audio.c) | **MISSING with HW-safety split**: 0x8c half stays BANNED on HW; the ANA pad half (0x18a/0x18a/0x10a/0x100/0x100/0x101 via ADI) is mailbox-safe and addable |
| **D5** | ownership `0x8b0001c4` | **0x600** (mute path, cross-map R2) + **0x4** = b2 ARM_VB_ACC (driver-cycle rows 1-2) | only **0x200** = b9 (audio.c:292-294); audio_beep vb_ctl ORs 0x1C0 bits 5/6/8 only (audio.c:380-381) | **WRONG VALUE** - full ARM-owns set = **0x604** (b9\|b10 codec + b2 VBC). Assert b10 and b2 too |
| **D6** | `0x82001a44` keepalive | read = 0 + write = 0 every cycle (rows 8-14, 20-26, 32-38) | **never touched** | **MISSING (secondary)** - codec RAM-shadow region, exact role UNLISTED/UNKNOWN |
| **D7** | DSP IRQ `0x8b000160` = 0x2 | driver-cycle rows 45-46 | never sent | Record only - codec is ARM-controlled (`is_dsp_ctl==0`); not required for a beep |
| **D8** | VBC `0x82003018` DMA-EN + VBC power half + b2 | `_vbc_en_op` bits 1<<13/14 (DA) + 1<<11/12 (AD) on 0x82003018 + 1<<18 on 0x8b000060/64 + ARM_VB_ACC (cross-map row 12) | only VBENABLE 1<<15 (audio.c:407-408); power half is 0x200000 bit 21 (audio.c:304-305), never 0x40000 bit 18 | **MISSING** - add the DMA-EN bits + the 1<<18 VBC power half + b2 for the sound-data path |
| **D9** | PA power halves `0x8b000060/64`, `0x8b0000a0/a4`, ANA `0x82001450/54`, `0x82001440/44` | cross-map rows 4/5 (FUN_00081516/0x81562; later path, not captured) | audio.c:304-311: 0x200000 on 0x8b000060/64, 0x10000000 on 0x8b0000a0/a4, adi 1 on 0x82001450/54, adi 4 on 0x82001440/44 | **CONFIRMED MATCH** - byte-identical to the cross-map; keep |
| **D10** | ordering | subsystem first (P2-P6), codec chain later (never reached on warm path) | codec registers first (audio_power_on → aud_dac_path_on → audio_dac_on → DP → PA), subsystem power/clock/pinmux NEVER | **structural gap** - the whole P2-P6 phase is absent and should precede the codec register writes |

**Highlights:** **D1** (ANA clock block) is the #1 missing step; **D5** — full
ARM ownership is **0x604**, not 0x200; **D8** — the VBC DA engine is never
armed (no DMA-EN bits, no 1<<18 power half).


## 9. Recommended next hardware step

1. **`audio_power_on()` (drivers/audio.c:103)** — prepend the subsystem phase
   in the exact captured order (the full value lists are the P3/P4/P5/P6 rows
   of §7):
   a. `MEM4(0x205000c0) |= 0x60;` (row 5)
   b. the 23-bit APB ladder on 0x8b0000a0 (P3 — 0x1000000, 0x400000, 0x10000,
      0x40000000, 0x2, 0x200000, 0x200, 0x80000, 0x40, 0x800000, 0x80, 0x4000000,
      0x2000000, 0x100, 0x2000, 0x400, 0x800, 0x10, 0x8000, 0x4000, 0x20000,
      0x8, 0x80000000).
   c. the 15-write ANA clock block (P4 — `aud_adi_write(0x820010e0, 0x4)`,
      `(0x10e4,0x2)`, `(0x10e0,0x10)`, `(0x10e4,0x10)`, `(0x10e0,0x20)`,
      `(0x1040,0x2)`, `(0x1040,0x1)`, `(0x10e0,0x8)`, `(0x10e4,0x4)`,
      `(0x10e0,0x2)`, `(0x10e4,0x8)`, `(0x10e0,0x40)`, `(0x10e4,0x20)`,
      `(0x10e0,0x80)`, `(0x10e0,0x100)`).
   d. the 14-bit AHB ladder on 0x20500060 (P5 — 0x4000, 0x2, 0x400, 0x100, 0x1,
      0x4, 0x8, 0x800, 0x10000, 0x8000, 0x20, 0x1000, 0x40, 0x80).
   e. the 6 ANA pad writes (P6 — MAILBOX-SAFE; the 0x8c half stays BANNED on
      HW): `aud_adi_write(0x82001874, 0x18a)`, `(0x82001850, 0x18a)`,
      `(0x82001878, 0x10a)`, `(0x8200187c, 0x100)`, `(0x82001884, 0x100)`,
      `(0x82001880, 0x101)`.
   Then the existing codec power ladder (0x820012C0 bits + GPIOs) and the
   0x82001294 = 0 write follow unchanged.
2. **`audio_pa_mute()` (drivers/audio.c:292-294)** — unmute asserts the full
   ownership set `v |= 0x604` (b9 | b10 | b2); mute clears 0x604, not just 0x200.
3. **`audio_beep()` VBC section (drivers/audio.c:380-408)** — add
   `MEM4(0x82003018) |= (1<<13) | (1<<14);` (VBDALDMA_EN | VBDARDMA_EN) plus
   VBENABLE 1<<15; `MEM4(0x8b000060) |= 0x40000;` and `MEM4(0x8b000064) |=
   0x40000;` (the VBC 1<<18 power half — bit 18, NOT the PA's bit 21 0x200000);
   keep the vb_ctl 0x8B0001C4 OR of bits 5/6/8 and add bit 2 (ARM_VB_ACC).
4. **Rebuild + hardware test**: `make` → flash → press LSOFT (`audio_beep(440,
   300)`). Expected: 440 Hz square wave → speaker. If still silent, add the D6
   probe (`0x82001a44 = 0` after `audio_dac_on`) and check the PA/AMP_EN GPIO
   (candidates 18/39).

After this, `audio_beep` performs: subsystem power (P2-P6) → codec power ladder
→ DAC path → DP DAC → PA → VBC data path — matching the stock order (D10),
ownership (D5), and sound-data path (D8).


## 10. Remaining unknowns (honest frontier)

| # | Unknown | Evidence / status |
|---|---|---|
| 1 | NCP2817BFC **AMP_EN GPIO number** | candidates 18/39 + GPIO_0/63 (emulator). os-dsp-boot phases toggle 18 then 39. |
| 2 | Whether the VBC accepts **CPU-fed samples** with no DMA | the decisive test (DIRECT/VRAMSW vs DMAEN). |
| 3 | **SC6530C system-DMA base** | not in SDK/dump audio code; porting the DMA path is blocked until HW-probed. |
| 4 | DSP **NV-config forward** (the dsp_codec params) | the stock loads NV → `AUDIONVDSP_InitModeManager` @0x87B64 → walker @0x87764 → share-mem config → DSP applies (Z-space 0xBC78/0xBD77). Runtime-dispatched, no static command format; NOT needed if the ARM VBC path works. |
| 5 | `0x82001a44` codec-state keepalive (D6) | the stock RMWs it in the DSP keepalive cycle; role UNKNOWN (emulator-only). |
| 6 | Jack/headset-detect gate | `ADC_CheckHeadsetStatus` @0x22F64; the speaker path may gate on the jack state. |


## 11. The emulator capture status — the chime was NEVER captured

The QEMU machine (`tools/qemu-b310e/`, `./stock/` images) has **no audio output
model** — the VBC/ANA devices are log+store observatories. The emulated stock
OS has never played a chime: `0x820030xx` has **0 hits** and the codec-DAC
registers appear only as a `0x82001320` status read. The warm-path stock OS
stalls in a pre-UI driver re-init loop (15 keys injected, no response); the
playback writes are **PSRAM-resident function pointers** (DAI device-send
callback at `device+0x14`) — only reachable by running the emulated stock OS
far enough, or by a GDB-jump into its tone path.

**What the emulator DID capture** besides the P1-P7/D1-D8 material: the DSP
keepalive cycle (46 rows) + **three codec-IF register writes in the periodic
audio cycle that our chain NEVER makes**:

| Reg | Stock value (periodic) | Our chain | Meaning |
|---|---|---|---|
| `0x820011C0` | **0x19** (bits 0,3,4) | never written | unlisted codec IF register — a missing clock/enable candidate |
| `0x820011A0` | 0x2 (bit 1) | bits 4/15 only | IF1 keepalive bit — completes the stock IF1 state (0x8012) |
| `0x82001180` | 0x0 (cleared) | gated (HW stall) | IF0 — the clear is left out (|= 0x10 stalls the AHB) |

**Emulator-vs-reality boundary:** the captured sequence is the **subsystem
power/clock/pinmux phase** of the stock audio bring-up (chain `FUN_00034ed4` →
`FUN_0001a33e` → `FUN_0001a4e4` → `FUN_00035c98` → `FUN_0006a410` → PSRAM thunk
0x400002b2). The PA/DAC/VBC codec chain is a **later path the warm boot never
reached** — so D8's VBC requirements come from the protocol doc (cross-map row
12, R5), not a capture, and D9 stays a doc-level match pending hardware. The
0x8c pinmap WAS executed in the emulator (log+store model); it stays **banned
on the real phone** — only the mailbox-safe ANA pad half (P6) is portable.


## 12. The test image — `os-dsp-boot.bin` (built by `make debug`)

Build: `make debug` (dsp-boot extracts seg0 from the DSP blob
`dsp-blob-CC874.dec.bin` with a SHA256 gate; links os.ld, relocatable). Staged:
`sdcard/progs/os-dsp-boot.bin`. Runs: chip init → **DSP boot** (stages 1-6 with
LCD read-backs) → **post-boot probe** (10 locations, 2 s) → **audio chain**
(§3) → **5 tone phases**:

| Phase | Freq | VBC feed | AMP | Isolates |
|---|---|---|---|---|
| T1 | 440 Hz | DIRECT (no DMA-EN) | GPIO 18 | the DP fixes + CPU path |
| T2 | 660 Hz | VRAMSW stop-start | GPIO 18 | the MCU-buffer dance |
| T3 | 880 Hz | DMAEN (b13/b14) | GPIO 18 | the DMA-request mode |
| T4 | 990 Hz | DIRECT | GPIO 18 | **fs=9 vs fs=10** |
| T5 | 550 Hz | DIRECT | GPIO 39 | the second AMP_EN candidate |

The LCD row 13 shows the current phase tag; the DSP verdict rows + the `VBC=`
read-back (0x82003018 after T1) print the register state. The chain includes
the emulator-captured keepalive steps ("IFC0" = 0x820011C0 |= 0x19, "IF1+2" =
0x820011A0 |= 0x2, re-asserted every 500 ms during the tones). The user reports
WHICH frequencies sound → the working feed mode + amp GPIO.


## 13. stock-spy — the software bus spy (the JTAG-equivalent)

**What it is:** a software bus spy that runs *inside* the stock OS to capture
EXACTLY what the stock firmware writes to its audio registers when it plays the
bootup chime or a song — the capture the QEMU machine could never produce (no
audio model). No JTAG is possible on this device (no test points, BGA, fused).

**How it works:** `stock-spy.bin` boots the real stock OS from PSRAM (the stock-
ram shim) with a 6-byte patch on the stock's ADI write helper (`0x3038A`). Every
analog-register write is captured by a hook at VA `0x042F0000` (PSRAM) into a
ring buffer: **ring1** (512 × {addr, value}, every ANA write) + **ring2** (128 ×
{ownership, PA power, PA power2, DSP IRQ}, snapshotted on every write into
`[0x82001000, 0x82001600)`).

**Run / recover:**
1. Build: `powershell -ExecutionPolicy Bypass -File tools\stock-spy\pack-stock-spy.ps1`
   → `stock-spy.bin` (patches the `0x3038A` ADI-write site, places the 8 KB
   payload @ image 0xE000, writes the `1SPY` magic).
2. Boot: `.\spd_dump.exe fdl nor_fdl1.bin 0x40004000 fdl ..\stock-spy.bin ram`.
3. WATCH + LISTEN: the phone boots the STOCK OS (LCD blank is known); the bootup
   chime plays (~2-5 s) — that is what's captured; optionally play a song.
4. **Hold CENTER ~3-4 s** — the spy fires the watchdog; PSRAM survives the
   reset and the same CENTER hold re-enters USB download mode.
5. Recover: `.\spd_dump.exe fdl nor_fdl1.bin 0x40004000 read_mem 0x352F0800
   0x2000 spy-rings.bin`; decode with `tools\stock-spy\read-spy.ps1`.
   Alternative: load `os.bin` (built-in `spy_dump()` in arch/main.c checks the
   `1SPY` magic at `0x352F0800` and streams the rings over kprintf →
   libc_server).

**What the trace answers:** **ring3** = the captured UART debug text (the
AST_BLUESCREEN/assert message names the file/line — why the RAM-booted stock OS
hangs). **ring1** = the playback-time codec config (`0x820010xx/0x820011xx/
0x820012xx` writes before any `0x82003018` activity) + the `0x82001a44`
keepalive + the missing IF registers (`0x820011c0` = 0x19, `0x820011a0`,
`0x82001180`). **ring2** = ownership (`0x8b0001c4`: 0x604 = ARM owns codec+VBC,
b2 = ARM VBC, 0 = DSP), the PA power sequence (`0x8b000060`/`0x8b0000a0`: bit
21 = PA, bit 18 = VBC, bit 28 = PA2), the DSP interaction (`0x8b000160`).

**Safety:** zero NOR writes (RAM-load only); the hook lives at 0x042F0000 (just
above the stock OS's 2.9 MB PSRAM high-water). The spy snapshots ONLY
always-clockable GLB registers (an unpowered-peripheral read freezes the bus).
Files: `tools/stock-spy/{pack-stock-spy.ps1, shim-spy.s, spy-hook.s,
read-spy.ps1}`.


## Sources (condensed)

- **Protocol + cross-map + NV:** `docs/audio-dsp-protocol.md` (DSP-interface
  §4c/§5/§7, Codec-driver §2/§6, Cross-map rows 1-12 + R1-R5, NV §1/§4).
- **Captured sequence:** `tools/qemu-b310e/logs/w6/stock-sound.csv` (231 rows;
  P1-P7) + `stock-sound-driver-cycle.csv` (46 rows; the DSP keepalive).
- **Our chain:** `drivers/audio.c` (`audio_power_on` 103-132, `aud_dac_path_on`
  158-190, `audio_dac_on` 226-265, `audio_dp_dac_on` 197-219, `audio_pa_enable`
  300-313, `audio_pa_mute` 282-298, `audio_beep` 352-452); `drivers/audio.h`.
- **Stock firmware functions (Ghidra):** `FUN_00034ed4` (bring-up),
  `FUN_0001a33e` (codec-ID read), `FUN_0001a4e4` (AHB gate), `FUN_00035c98`
  (pinmap walker, table @0xc6ef0), `FUN_0006a410` → PSRAM 0x400002b2 (audio
  dispatcher), `FUN_00080744` (RMW helper), `FUN_000810fe` (codec RAM-shadow
  dump @0x82001A00); VBC PHY `FUN_000b06a0`/`b06d6`/`b060c`/`b071a`/`b077a`/
  `b0790`/`b0820`/`b0bd8`, DAI `FUN_000a7844`/`a78e4`/`a7d0c`/`a7806`/`a80f4`
  (pools @0x0B0A14/0x0B0A18/0x0A7C08), DSP keepalive `0x1A480`-`0x1A680`.
- **stock-spy tooling:** `tools/stock-spy/` + `arch/main.c` (`spy_dump()`).
