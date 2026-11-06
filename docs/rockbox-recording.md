# B310E Rockbox microphone recording

The port records from the main microphone through the on-die ADC and the
VBC capture FIFO, with the ARM owning the path. Rockbox's normal recording
screen (Main Menu → Recording) writes WAV, AIFF, WavPack or MP3 files to
`/Recordings`.

**Status: verified on the phone (2026-11-06).** It records clean speech at
16 kHz: peaks around ±22,000, no clipping, DC offset about −60. The owner
rates the quality well above the stock recorder. It took three hardware
rounds:

1. **Constant full-scale rail** (0x8000 from AD0, 0x7fff from AD1, no LSB
   noise); DMA and file writing worked. The cause was the missing "ADC
   core" power-up described below.
2. **ADC core powered, VBC `+0x80` bits `0x30` set:** all-zero samples.
   Those bits route AD to the DSP.
3. **Bits cleared** (Debug → View I/O ports, key 7): working audio. The
   driver now clears them by default.

Both failures reproduce in the QEMU model.

## Why not the stock recorder path

The stock Voice Recorder sends commands to the audio DSP through shared
memory: the IIS pins are set to `0x188`, then the DSP is started. The DSP
owns the VBC capture and does the encoding. QEMU does not execute DSP
code, so a trace of the stock recorder stops there.

The stock VBC and codec drivers also contain an ARM-owned capture path,
the mirror of the ARM playback route this port already uses. That is the
path implemented here. No vendor DSP firmware is involved.

## Register sequence

Every value below comes from the unmodified `e52q7a` firmware. The
function addresses are NOR offsets. The codec analog block is at ADI
`0x82001a00`, the digital codec at `0x8a002000`, and the VBC at
`0x82003000`.

| Step | Write | Stock source |
|---|---|---|
| AD0 and AD1 owned by ARM | `0x8b0001c4 \|= 0x18` (stock capture DAI modes are all `0x30xxxxxx`) | `0xb0656`, mode table RAM `0x4238680` (init image NOR `0x12ac50`) |
| AD not routed to DSP | VBC `+0x80 &= ~0x30` (when set, the ARM FIFO reads zeros on hardware) | `0xb08ce` via `0xa7e28` |
| ADC rate first | `0x8a002018[3:0] = rate / 4000` | codec open `0x69c92` |
| Mic gain | codec `+0x70` bits 7:4 | gain ioctl `0x69bfa` → `0x80a4e` |
| Main mic → ADC L | codec `+0x64 \|= 0xc0` | input path 8, `0x80eec` |
| Mic bias supply | `0x82001164 &= ~0x200` (LDO id 0x1b) | `0x80876` → `0x80846`, table `0xca4f4` |
| Mic bias | codec `+0x40 \|= 0x04` | same |
| **ADC core** | ADI `0x82001440 = 1`, then `2`; codec `+0x5c &= ~0x80`; `+0x60 &= ~0xa0`; `+0xac &= ~0x40` | power state 5 `0x820d4` → `0x8153e`, `0x80af8` (`0x80a1c`, `0x80a30`, `0x80ae4`) |
| ADC L power | codec `+0x5c &= ~0x02`; `0x8a002000 \|= 0x02`; codec `+0x00 \|= 0x02`; codec `+0x5c &= ~0x20` | `0x80b4e(1, 1)`: `0x80b24`, `0x814f8`, `0x809fe` |
| AD bank size | VBC `+0x10[7:0] = 159` | `0xb071a` via `0xa8494` |
| DMA request | `0x20102034 = channel` (AD0, request 14); `0x20102030` for AD1 (request 13) | bind `0xa7b96`, unbind `0xa7c58` |
| VBC enable | `VBC_CTRL \|= 0x8000 \| 0x1800` (both AD channels; DMA drains AD0 only) | `0xb06d6`, `0xb0874` |
| DMA channel | source = FIFO `0x8200300c` (AD0) / `0x82003008` (AD1), destination = RAM, `+0x18 = 0x50100140`, 320-byte blocks | `0xb0950`, `0x32a68` |

Stopping reverses these steps.

