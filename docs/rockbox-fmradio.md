# B310E Rockbox FM radio

Rockbox's FM radio screen (Main Menu → FM Radio) drives the phone's
Richwave RTC6211 tuner (U101). The service manual says RTC6211S; the phone
tested has the **RTC6211N**. The driver supports both. The audio takes the codec's analog
line-in bypass to the headphones or the speaker. The headset cable is the
antenna, so plug in headphones even when listening on the speaker.

**Status: verified on the phone (2026-11-06).** It plays and scans, and
the owner reports the audio is great. The dump shows `present=1 N=1
nor_tables=1`, reg01 = `0x12c0`, and RSSI `0x24` on 100.0 MHz.

The first build supported only the S. It found the chip but reported no
tuner: reg00 read 0x1688 and everything else read 0xFFFF, which is the N
before it is woken.

**Known glitch:** once, after a lot of key presses, the radio played
nothing until the screen was left and reopened. It has not been
reproduced; note the last keys if it happens again.

## Hardware

From the service-manual block diagram and the stock firmware (`e52q7a`;
NOR function addresses):

| Signal | Connection | Stock source |
|---|---|---|
| FM_SCL / FM_SDA | I2C controller 1 at `0x86001000`, 100 kHz, pad select `0x8b0001e0 \|= 2` | `i2c_phy_v5` `0xa6ba4`, `0xa6e6c`; device struct NOR `0x128d34` |
| Tuner address | 7-bit `0x64` (writes `0xC8`, reads `0xC9`) | same |
| FM_RST | GPIO 31 (bank 1 bit 15); low = reset/off | `0xa14a6`, `0xa14e4` |
| 32KHz_DIGIT | IISMCK pad: pinmux `0x8c00017c = 0x111`; boot value `0x140` restored on off | `FM_ConfigClk` `0xa1484` |
| FM_L / FM_R | codec line-in AIL/AIR | codec v0 LINEIN function |
| FM_INT | not used: stock polls | — |

**I2C controller 1:**
- Registers: CTL `+0x00` (b0 INT, b3 IE, b4 EN), CMD `+0x04` (data 15:8;
  b0 INT_ACK, b1 NACK, b2 WRITE, b3 READ, b4 STOP, b5 START, b6 slave NACK,
  b7 BUSY), dividers `+0x08`/`+0x0c`, reset `+0x10`.
- Clock `0x8b0000a0 = 0x80000000`; reset pulse `0x8b000060`/`0x64` bit 25.
- Each command waits for CTL.INT and BUSY clear, then writes `CMD | 1` to
  acknowledge.

**The pinmux write.** `0x8c00017c` is the only pinmux register the port
writes, against the earlier "no 0x8c writes" rule. The torch-test dump
showed the boot menu already applies the stock pinmux table on this phone
(`0x17c` reads back `0x140`, the stock boot value). One targeted register
is far from the full-table replay that hung the phone.

## Tuner protocol

Writes always start at register `0x02` and reads at `0x0A`; both wrap
`0x0F` → `0x00` (Si470x framing). The register bits are the Richwave
RTC6213 layout, **not** the Si4702 one, so Rockbox's si4700 driver does
not apply.

| Reg | Use |
|---|---|
| 00 / 01 | `0x1688` / bits 15:10 = 3 (RTC6211S) or 4 (RTC6211N, once awake) |
| 02 | b15 DSMUTE, b14 DMUTE (1 = audio on), b3:0 volume (stock 0..12) |
| 03 | b15 TUNE, b13:12 band (0 = 87.5–108, else 76 MHz base), b11:10 spacing (0 200 kHz, 1 100 kHz, 2 50 kHz), b9:0 channel |
| 05 | b15 SEEK, b14 SEEKUP, b7:0 seek RSSI threshold (stock `0x13`) |
| 06 | b15 ENABLE, b14 DISABLE |
| 08 | hidden bank select (init tables only) |
| 0A | b14 STC, b13 SF, b10 stereo, b9:0 read channel |
| 0B | b7:0 RSSI |

Tuning sets channel and TUNE, polls STC (stock: 20 × 50 ms), then clears
TUNE.

**RTC6211N** (stock probe `0xaf2be`, driver `0xae680..0xaf480`):
- **Asleep:** before it is woken, every register except reg00 reads
  0xFFFF.
- **Turn off / on:** 30-byte streams set reg02..0F to 0 and then, by
  wrap-around, reg00 to `0x16AA` (off) or `0x96AA` (on). The driver builds
  these itself.
- **Probe:** read 32 bytes; reg00 must be 0x1688. If reg01[15:10] is not 3,
  wait 200 ms, turn off, wait 30 ms, turn on, wait 50 ms, read again;
  reg01[15:10] = 4 means N.
- **Start** (`0xaed62`, `0xae758`), with 30 ms between steps:
  1. Turn off, then turn on.
  2. The five bank tables.
  3. reg02..09 = `0 0 0 0 8000 0 0 4050`.
  4. rPwrOn reg02..07 = `C00F 0400 0000 000C 8000 7000`.
  5. The shadow with ENABLE, then band/spacing, threshold `0x13`, mute,
     volume.
- **Close** (`0xaeeb6`): mute, rPwrOff `C00F 0400 0000 000C C000`, turn
  off, clear ENABLE.
- **Normal operation:** tune, status, volume and seek use the same
  registers as the S.

**Init tables.** Both variants stream 46-byte bank tables at power-up:
- **S:** six, at NOR `0xa1990`, `0xa19c0`, `0xa19f0`, `0xa1a20`, `0xa1a50`,
  `0xa1aa0`; CRC `0x871c1aa4`.
