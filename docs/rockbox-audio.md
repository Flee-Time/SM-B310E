# B310E Rockbox playback from stock measurements

The port implements ARM-owned stereo DMA playback into the on-die DAC,
software volume, mute, stop/restart, and the stock speaker/headset output
sequences. Rockbox's software codecs and DSP produce PCM; this route does
not need the vendor DSP firmware or hardware MIDI renderer.

This is playback support, not a claim that the entire phone audio system
has been implemented. Microphone recording, telephony, Bluetooth/FM routes,
analog ramp IRQs and physical output fidelity have not been validated.
QEMU captures digital PCM before the analog mixer, gain stages and amplifier.
Register assertions verify their programming, not their electrical behavior.

## Evidence and corrected assumptions

The input is the unmodified 8 MiB `e52q7a.bin`:
SHA256 `5e44e0858d4eacbd1c0124d3bb8d152cb429c9222a99b42f8f75c86300846c92`.
Stock boot uses `boot-overlays=off`; the explicit experimental GPIO49-high
input allows LCD navigation. Its physical board purpose remains unconfirmed.
Keypad events open the stock ringtone preview; no guest RAM or NOR patches
are used. The reference archive is used for register semantics only; no
vendor code, sound bank, firmware dump or reference archive is included.

| Observation | Stock evidence | Port consequence |
|---|---|---|
| Analog codec base is `0x82001a00` | Live pointer at RAM `0x042354b0` | Replaces the old `0x82001280` assumption |
| Rail controls are regulator IDs | NOR `0x358d6`, table `0xca38c`, ladder `0x80982` | Clear only audio PD bits `0x1f0` at `0x82001164`; no guessed GPIO28..31 writes |
| DAC mode selects the sample clock | NOR `0x8123e`; stock playing `DAC_CTL=0x8005` | Ten Rockbox rates, 8–96 kHz; mode9 is 9.6 kHz, mode10 is 8 kHz |
| DAC_CTL bit15 enables mute control, bit14 requests mute | NOR `0x8131e`, playing trace | `0x8000` is unmuted; `0xc000` requests mute |
| APB +0x60/+0x64 are reset SET/CLEAR | NOR `0x81516` | Pulse only audio/VBC reset bits21/18; preserve keypad/EIC |
| INTC +8 is a full R/W enable mask | NOR `0x1b920` and device tests | Audio uses read/OR/write to preserve timer/other enables |
| Standard DMA channels use different fields from the older controller | Stock DMA trace, consumption at NOR `0x32e3c` | Halfword, fixed DAC destination, 320-byte bank, IRQ20 |
| Headset detection is digital EIC0, active low | Product GPIO ID17 in RAM table `0x0423c8dc`; NOR EIC table `0xca92c` | Poll `0x8a001000` bit0 after unmasking +4; separate from analog END at `0x82001900` |
| Headset output enable is physical GPIO0 | Product ID34 at `0x0423cafc`; stock GPIO0 rises during headset preview | Preserve other pins while setting mask/direction and output |
| Speaker PA is controlled internally | Product ID33 callback `0x24d24` → `0x69d00` | Apply the captured codec PA sequence; no GPIO18/39 probing |

The previous emulator returned zero for digital EIC0 and therefore showed
the stock headset icon. Explicit inserted/unplugged captures now distinguish
the two routes. QEMU defaults to an empty jack; use
`-global sc6530_aux.headset-present=on` for an inserted headset. Its named
`eic-input` pin0 also accepts an active-low external level for polling tests.
EIC debounce and hotplug interrupts are not modeled.

## Playback and output contract

| Block | Registers and fields |
|---|---|
| DMA | `0x20100000`; channels at +0x1000, stride0x40; zero-based left3/right2 |
| DMA channel | CFG+8=`0x3001`; source+0x10; destination+0x14; FRAG+0x18=`0x50300140`; BLOCK+0x1c=320 |
| DMA IRQ | Channel +0xc: enable bits4:0, raw12:8, masked20:16, W1C28:24; block bit1, error bit4; global masked +0x10 |
| VBC | `0x82003000` left +0/right +4; size−1 at +0x10 bits15:8; 160-frame banks |
| VBC control | +0x18: bank9, CPU access10, DAC DMA13/14, enable15 |
| Ownership | `0x8b0001c4`: ARM access2, DA clock5/6, analog8, codec control9/10 |
| Digital DAC | `0x8a002000`: left/right gates0/2; +0xc mode3:0 and mute15/14 |
| Codec power | Analog base+0x40=`0xfa` after ordered bandgap/bias/VCM/buffer/VB/output-bias ladder |

DAC modes0..10 are 96000, 48000, 44100, 32000, 24000, 22050, 16000,
12000, 11025, 9600, 8000 Hz. Rockbox exposes all except nonstandard 9600.
`audiohw_set_frequency` receives a Rockbox index, not Hz. Reinitialization
retains the selected frequency. Samples are signed 16-bit stereo; the sink
deinterleaves arbitrary core buffers into two 32-byte-aligned 160-frame
planes and commits the cache before arming DMA.