**The ADC core step is required.** Stock's codec state machine has a
separate capture power state (5) that `0x81cfc` requires before it will
enter the ADC state (6, `0x82146`). Playback never requests state 5. The
first hardware build skipped it, and the ADC returned a constant rail.
The QEMU model now starts these power-down bits set (`+0x5c` = `0xa8`,
`+0x60` = `0xa0`, `+0xac` = `0x40`) and records silence until they are
cleared, so the test fails on that earlier build.

The digital codec window is only `0x8a002000`–`0x8a002028`. Stock writes
no ADC reset, sinc or DMIC bits there.

**Sample rates:** the ADC rate field is `rate / 4000`, so recording offers
only 8, 12, 16, 24, 32 and 48 kHz. The default is 16 kHz.

**Mono:** stock powers only ADC L, so recording is mono. Each sample is
written to both channels of Rockbox's stereo buffer, and the encoder's
mono setting then stores a single channel.

## Headset mic detection

**How it decides:** at the start of a mic recording with headphones in,
the driver turns on mic bias, selects the headset mic and reads the mic
line on **ADC channel 1**. This is the channel stock's
`ADC_CheckHeadsetStatus` (`0x22d7a`) uses: the mean of two 10-bit
conversions. A 4-pole headset's mic sits well above ground; a 3-pole plug
grounds that contact.
- **Reading above `HEADSET_MIC_MIN_RAW` (40, a first guess):** the headset
  mic is used.
- **Otherwise:** the phone's mic is used.

**Calibrating it:** the dump's `MIC` line shows the reading at the last
recording start (`mic_adc_at_start`), the live reading, the path used and
the headset-button EIC level.

**On hardware (2026-11-06):** detection works. A headset with a mic reads
about **508**, far above the threshold of 40.

**Related stock facts:**
- `GetHeadsetStatusVoltage` converts with raw × 133 / 198 (mV).
- Stock treats a mic line below 121 mV as the in-line button being held
  (`0x136d8`).
- The button itself is product GPIO `0x13` = EIC 20, active high, which is
  analog EIC channel 4 (`0x82001900` bit 4).

**Headset buttons.** A three-button headset puts its buttons on the mic
line: the centre one shorts it (EIC channel 4's comparator sees only
that one), the side ones pull it partway down through a resistor, which
only the ADC can tell apart.

They work only for a headset that passes a check when it is plugged in
(or found at boot); a 3-pole plug grounds the mic contact and would read
as "held" all the time. From the 100 Hz button tick
(`sc6530_headset_tick`):
1. Headphones appear: wait 300 ms for the plug to settle.
2. Mic bias on, wait 100 ms.
3. Enable the buttons only if the mic line (ADC channel 1) reads above 40
   raw (a mic) **and** EIC channel 4 reads released. That reading becomes
   the idle level. Otherwise the bias goes off again and the buttons stay
   disabled until the next plug-in.

While enabled, the tick samples the mic line every 20 ms (every tick while
the comparator is high) and classifies it against the idle level, which
tracks slow drift while released:

| Mic line vs idle | Phone reading (idle 509) | Button | Tap | Hold (> 0.4 s) |
|---|---|---|---|---|
| >= 50% | ~509 | released (or centre, if the comparator is high) | – | – |
| < 4% | 0 | centre | Play: play/pause | Play |
| 4–12.5% | 40 | + | Next (skip forward) | Volume up, repeating |
| 12.5–50% | ~86 | − | Previous (skip back) | Volume down, repeating |

The side buttons wait for the release: let go within 0.4 s and the driver
sends a 60 ms tap of `BUTTON_NEXT` / `BUTTON_PREV` (the music screen skips
on their release); hold longer and it holds `BUTTON_VOL_UP` /
`BUTTON_VOL_DOWN` until release, which Rockbox's key repeat turns into
volume steps (`hook_output`). The centre button is Play while held. In
menus a tap is Next/Previous, which most lists ignore.

Which side is which comes from the press log: + pressed three times,
then − three times, logged `-40 -40 +86 +86 +84 +86` with the first
mapping (oldest first), so − reads ~86 and + reads 40.

A class must hold for 40 ms (and two samples) before it counts; while a
press is settling the line is sampled every tick. (60 ms in the first
calibrated build; 40 ms with 20 ms sampling lets a quick tap register
within about 60 ms.) The first build took a
side button for Play and paused playback: the reading of 40 fell in its
centre band, and a side button's contacts probably short the line for a
moment on first touch, which a two-samples-10-ms rule caught. QEMU's
`headset-bounce-ms` property models that short (test case
`side-button-bounce`). The dump's `HOOKLOG` line lists the last six
presses as class and raw value (`C0 +90 -40`); the `HOOK` line also
counts taps and holds (`taps=next/prev holds=up/down`).