- **N:** five, at `0xae9ec`, `0xaea1c`, `0xaea4c`, `0xaea7c`, `0xaeaac`;
  the CRC `0xa305ae11` also covers the turn-off/on streams at `0xae9ac`
  and `0xae9cc`.

The driver reads the tables from **this phone's own NOR** at run time and
uses them only if the CRC (Rockbox `crc_32`) matches the stock image. The
repository carries the offsets and checksums, not Samsung's bytes. If the
CRC does not match, the driver skips the tables and continues.

## Analog route

FM never touches the ADC/DAC data path or the DSP: AIL/AIR → ADC PGA →
PGA output buffers → headphone or speaker mixer. Codec offsets from
`0x82001a00`:

| Step | Write |
|---|---|
| ADC input buffer, PGA power | `+0x5c &= ~0x80`; `+0x60 &= ~0xa0` |
| Line-in PGA gain | `+0x70 = 0x88` (stock value is in NV; not extracted) |
| Line-in mux | `+0x6c = 0xac` (AIL→L, AIR→R, VCM), 250 ms settle |
| DAC off the outputs | `+0x7c = 0` |
| Headphones | `+0x78 = 0x84` (PGAL→HPL, PGAR→HPR), `+0x60 \|= 0x0a`, HP enables as for music, GPIO 0 high |
| Speaker | `+0x78 = 0x33`, `+0x60 \|= 0x0f`, speaker PA as for music |

`select_output()` applies the FM mixer whenever FM is on, so jack
switching during FM works the same way it does for music. Leaving FM
restores the DAC route.

**Volume.** Software PCM volume cannot reach an analog path. Below 0 dB,
Rockbox's volume becomes the tuner's own volume steps (about 2 dB each,
unmeasured; 0 mutes). Positive volume uses the headphone gain, as for
music.

## Driver layout

- `firmware/target/arm/sc6530c/fmradio-i2c-sc6530c.c`: the I2C controller,
  `fmradio_i2c_read/write`, and `tuner_power()` (32 kHz pad, GPIO 31).
- `firmware/target/arm/sc6530c/rtc6211.c` and `firmware/export/rtc6211.h`:
  the tuner driver, modelled on Rockbox's rda5802 driver.
- `audiohw-sc6530c.c` `audiohw_fm_route()` and `audio-sc6530c.c`
  `audio_input_mux()`: the analog route.
- `b310e.h`: `CONFIG_TUNER RTC6211`, `INPUT_SRC_CAPS` with
  `SRC_CAP_FMRADIO`, and `REC_SRC_CAPS SRC_CAP_MIC` (no FM recording yet).
- `scripts/targets/rockbox.sh`, patch 20: adds the `RTC6211` tuner type to
  `config.h`, `tuner.h` and `tuner.c`.

## Radio screen keys

| Key | Action |
|---|---|
| Left / Right | Tune down / up; hold to seek |
| Up / Down, 2 / 8 | Volume |
| 4 / 6 | Previous / next preset |
| Center | Scan ↔ preset mode; hold for the preset list |
| DIAL | Play / mute |
| Left soft key | FM menu |
| Right soft key | Leave the screen; the radio keeps playing |
| END | Stop the radio |

## QEMU model and test

`emulator/qemu/machine/hw/misc/sc6530_i2c.c` models I2C controller 1 and
an RTC6211S on its bus:
- IDs, framing, bank select, tune and seek.
- Stations come from `-global sc6530_i2c.fm-stations=10000,9230` (10 kHz
  units; that is the default).
- `-global sc6530_i2c.fm-chip=n|s` picks the variant. The default is `n`,
  which reads 0xFFFF until woken, as the phone did.
- On a station, RSSI is 50 and stereo is set; elsewhere RSSI is 5.
- Trace events: `sc6530_i2c_cmd`, `sc6530_fm_reg`, `sc6530_fm_tune`.

`ports/rockbox/tests/test-fmradio.py` (`--qemu`, `--rockbox`, `--runtime`,
`--nor`, `--output`, `--chip n|s`) starts Rockbox in the radio screen with
the stock NOR attached and accepts the auto-scan prompt. It then checks
that:
- the tuner was addressed and every NOR table was streamed (5 for N, 6
  for S, counted as 46-byte writes);
- the scan found exactly 92.3 and 100.0 MHz;
- the codec line-in and headphone mixer registers were set;
- the reset line and the 32 kHz pad were driven.

## Recording FM

Recording → Source: FM Radio records a mono mix of both FM channels: the
SC6530C's right ADC does not work, so stereo capture is impossible. The
headphones carry the same mono mix while recording. See
`docs/rockbox-recording.md`.

## Debug

The Debug → View I/O ports dump (`.rockbox/torch-b310e.txt`) includes an
`FM` line:
- `present`: the tuner ID check passed at boot;
- `N`: the chip is the RTC6211N;
- `powered`;
- `nor_tables`: the NOR CRC matched;
- the 32 kHz pad register;
- the driver's 16-register cache.

## Unconfirmed

1. Line-in PGA gain: default `0x88`, now adjustable as the recording
   screen's left/right gain. The tuner volume step size is unmeasured.
2. The band field for Japan (76–90 vs 76–108); Europe/US use band 0.
3. RSSI scale: `RSSI_MAX` is 80, a guess; scanning uses the stock seek
   threshold `0x13`.
4. The silent-after-fiddling glitch above (not reproduced). Likely
   cause: an I2C transfer failing during the half-second start
   sequence, which the first build ignored, leaving the chip
   half-configured until the next stop/start. The driver now counts
   I2C errors, and if a start saw any (or the chip does not read back
   its ID), it resets and starts again, up to 3 tries. The dump's FM
   line shows `i2c_errors` and `retries`. Reads also no longer
   overwrite the write shadow (registers 02–09), matching stock.
