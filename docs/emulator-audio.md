# Stock boot and audio bring-up — 2026-10-06

The ARM audio path now produces verified PCM through QEMU's audio backend:
CPU-filled ping-pong banks and DMA-fed stereo playback both work. The stock
firmware gets through flash initialization, compressed code paging and DSP
download, starts its ThreadX tasks, and reaches the stock "Time and date"
setup prompt in a 90-second capture.
With the explicit `gpio49-high=on` board experiment, the unmodified stock OS
also reaches the home screen, settings and default ringtone picker. Input49
is polled by its LCD sleep/wake wrapper; the electrical purpose of that
signal is still unconfirmed, so the option defaults off. Built-in stock
ringtone playback now produces stereo WAV output through the real firmware's
MIDI renderer, PCM conversion and DMA queues. The first successful capture
contains 341018 frames at 44.1 kHz (about 7.7 seconds), 661461 nonzero samples,
peak 19106 and no clipped samples. The firmware image is unpatched.
Full phone operation, exact sound fidelity and DSP-controlled audio remain
unfinished. Earlier silence captures exposed missing MIDI synthesis, incorrect
DMA progress reporting and an incorrect mute-enable interpretation.

The current model also fixes short one-shot timer polling, composes the
stock RGB565 image/OSD layers, handles RAM capture and retains partial panel
updates. In a controlled comparison, the initial SM-B310E screen remained
visible for about 20.1 seconds before the timer fix and 3.0 seconds after it.
The time/date prompt appeared at about 19 seconds in the updated capture.
A 150-second navigation/idle run remained responsive, including entering
digits in Contacts and waking the locked keypad after an idle interval.
This does not establish that every stock freeze is resolved.

## Inputs and provenance

This work uses `D:\floppy\phonefirmware\e52q7a.bin`, an 8 MiB NOR image:

```text
SHA256 5e44e0858d4eacbd1c0124d3bb8d152cb429c9222a99b42f8f75c86300846c92
```

The user-supplied reference archive is `mocor-zw217.7z` in the same folder.
Reference files are extracted outside this repository. Its other CPU variants
help identify register families, but the stock ARM code determines this
machine's addresses, bit fields and interrupt numbers. No proprietary source
or stock binary has been added to the repository.

The LZMA model is an independently written decoder using the LZMA1 algorithm
described by the public-domain LZMA SDK. The synthetic test stream encodes our
own text. Reference SDK encoder/decoder experiments stay outside the repo.