Both channel completions must arrive before the planes are reused. Nested
PCM locks mask only IRQ20; the dispatcher reads masked INTC status. End of
stream zero-pads the final partial bank, drains it and then stops/mutes.
Explicit stop is immediate. DMA errors invoke the core's error callback and
disable both channels. QEMU retains PCM that already left its emulated DAC
until the host backend drains it, and anchors timing to the sample clock
instead of accumulating host callback delays.

Both analog routes share DAC enable, SDM setup and the power ladder. The
following values are captured from stock while playing (offsets from the
live analog base):

| Register | Headset inserted | Jack empty / speaker |
|---|---:|---:|
| PMUR2 +0x44 | `0x00` | `0x88` |
| DAOCR2 +0x7c | `0x84` (separate L/R) | `0x33` (both channels to SP+/SP−) |
| DCR1 +0x84 | `0xc4` | `0x10` |
| DCR2 +0x88 | `0x00` | `0x30` |
| DCR3 +0x8c | `0x00` | `0x80` |
| Headphone gain +0x94 | `0x44` | `0x00` |
| PA gain +0x9c | `0x00` | `0x70` |
| GPIO0 output | High | Low |

The port mutes while changing routes, disables the previous output, then
enables the new one and restores mute state. Rockbox's speaker setting
defaults to Auto; its existing jack debounce/events select speaker or
headphones. On explicitly selects speaker; Off selects the headset path.
Software volume scales PCM once; analog gains stay at the stock values.
Close mutes, stops VBC, disables output gates/PA and powers down the audio
rails while preserving unrelated pins, regulators and power-button clocks.

## Reproduce the build and tests

`tools/rockbox-port/build.sh` pins official Rockbox to
`ecdeb02dda6dbb94c1c3b01b8406203eda225f9f` under `build/rockbox`.
It refuses a different existing HEAD without resetting files. The build
requires the ARM bare-metal toolchain, a host C compiler, make, Perl and zip.
On this Windows workspace:

```powershell
$env:B310E_TOOLCHAIN='/d/floppy/.tools/arm-toolchain/bin'
$env:B310E_HOST_CC='/c/msys64/mingw64/bin'
& tools/rockbox-port/build.ps1

& C:/msys64/mingw64/bin/python.exe tools/rockbox-port/tests/test-audio.py `
  --qemu D:/floppy/.tools/qemu-b310e-src/build/qemu-system-arm.exe `
  --toolchain D:/floppy/.tools/arm-toolchain/bin `
  --output tools/qemu-b310e/logs/rockbox-audio-final
```

The build stages `sdcard/progs/rockbox.bin` and the `.rockbox` runtime tree
(software codecs, languages, themes and fonts). Use the existing loader to
launch the raw image. The real ARM test links unchanged upstream `pcm.c`,
`pcm_sw_volume.c`, `pcm_sampr.c` and the generic `audio_path.c` speaker/jack
policy with the actual target startup/MMU, IRQ dispatcher, codec and DMA
driver. Only outer scheduler/application
hooks are replaced. Fixed instruction-count timing and an idle/tick loop
avoid host scheduling affecting the gap checks.

All 23 cases pass: ten rates with 4093 exact contiguous stereo frames;
−6 dB and software mute; stop/restart; DMA error; nested lock; headset
playback; both route transitions during playback; codec close/reinit;
and 1/13/159/160-frame clips. Register assertions check the captured routes,
power-off state, and preservation of another GPIO, nonaudio regulators,
analog END EIC and its clock controls. WAVs, traces, commands and
`results.json` remain in the ignored output directory.

Stock regression, with no guest patches:

```powershell
& C:/msys64/mingw64/bin/python.exe tools/qemu-b310e/scripts/capture-ringtone.py `
  --qemu D:/floppy/.tools/qemu-b310e-src/build/qemu-system-arm.exe `
  --firmware D:/floppy/phonefirmware/e52q7a.bin `
  --output tools/qemu-b310e/logs/stock-speaker-route --boot-seconds 65 --play-seconds 10
# Repeat with --headset and a separate output folder for the headset path.
```

The speaker capture contains 436842 frames at 44.1 kHz, 870982 nonzero
samples, peak17325 and zero clipped samples. The headset capture contains
435782 frames, 840702 nonzero samples, peak19106 and zero clipped samples.
Capture durations vary with host load. Device audio, MIDI, bring-up and
LZMA regression suites also pass, including independent stock DSP/page
decompression hashes.

## Validation boundary

The full Rockbox image builds and reaches its storage initialization in
QEMU. The current SD device models an absent card, so it reports
"No partition found"; playing a filesystem music file through the complete
Rockbox UI still needs SD emulation or a physical phone. The ARM audio tests
exercise the real core/driver directly and do not substitute for that test.
QEMU does not emulate analog gain, PA electrical behavior or DAC ramps.
Speaker and jack listening tests, maximum safe gain, pop suppression and
hardware timing remain to be measured on the phone. No microphone/recording
capability is advertised by this port. Vendor DSP execution is unimplemented.