The readings are from the phone with
the user's headset (2026-11-06); boundaries sit halfway between them.
Volume up pulls the line lower than volume down here.
The I/O screen shows the live raw value, the
idle level, the comparator, the class held (line 1) and the last press's
raw value with per-button counts (line 8); the dump's `HOOK` line has the
same.

**While recording from the headset mic, all headset buttons are off**:
any press shorts or pulls down the mic being recorded, sampling the line
would add to the recording, and the centre button (Play) starts a new
file in the recording screen. Recording anything else (FM, or the phone's
mic) keeps all of them. With a mic headset plugged in, mic recordings
always use the headset mic, so the rule is simply "recording with the
headset mic path (`+0x64` bits `0x0c`)". The dump's `HOOK` line counts
the ticks spent off (`muted`). `ports/rockbox/tests/test-headset-recording.py`
presses the centre button mid-recording: one WAV for a headset-mic
recording, two (a new file) for FM. The build before this rule split the
headset-mic recording into two files.

The mic bias stays on while the buttons are enabled. Recording and the
buttons share it through a reference count (`mic_bias()`). In QEMU the
keypad's `h`, `j` and `k` keys hold the centre, volume-up and volume-down
buttons; the side buttons pull the modeled line to the phone's ratios
(7.9% and 17% of idle) without the comparator (`ports/rockbox/tests/test-headset-button.py`).

**Headset mic clicks come from the LCD.** On the phone (2026-11-06) the
clicking stopped with the display off but not with only the backlight
off. With `LCD_SLEEP_TIMEOUT 0` the panel sleeps when the backlight times
out, which stops the LCD controller's DMA transfers to the panel. The
recording screen redraws constantly (peak meters, time), and that bus
traffic couples into the headset mic line. Workaround: a short backlight
timeout during headset recordings. No DSP filtering is used.

## Sources: phone mic, headset mic, FM radio

| Source | Path | Capture |
|---|---|---|
| Mic, jack empty | main mic, `+0x64 = 0xc0` | mono, ADC L → AD0 |
| Mic, headset with a mic | headset mic, `+0x64 = 0x0c` (stock path `0x20`) | mono, ADC L → AD0 |
| Mic, headphones without a mic | main mic, `+0x64 = 0xc0` | mono, ADC L → AD0 |
| FM radio | line-in mono mix: `+0x6c = 0xfc` (AIL and AIR into both PGAs) while recording | mono, ADC L → AD0, written to both channels |