The newer reference MIDI library's debug types match the stock 96-byte
`SCI_CTRLBLK` voice records exactly. They identify fields; the implementation
is independently written and contains no vendor code or soundbank. Pitch,
envelope and interpolation conventions were cross-checked against the public
[Android Sonivox wavetable engine](https://android.googlesource.com/platform/external/sonivox/+/9337d6bf914df7eabe767ec7f9337224393a7ce0/arm-wt-22k/lib_src/eas_wtsynth.c)
and its interpolation engine. The Q15 calculations and metadata are useful
evidence for behavior, but are not measurements of the accelerator's rounding.

## Windows headless build

QEMU is pinned to v11.1.0, commit
`84f07211cc5b4fc6a371559bf8a5de4fb068e648`. The local source/build is
`D:\floppy\.tools\qemu-b310e-src`; the executable is
`build\qemu-system-arm.exe`. MSYS2 MINGW64 supplies the runtime DLLs.

With MSYS2 installed at `C:\msys64`, install these build dependencies from
its MINGW64 shell if they are missing:

```sh
pacman -S --needed base-devel git mingw-w64-x86_64-gcc mingw-w64-x86_64-glib2 mingw-w64-x86_64-pixman mingw-w64-x86_64-pkgconf mingw-w64-x86_64-ninja mingw-w64-x86_64-python mingw-w64-x86_64-python-setuptools mingw-w64-x86_64-python-wheel mingw-w64-x86_64-dtc mingw-w64-x86_64-libpng
```

From the repository in PowerShell:

```powershell
& tools/qemu-b310e/scripts/build-local.ps1 -QemuSrc D:/floppy/.tools/qemu-b310e-src
```

This clones QEMU if the destination is absent, verifies the pin, installs the
machine and builds ARM with PNG and no GUI dependency. Configure uses the
bundled Python wheels with downloads disabled. It does not reset an
existing checkout. The optional install-bundle helper skips Windows symlink
permission failures; this build is for running locally, not `make install`.
For an already configured tree, `-SkipConfigure` rebuilds only changed files.
Logs are `b310e-configure.log` and `b310e-build.log` inside the QEMU checkout.

## Reproduce the checks

The tests launch headless QEMU under QTest, exercise the actual MMIO devices,
advance virtual time and inspect captured WAV samples. They use synthetic
RAM/NOR data. Only the optional LZMA stock check reads the phone dump.

```powershell
$python = 'C:/msys64/mingw64/bin/python.exe'
$qemu = 'D:/floppy/.tools/qemu-b310e-src/build/qemu-system-arm.exe'
& $python tools/qemu-b310e/scripts/test-audio.py --qemu $qemu --output tools/qemu-b310e/logs/tests-audio
& $python tools/qemu-b310e/scripts/test-midi.py --qemu $qemu --output tools/qemu-b310e/logs/tests-midi
& $python tools/qemu-b310e/scripts/test-bringup.py --qemu $qemu --output tools/qemu-b310e/logs/tests-bringup
& $python tools/qemu-b310e/scripts/test-display.py --qemu $qemu --output tools/qemu-b310e/logs/tests-display
& $python tools/qemu-b310e/scripts/test-lzma.py --qemu $qemu --output tools/qemu-b310e/logs/tests-lzma --firmware D:/floppy/phonefirmware/e52q7a.bin
```

Verified checks:

- CPU playback: 800 stereo frames at 8 kHz, 4800 at 48 kHz, correct signs and
  amplitudes, bank switching including byte reads, digital mute, DSP ownership.
- DMA: software memory copy, 512 exact stereo frames, source progress,
  completion, auto-disable, IRQ20 masking/acknowledgement and reset. The
  stock standard-channel halfword FIFO-fill encoding preserves VBC controls
  and produces 160 exact samples. Standard and full channels each produce
  512 exact stereo frames; the standard remaining-byte counter is checked
  during playback, not only after completion. Unset/wrong hardware request
  mappings stall; the correct DA0/DA1 mappings permit progress and reset
  clears them. Reserved channel gaps cannot index past the channel array.
- Flash: aligned JEDEC ID, WEL, byte TX writes, native halfwords, 1→0
  programming, erase, stale TX slot clearing and an unchanged input file.
- Battery conversion, RTC updates/acknowledgements and analog IRQ24, IRQ/FIQ
  masks, independent timer0/1 including frozen counts on stop, timer2
  one-shot/periodic, system alarm FIQ16 with no stale retrigger on ACK,
  LCDC completion IRQ14 and retained pixels after the guest reuses the DMA
  buffer, GPIO input/mask/direction/reset, keypad press/release IRQ8 and W1C, DSP download
  IRQ13, absent SD card and reset.
- Display: disabled/freed image source, OSD2, source pitch/crop, partial
  CASET/RASET windows, layer placement, color key, block alpha, direct DBI
  bytes and RGB565 RAM capture followed by display. Panel reset clears pixels.
- The complete Rockbox application mounts a generated FAT16 image and plays
  its stereo test track through the WAV codec; see [rockbox-audio.md](rockbox-audio.md).
  Stock also boots with an attached copy of the image and performs CMD17/18
  reads and CMD24/25 writes before reaching its time/date prompt.
- LZMA: exact synthetic output, slices/intermediate buffers, IRQ30/W1C,
  truncated input, buffer bounds, source mapping bounds and reset.
- MIDI: synthetic RAM/flash samples, 8/16-bit stereo/mono output, two linked
  voices, an octave of pitch change with interpolation, attack/release state,
  loop/end state, completion/W1C/reset and bounded malformed guest records.
  Clearing MIDI or LZMA preserves the other device's pending IRQ30.

Two real compressed streams match an independent SDK decoder experiment:

| Stock source | Output | SHA256 |
|---|---:|---|
| NOR `0x1a216c`, 2484 bytes | 4096-byte code page | `71082b67a8156ec3bd3cfc02a14325e6747c373ac955ff4e7a84c2c21033a3a7` |
| NOR `0xcc874`, 65300 bytes | 267484-byte DSP image | `4bfbfaed86b6c6e66addfa6b4877af42ac5629d866b65720abd733134fc98294` |

These are decoder and device checks, not proof that the stock OS plays audio.
The separate stock ringtone capture below provides that integration evidence.

## Capture the stock OS

```powershell
& $python tools/qemu-b310e/scripts/capture-stock.py --qemu $qemu --firmware D:/floppy/phonefirmware/e52q7a.bin --output tools/qemu-b310e/logs/stock-current --no-overlays --hold-end --audio --seconds 90
& $python tools/qemu-b310e/scripts/capture-asserts.py --qemu $qemu --firmware D:/floppy/phonefirmware/e52q7a.bin --output tools/qemu-b310e/logs/stock-asserts --no-overlays --hold-end --count 1 --timeout 120
& $python tools/qemu-b310e/scripts/capture-ringtone.py --qemu $qemu --firmware D:/floppy/phonefirmware/e52q7a.bin --output tools/qemu-b310e/logs/stock-ringtone
```

The first saves `screen.png`, `state.json`, PSRAM, traces and `audio.wav`.
The second breaks at both fatal SCI entries (`0x11172`, `0x11414`) and records
their arguments and stack. A timeout with zero hits means no observed SCI
assertion during that interval; it is not a boot-success result. Custom
`--break-address` values and `--instructions` are available for investigation.
`--hold-end` presses the power button at reset; the keypad model releases it
after two virtual seconds. Captures and test output are Git-ignored.
To record navigation attempts, repeat timed key arguments, for example
`--seconds 100 --key 65:f2 --key 68:f1`. Each key gets a screenshot one
second later. These times are wall time and depend on the host's speed.
Add `--gpio49-high` for the experimental external display input. This changes
an input pin through the GPIO device, rather than writing a guest RAM flag.
The stock keypad lock is released by holding the asterisk key for two seconds.
For example, `--key 68:asterisk:2000` holds it long enough; the default hold
for a key argument is 100 ms.

`capture-ringtone.py` applies that GPIO experiment, boots for 90 wall seconds,
uses the stock keypad to open Settings → Profiles → Normal → Edit → Call
ringtone → Default ringtones, and records the preview. It checks for sustained
nonzero stereo output and writes `audio-stats.json`. The per-key screenshots
make navigation failures reviewable. Increase `--boot-seconds` on a slower
host; `--play-seconds` controls the final preview duration (15 by default).
This recipe is specific to the dump hash above and uses no guest RAM patches.
The updated fresh-boot run passed with `--boot-seconds 36 --play-seconds 15`:
657222 stereo frames at 44.1 kHz, 1311560 nonzero samples, peak17325 and no
clipped samples. Ringtone icons and selection controls render correctly.
Host wall time and emulated audio duration differ when the firmware consumes
CPU time. Evidence is in the ignored `logs/stock-ringtone-lcd-timer` directory.

The NOR input opens read-only. Flash writes affect QEMU's private memory copy
so stock initialization can program its filesystem without modifying the dump.
They do not persist across emulator runs.

## What changed and why

Earlier RAM overlays replaced live RTOS structures and caused `sci_mem.c:313`.
They are opt-in now; the stock OS initializes its own data normally. Its flash
descriptor names JEDEC `C8 60 17`, returned in the receive shifter's high bytes.
The stock SPI driver fills TX words byte by byte, sends native halfwords,
uses WEL, and clears TX enables between transactions. Keeping stale enables
after a long program corrupted the next word and caused `spiflash.c:2277`.

The battery ADC channel is selected by the low four bits, including the stock
value `0x35`. Channel5 returns 900 by default (about 3978 mV using the dump's
calibration); the previous zero result made the OS shut itself down.

Stock compressed pages use 11-bit probabilities, separate match/repeat length
tables and plain literals after matches. Ordinary liblzma cannot decode this
variant. DSP loading also sets SRC_LEN=0; the model bounds this by the actual
RAM/ROM mapping, including the ROMD region size.

DSP COPY_DONE needs IRQ13 and the interrupt controller's masked status at +0.
The enable register must read back for the stock read/OR/write pattern. The
stock scheduler uses timer0/1 and the system alarm via FIQ; flash delays use
timer2 in one-shot mode. RTC counters start at ANA +0x600, not +0x620 (which
contains alarms). Update ACKs go through the analog interrupt controller.
Display completion similarly needs IRQ14 to wake the stock LCD task.
Display pixels persist after the DMA source buffer is freed. Reading live
guest memory for every screenshot previously displayed unrelated allocations.
The refresh START strobe clears after completion, so a later controller
enable does not transfer that freed buffer again. Partial panel windows
now use the panel's CASET/RASET/RAMWR cursor. Enabled image/OSD layers use
their source pitch, position and LCM crop. The stock also composes RGB565
into RAM through CAP +0xe0..+0xf0, then displays that allocation as OSD2
with IMG disabled. Ignoring capture or reading the disabled image's freed
source caused missing icons and textbox corruption. Unrotated RGB565 is
verified; other formats and rotation are logged as unsupported. Panel
initialization, scaling and blend precision remain partial.
Stopping timer0/1 preserves the current count: stock stops before measuring
elapsed time. The system alarm compares an absolute 32-bit millisecond
deadline; acknowledging it must not turn a past deadline into a 1 ms loop.
The keypad matrix raises IRQ8 for press/release latches. Stock's key table at
NOR `0xc6e70` indexes `column * 8 + row`, with positions matching the host
key map (F1/F2 soft keys, Return centre, arrows and digits).

Stock PSRAM `0x040109ba` polls timer2's 780-count one-shot for a 30 µs SPI
delay. Waiting only for the host's timer callback stretches thousands of
these delays during boot. Status reads now observe the real 26 MHz virtual
countdown: when it reaches zero, they latch completion and cancel the delayed
callback. No earlier completion or guest clock jump is introduced. Tests
check pending remains clear at 29 µs, sets at 31 µs, and stays clear after ACK.

The ARM LCD wrapper at `0x15520` repeatedly reads GPIO49 and re-enters the
panel sleep/wake callback while it reads low. GPIO49 maps to DATA
`0x8a000180` bit1, with mask at +4 and direction at +8. Raising that external
input clears the loop and permits UI navigation. This is an observed wait
and a controlled input experiment, not identification of the physical signal
or proof of an accurate panel model. It is not labeled as TE or ESD without
schematic or hardware evidence.

Stock ringtone initialization uses DMA standard channels with widths in
fragment bits31:30/29:28 and address-fix bits20/21 (`0x50300140` fills a
160-sample halfword FIFO). Full channels use widths27:26/25:24 and explicit
steps. Treating standard channels as the older SC6530 encoding overwrote
VBC control registers. The live DMA ISR at `0x324b2` also reads masked status
at `0x20100010`; raw status is modeled at +0x0c. Returning zero at +0x10
left the CPU servicing IRQ20 without acknowledging the completed channels.
The SC6530C standard block register also reports a 17-bit remaining-byte
count, rather than the older chip's fixed low-half length/high-half progress.
Stock `0x32e3c` subtracts successive reads to determine consumption. A fixed
length left the MIDI PCM producer sleeping in `0x83f94` with full output
queues while the DAC DMA repeatedly received zeros. The new model decreases
the counter with each transfer and retains its original block length
internally.

Codec DAC_CTL +0xc bit15 enables its mute ramp controller; bit14 is the mute
request. The stock playing value `0x8005` therefore means 22.05 kHz with the
controller enabled and mute released. Treating bit15 alone as mute silenced
valid DMA samples. The playback model now requires both bits for mute; tests
exercise enabled/unmuted and enabled/muted states. Analog ramps and their
completion interrupts still need a detailed model.

The stock ringtone renderer calls the hardware wavetable block at
`0x20b00000` (ARM driver `0xc00f8`). Returning zero from this entire region
previously allowed it to finish without producing any mixer samples. The
device now reads linked 96-byte records, interpolates instrument data from
NOR or RAM, mixes on the 32-bit bus, and updates pitch/envelope/voice state.
Stock code subsequently converts this bus to signed 16-bit PCM and queues it
for VBC DMA. IRQ30 is shared with the LZMA accelerator through an OR gate;
one accelerator's acknowledgement must not deassert the other.

Important record fields, identified in the stock packer `0xad9f4..0xadb6c`:

| Record offset | Meaning |
|---|---|
| +0/+4 | voice done byte / next record address (-1 ends the list) |
| +8 | signed 16-bit instrument gain / tuning in cents |
| +0xc | note, velocity, voice flags, voice state (one byte each) |
| +0x10/+0x14 | previous gain, channel gain / channel pitch |
| +0x1c/+0x20/+0x24 | envelope states / EG1 and EG2 values and increments |
| +0x28/+0x2c | loop start / end sample addresses |
| +0x30/+0x34 | current sample address / Q15 fractional phase |
| +0x38 | signed 16-bit right / left pan gains |
| +0x3c/+0x40 | LFO state / filter state |
| +0x44..+0x54 | filter, LFO, pitch and envelope articulation settings |

The first MIDI integration attempt rejected NOR reads because the dump is
mapped as a ROMD device, rather than plain RAM. Read-only ROMD samples are
now accepted and tested; voice/output writes still require writable RAM.

## Audio register contract and remaining work

Rockbox's implementation and stock speaker/headset register measurements
are documented in [rockbox-audio.md](rockbox-audio.md). Digital EIC0 now
models the active-low headset input instead of returning zero unconditionally.
An empty jack is the default; `capture-ringtone.py --headset` reproduces the
inserted route. The default capture handles the unplugged UI's extra preview
confirmation. EIC hotplug/debounce IRQs remain unmodeled.

The VBC sample timer is anchored to its prior sample deadline, avoiding a
stream slowdown from accumulated host callback latency. On stop, PCM that
already left the DAC drains to the host backend before output deactivates.

| Block | Implemented contract |
|---|---|
| DMA `0x20100000` | 32 channels at +0x1000, stride0x40; stock width/length/step fields; IRQ20 |
| DMA request map | +0x2000, stride4; one-based request15/16 select one-based channel4/3 for DA0/DA1 |
| SDIO `0x20700000` | SDHCI command/data/card model; IRQ28; slot-status alias +0x1fc; optional backend `id=sdcard` |
| MIDI `0x20b00000` | +0 start/busy; +4 stereo/8-or-16-bit/block/polyphony config; +8 IRQ control; +0xc result; +0x10 voice list; +0x14 32-bit mixer output; shared IRQ30 |
| VBC `0x82003000` | left/right DAC ports +0/+4; size−1 at +0x10 bits15:8; two banks of up to160 frames |
| VBC control +0x18 | bank bit9, RAM access bit10, DAC DMA bits13/14, playback bit15 |
| Digital codec `0x8a002000` | left/right gates at +0 bits0/2; sample mode +0x0c bits3:0; mute controller bit15 / request bit14 |
| DSP `0x8b0001c4` | bit2 gives ARM access to VBC; DSP-owned playback produces silence |

Sample modes0..10 are 96000, 48000, 44100, 32000, 24000, 22050, 16000, 12000,
11025, 9600, 8000 Hz. Samples are signed16-bit little-endian stereo. A QEMU
audio device can capture WAV or use an available host playback backend.

The DSP device still models a download handshake, not DSP instructions or the
runtime audio/modem firmware. No claim is made that DSP-owned sound works.
Remaining work includes tracing the stock startup/runtime DSP messages,
implementing the DSP/audio execution path, and measuring fidelity against a
physical phone. Built-in MIDI playback is verified; DSP-owned sound is not.
The wavetable model does not yet apply the voice filter, and its exponential
pitch/gain math, noise generator, boundary interpolation and completion timing
are not bit-exact hardware claims. Recording, analog gain/PA behaviour, DMA
linked lists/wrapping/swapping, other hardware DMA requests, high-speed
timers3..5, modem operation and complete migration are unmodeled.