**FM recording:** Recording → Source: FM Radio. Rockbox starts the radio
(tune it in the FM screen first), and the FM audio keeps playing while
recording.
- **Mono mix.** ADC R does not work on the SC6530C (see "Hardware sweep
  result" below), so FM is recorded as stock does: both FM channels mixed
  into PGA L and captured on ADC L. A "stereo" WAV gets the mix on both
  channels; choose mono channels to halve the file size.
- **Line-in mux** (stock `0x80bcc`): bit 7 AIL→PGA L, bit 6 AIR→PGA L,
  bit 4 AIL→PGA R, bit 3 AIR→PGA R, bits 5/2 the common mode. Listening
  uses `0xac` (stereo). While recording the driver sets `0xfc`, so the
  headphones carry the same mono mix as the file (stock's `0xe0` would
  leave the right ear silent); `0xac` returns when recording stops.
- **Line gain:** the recording screen's left "line" gain is the PGA L gain
  (`+0x70` [7:4], default 8) and sets the recording level; the right gain
  only affects listening. Raw steps 0–15. On the phone, gain 8 put a
  station's peaks near ±26000.
- The stereo capture code (AD1 on DMA channel 6, the rail fallback, the
  ADC R variants on key 7) remains for the key 0 sweep.

## Driver layout

- `audiohw-sc6530c.c`:
  - `audiohw_enable_recording()` / `audiohw_disable_recording()` handle
    ownership, mic path, bias and ADC power.
  - `audiohw_set_recvol()` sets the mic gain.
  - `audiohw_set_frequency()` also sets the ADC rate.
- `pcm-sc6530c.c`:
  - Capture DMA on channel 5 into two 160-frame banks.
  - The DMA interrupt, shared with playback, re-arms the next bank first,
    then widens the finished one into Rockbox's buffer.
  - Playback stop clears only the DA enables while a recording runs.
  - The shared DMA interrupt mask honours both the playback and the
    recording locks.
- `audio-sc6530c.c`: `audio_input_mux()` switches between playback and
  mic.
- `apps/keymaps/keymap-b310e.c`: the recording-screen keys.
- `scripts/targets/rockbox.sh`, patch 19: `pcm_record.c` uses Rockbox's
  small-memory buffer profile (about 224 KB of PCM FIFO). The default
  profile wants a 2 MB FIFO, which on this 4 MB phone left the encoder
  buffer smaller than one chunk. The recording screen then showed
  `Warning: 00000002` and wrote nothing.

## Recording screen keys

| Key | Action |
|---|---|
| Center | Start recording; then pause / resume |
| DIAL | Start a new file |
| Up / Down | Choose a line (volume, gain, …) |
| Left / Right, 2 / 8 | Change the chosen value |
| Right soft key / END | Stop; press again to leave |
| Left soft key | Recording settings |

## QEMU model

`sc6530_adi.c` models VBC capture:
- Each AD bank period it fills a 160-frame bank and raises DMA request 14
  (AD0) and/or 13 (AD1). The DMA reads the samples from the FIFO
  registers.
- The bank holds a synthetic sine (`-global sc6530_adi.mic-tone-hz=1000`,
  `mic-level=8192`). The level is scaled by the mic gain step
  (`(gain + 1) / 16`).
- The tone appears only while the ADC core, ADC L (digital and analog),
  the ADC L power-down clear, the main-mic path, the bias and the bias
  supply are all on, and VBC `+0x80` bit `0x20` (AD0 → DSP) is clear.
  Otherwise the bank is silent, so a driver that skips a step records
  silence. Both hardware failures reproduce this way.
- The trace event `sc6530_vbc_ad` logs each bank.

The test `ports/rockbox/tests/test-recording.py` (`--qemu`, `--rockbox`,
`--runtime`, `--output`):
- builds a private SD image whose config starts in the recording screen;
- presses Center, waits, then presses Back;
- finds the new WAV in the image;
- checks it is mono 16-bit at the configured rate, and that the tone's
  amplitude matches the gain within 5% with no energy off the tone.

Cases (`--cases` picks some):
- `mic-16k`, `mic-8k-gain15`: the phone's mic at two rates and gains;
- `headset-mic`: a headset with a mic, recorded at 2/3 of the gain;
- `headphones-no-mic`: falls back to the phone's mic;
- `fm-mono-mix`: FM line-in with a different tone on each input; both
  WAV channels must carry both tones;
- `sd-write-faults`: every 7th SD block write fails
  (`-global sc6530_sdio.fail-write-every=7`); the recording must still
  come out whole, and the test checks that faults were injected;
- `sd-slow-card`: at 48 kHz, every 1000th block write stays busy for 1 s
  (`write-busy-every=1000`, `write-busy-ms=1000`). The test tracks the
  tone's phase in 10 ms windows; a step means audio was dropped. The
  build that spun during the wait fell far behind and never finished its
  WAV; the yielding one records with no phase steps.
- `sd-48k-stereo-wav`: 48 kHz stereo WAV with every block write busy for
  1 ms. It passes with both the per-block-spinning and the yielding
  build, so it does **not** reproduce the phone's warning (see below).

With `-icount` the guest runs ahead of wall time, so a few seconds of
waiting produce a recording a few minutes long.

## SD writes during long recordings

Long recordings used to end in a panic from the FAT writeback
(`dc_writeback_callback() - Could not write sector`, 2026-11-06). Rockbox
panics there whenever the storage driver reports a failed write. Two
changes in the SD driver:
- **Longer wait for data commands.** A write completes only after the
  card has programmed it, which the SD spec allows to take 250 ms
  (500 ms for SDXC), and longer while the card does housekeeping during
  long sequential writes. The old poll budget (about 50–100 ms) ran out
  there, and the driver then reset the lines mid-write. Data commands now
  wait up to 1.5 s; the controller's own data timeout still ends a stuck
  transfer.
- **The wait yields.** Rockbox threads are cooperative, so spinning for a
  slow card also stops the UI and the encoder. On the phone a ~1 s stall
  froze the screen and overflowed the PCM buffer, which holds about 1.2 s
  at 48 kHz on this 4 MiB build (recording warning `00000001`,
  `PCMREC_W_PCM_BUFFER_OVF`). The driver now spins briefly (a normal block
  completes inside it), then polls with `yield()`. With interrupts off it
  keeps spinning, bounded by an iteration count.
- **Keys during the wait.** Each block pauses the keypad matrix scan
  around the SDIO DMA (a held key plus SDIO DMA hung B310E-OS). With the
  yielding wait the pause also covered the card's programming time, and
  during 48 kHz stereo FM recording stop presses were lost. The scan now
  resumes while the wait yields (the 512-byte data phase is long over by
  then) and pauses again before the next command. QEMU does not reproduce
  the lost presses, so this rests on hardware testing.
- **Retries.** A failed block is retried up to 3 more times. Before each
  retry the driver polls the card status (CMD13) until the card is back
  in the transfer state and ready (up to 1 s), sending CMD12 if the card
  was left in the receive state.

The dump's `SD` line counts retried and failed blocks since boot. On the
phone it read `retries=0 failures=0` after a stall: the card was slow,
not failing. It also gives the write throughput since boot (`wr_kbps`,
time spent inside write requests) and the slowest request (`wr_max_ms`).

**Still open: 48 kHz WAV.** With WAV at 48 kHz the phone still shows
warning `00000001`; MP3 does not. In `pcm_record.c` a full encoder buffer
makes the encoder wait without a warning of its own, and the PCM buffer
behind it then overflows. So the warning means **the card writes can't
keep up** with 192 KB/s, not that a thread starved. The driver writes one
512-byte block per command (CMD24); a USB copy through the same path
peaked at about 347 KB/s. Multi-block writes (CMD25) are the likely fix.

## Debug tools

The View I/O ports dump (opening the screen, or key 9) appends two
`CODEC` lines and a `VBC` line to `.rockbox/torch-b310e.txt`:
- `recstart`: the codec registers right after the last recording power-up;
- `now`: the live values;
- the VBC line adds VBC `+0x10`/`+0x18`/`+0x80`, the owner register and
  the digital ADC registers.

Key 0 runs the **ADC R bit sweep** (about 25 s) and writes
`.rockbox/adcr-sweep.txt`. It turns on the line-in route if FM is not
already playing, powers stereo capture at 48 kHz without recording
(a stats-only capture mode in `pcm-sc6530c.c`), then flips one bit at a
time and measures both channels for 100 ms before restoring it:
- codec `+0x40` to `+0x6c` and `+0xac`, every bit (output registers from
  `+0x74` up are left alone);
- digital codec `0x8a002000` bits 0–7 and `0x8a002018` bits 4–7;
- VBC `+0x80` bits 0–7;
- the stock mux values (`0x1c`, `0x74`, `0xe0`) and ADC R variants 0 and 2.

Each line gives the left and right min/max and the number of banks.
`R-LIVE` marks a right channel that moves and is off the rail, `L-DEAD`
a left channel that does not, and `STALL` a window with no banks. The
test `ports/rockbox/tests/test-adcr-sweep.py` runs it in QEMU, where
ADC R works, and checks that the model's fatal bits are caught.

Key 7 cycles the ADC R power-up variant (0–3) used at the next stereo
recording start. The dump's `REC` line shows it, the number of banks
whose right channel sat at the rail and was replaced by the left, and the
raw right-channel data of the last stereo bank (`r_min`, `r_max`, first
four samples). A constant `-32768` means AD1 delivers no data at all;
noisy values near the rail mean ADC R runs but is powered or biased
wrongly. On the phone every variant so far gave the fallback: L and R
are bit-identical in the WAV. The first dump with the raw data (FM at
48 kHz, variant 1) read `r_min=r_max=-32768` for all 6298 banks: AD1's
DMA request fires, but the FIFO hands over a constant `0x8000`. The
digital side of the right channel is not running; stock never records
FM in stereo, so there is no reference sequence yet.

### Stock's per-channel codec helpers (traced 2026-11-06)

Stock's codec driver (`sprd_codec_state_v0.c`) is generic: its helpers
take a channel mask (bit 0 left, bit 1 right, `8` both) and
`0x80770(on, mask, reg, Lbit, Rbit)` sets or clears one bit per channel.

| Step | Register | Left | Right | Function |
|---|---|---|---|---|
| ADC power-down | codec `+0x5c` | `0x20` | `0x08` | `0x809fe` |
| ADC "R on" | codec `+0x5c` bit 1 | – | set when right is on | `0x80b24` |
| PGA power-down | codec `+0x60` | `0x80` | `0x20` | `0x80a30` |
| ADC core | `+0x5c` bit 7, `+0xac` bit 6, ANA `0x82001440` 1 then 2 | shared | shared | `0x80af8`, `0x8153e` |
| ADC enable | codec `+0x00` | `0x02` | `0x08` | `0x808bc` |
| ADC enable (digital) | `0x8a002000` | `0x02` | `0x08` | `0x811e6` |
| ADC sample rate | `0x8a002018` [3:0] | shared | shared | `0x81220` (index = rate / 4000) |
| Line-in mux | codec `+0x6c` | `0xa0` | `0x0c` (both: `0xac`) | `0x80bcc` |
| VBC AD enable | `0x82003018` | `0x1000` | `0x800` | `0xb06d6` |
| VBC DA enable | `0x82003018` | `0x4000` | `0x2000` | `0xb06a0` |
| DAC analog | codec `+0x74` | `0x80` | `0x40` | `0x80b7c` |

Line-in mux with one channel: left only `0xe0` (both FM channels mixed
into the left PGA; stock's mono FM recording), right only `0x1c`.
`0x8a002024`/`+0x28` are the codec interrupt's clear and enable bits
(IRQ 9), not ADC controls.

Variant 1 writes every right-channel bit above exactly as stock would
(dump: `+00=001f`, `+5c=0002`, `+60=000a`), and ADC R still delivers a
constant `-32768`. A sigma-delta ADC whose input is driven into a rail
also gives an exact constant, so this does not prove that no data
arrives. FM playback is stereo, so the right PGA works. What remains is
a step outside these helpers, or an SC6530C codec with only one working
ADC (stock never powers the right one).

### Hardware sweep result (2026-11-06)

The key 0 sweep on the phone, with FM playing (`tuner=1`, variant 1):
- **Baseline:** left carries the station (about ±26000), right is a
  constant `-32768`.
- **VBC `+0x80` bit 0** makes the right channel an exact copy of the left
  (identical min/max). The AD1 FIFO and its DMA carry data, so the VBC
  side is not the problem. (Bits 1/3 and 4/5 silence AD1/AD0, as the
  DSP-routing bits do.)
- **Digital `0x8a002000` bit 3** (ADC R filter enable) cleared: right
  becomes a constant `-49`, its idle value. The digital filter runs and
  sees a modulator stuck at one rail.
- **No analog bit changes the right channel.** All 104 codec bits from
  `+0x40` to `+0x6c` and `+0xac`, stock's three mux values and variants
  0/2 leave it at exactly `-32768`, including its own power-down
  (`+0x5c` bit 3). The left's power-down (`+0x5c` bit 5) kills the left
  at once, and many bits audibly change it.

Conclusion: ADC R's analog modulator is absent or not connected on the
SC6530C. Stereo line-in capture is not possible on this path. The best
FM recording is stock's mono mix (mux `0xe0`: both FM channels into the
left PGA). A two-bit combination is not ruled out by a single-bit sweep,
but nothing analog even nudged the right channel.

## Still open

1. **Mic gain scale.** `+0x70` bits 7:4 changes the level, but its dB
   step is unknown, so the setting shows raw steps 0–15.
2. **The DMA field `0x50100140`** is derived rather than traced; it works
   on the phone.
3. **Bias settling.** The driver waits 20 ms after enabling the mic bias;
   stock waits 0 ms.
4. **Headset mic on hardware.** Detection works on the phone (a headset
   mic reads about 508 raw); see the dump's `MIC` line.
5. **Headset buttons on hardware.** The bands come from one headset's
   readings; other headsets may need different ones. Whether the centre
   button also raises EIC channel 4 on the phone is not confirmed (the
   ADC alone classifies it).
