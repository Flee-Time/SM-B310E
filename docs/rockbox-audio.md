# B310E Rockbox playback from stock measurements

The port implements ARM-owned stereo DMA playback into the on-die DAC,
software volume, mute, stop/restart, and the stock speaker/headset output
sequences. Rockbox decodes and processes PCM on the ARM CPU and selects
ARM ownership of VBC. The phone owner confirmed audible playback through
both headphones and the speaker after the analog-clock fix (6 October 2026).
This playback route works on the phone without a vendor DSP firmware loader.

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
| Analog audio needs a separate clock enable | NOR `0x81562`; live ADI caller `0x81587` writes `0x82001440=4` | Enable it before codec reset; later SET writes of 1/2 preserve bit2 |
| INTC +8 is a full R/W enable mask | NOR `0x1b920` and device tests | Audio uses read/OR/write to preserve timer/other enables |
| Standard DMA channels use different fields from the older controller | Stock DMA trace, consumption at NOR `0x32e3c` | Halfword, fixed DAC destination, 320-byte bank, IRQ20 |
| Hardware requests must be routed to DMA channels | NOR `0xa7b96`; stock writes `0x20102038=4`, `0x2010203c=3` | Map DA0/DA1 requests15/16 to one-based channels4/3 before playback |
| Headset detection is digital EIC0, active low | Product GPIO ID17 in RAM table `0x0423c8dc`; NOR EIC table `0xca92c` | Enable both digital EIC clocks, then poll `0x8a001000` bit0 after unmasking +4; separate from analog END at `0x82001900` |
| Digital EIC needs APB and RTC clocks | `EIC_D` device descriptor at NOR `0xc9b54`, open/close callbacks `0x673c0/0x673da` | Write bits25/26 to APB clock SET `0x8b0000a0`; keep them enabled for jack polling when audio closes |
| Headphone gain has two independent nibbles | Stock setter `0x80a70` passes `0x0f/0xf0`, shifts0/4 to `0x807ea`, using live codec +0x94 | Code4 is the captured ringtone level, not the codec maximum; use equal gains for stereo |
| Headset output enable is physical GPIO0 | Product ID34 at `0x0423cafc`; stock GPIO0 rises during headset preview | Preserve other pins while setting mask/direction and output |
| Speaker PA is controlled internally | Product ID33 callback `0x24d24` → `0x69d00` | Apply the captured codec PA sequence; no GPIO18/39 probing |

The previous emulator returned zero for digital EIC0 and therefore showed
the stock headset icon. Explicit inserted/unplugged captures now distinguish
the two routes. QEMU defaults to an empty jack; use
`-global sc6530_aux.headset-present=on` for an inserted headset. Its named
`eic-input` pin0 also accepts an active-low external level for polling tests.
The input mask alone is insufficient: both APB clock bits25/26 must be on.
Clock SET/CLEAR aliases accumulate into status `0x8b0000a8`; a disabled
sampling path reads zero in QEMU. This catches the port's missing clock
setup, which otherwise falsely indicates insertion. Rockbox's SD-loader
environment enables the analog END-key EIC but omits these digital clocks.
The correction is tested in QEMU; physical jack switching needs a phone test.
EIC debounce and hotplug interrupts are not modeled; polled level changes are.

## Playback and output contract

| Block | Registers and fields |
|---|---|
| DMA | `0x20100000`; channels at +0x1000, stride0x40; zero-based left3/right2 |
| DMA request map | +0x2000, one register per one-based request; request15/+0x2038=4, request16/+0x203c=3 |
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
| Headphone gain +0x94 | `0x44` at 0 dB; up to `0xcc` at +24 dB | `0x00` |
| PA gain +0x9c | `0x00` | `0x70` |
| GPIO0 output | High | Low |

The port mutes while changing routes, disables the previous output, then
enables the new one and restores mute state. Rockbox's speaker setting
defaults to Auto; its existing jack debounce/events select speaker or
headphones. On explicitly selects speaker; Off selects the headset path.
Software volume scales PCM once. At settings of 0 dB or below, analog gains
stay at the stock values and the existing loudness/default remain unchanged.
The target `audiohw_set_volume` hook sets the software PCM master factors
using the tenths-of-a-dB value supplied by Rockbox. A no-op here leaves those
factors at their initial zero, even when the DMA engine advances normally.
The minimum setting, −100 dB, requests exact software mute.

The headphone maximum now extends to +24 dB relative to the old maximum.
The reference codec gain definitions describe codes1..15 as −33..+9 dB
in 3 dB steps (code0 mutes); their register fields match the stock setter.
The port uses only codes4..12, ending at codec unity instead of boosting
the digital samples or changing PA/bias/current-limit controls. For each
positive setting, it rounds analog gain upward to a 3 dB step and applies
the remaining attenuation in software:

| Rockbox setting | Headphone register | PCM attenuation |
|---|---:|---:|
| 0 dB | `0x44` | 0 dB |
| +1 dB | `0x55` | −2 dB |
| +3 dB | `0x55` | 0 dB |
| +12 dB | `0x88` | 0 dB |
| +24 dB | `0xcc` | 0 dB |

Values are bounded to −100..+24 dB. Route changes and codec reinitialization
restore the current volume/gain. The speaker retains its captured PA gain
`0x70`; positive settings saturate at its previous maximum. The electrical
gain, distortion and noise at the new headphone levels need measurement on
the phone: QEMU captures PCM before the analog gain stage.

An existing `volume limit: 0` saved setting still limits volume to 0 dB.
Raise **Sound Settings → Maximum Volume Limit** to +24 dB to unlock the
additional range, then increase playback volume above 0 dB as needed.
Close mutes, stops VBC, disables output gates/PA and powers down the audio
rails while preserving unrelated pins, regulators and power-button clocks.

The phone subsequently reported an advancing timer with both outputs silent.
Its log shows the expected request mappings, DMA configuration and unmuted
DAC control, but those digital registers do not establish analog readiness.
Rockbox omitted the analog clock SET write at `0x82001440=4`. Stock helper
`0x81562` pairs it with APB SET `0x8b0000a0=1<<28`; disabling uses analog
CLEAR `0x82001444=4` and APB CLEAR `0x8b0000a4=1<<28`. Separate stock helper
`0x8153e` writes 1 and 2 to the same SET/CLEAR aliases for the DAC paths.
Recording only the final SET value (2) had hidden the earlier clock enable.
The new caller trace reads the return address saved by stock's shared ADI
write helper at `SP+20`; it does not modify stock RAM or NOR.

The port now enables this clock and disables the audio clock/DAC paths on
close. It removes regulator/interface writes inferred from unused NOR
helpers `0x6a14c/0x6a2a4`; they were absent from the running chip's audio
calls. QEMU gates audible samples on the analog clock while DMA/bank
progress remains independent. A regression case clears the clock, checks
normal source progress/completion/IRQ clearing, and verifies a silent WAV.

The reported ADI FIFO values `0x3150`, `0x31a0` and `0x31f0` all have FIFO
empty bit8 set and full bit9 clear. Variation in other bits does not itself
show a stuck FIFO. The driver waits only on the proven empty/full flags.

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

The ARM suite exercises the target's actual volume hook: ten rates with 4093 exact contiguous stereo frames;
−6 dB and software mute; stop/restart; DMA error; nested lock; headset
playback; both route transitions during playback; codec close/reinit;
and 1/13/159/160-frame clips. Register assertions check the captured routes,
power-off state, and preservation of another GPIO, nonaudio regulators,
analog END EIC and its clock controls. WAVs, traces, commands and
`results.json` remain in the ignored output directory.
All 39 cases pass. Additional cases cover every extended analog gain step, the +1/+2/+4 dB
software residuals, upper-bound clamping, speaker saturation, route changes
and close/reinitialization at +24 dB. A host-driven hotplug case toggles only
the physical active-low EIC input while real ARM playback runs, then verifies
both Auto routes and uninterrupted DMA progress. Clock tests check separate
SET writes, byte accesses, missing-clock behavior, CLEAR and reset.
Both stock ringtone captures also pass with the clock-gated input model:
435926 headset frames (peak19106) and 436522 speaker frames (peak17325),
both at 44.1 kHz with no clipped samples. The caller trace records the
digital EIC SET writes at `0x673d2/0x673d6` and stereo headphone gain
updates at `0x80821/0x80845`, independently confirming the register paths.

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

## Complete player and SD image

QEMU now attaches an optional raw SD image through its SDHCI/card model.
The Rockbox driver restores SDHCI's CSD word order/CRC shift, selects byte
or block addressing from the OCR CCS bit, and requires data-transfer
completion with no error. CMD8 support alone does not make a small card
SDHC. FAT16 support is enabled for the generated 64 MiB card.

After building the port, create the image and run the full application:

```powershell
$python = 'C:/msys64/mingw64/bin/python.exe'
$qemu = 'D:/floppy/.tools/qemu-b310e-src/build/qemu-system-arm.exe'
& $python tools/rockbox-port/tests/make-sd-image.py --force
& $python tools/qemu-b310e/scripts/capture-rockbox.py `
  --qemu $qemu --rockbox sdcard/progs/rockbox.bin `
  --sdcard sdcard/emulator-sd.img `
  --output tools/qemu-b310e/logs/rockbox-player --seconds 24 `
  --key 6:down --key 8:ret --verify-test-tone
```

The image contains `.rockbox`, `progs/rockbox.bin` and `/test.wav`, a
30-second signed-16-bit stereo track at 44.1 kHz. Left is 440 Hz, right
is 660 Hz, with peak4096 and short fades. Its configuration selects the
file browser and 0 dB software volume. `--force` replaces the generated
image; no physical disk is accessed. Build/image outputs remain ignored.

The complete Rockbox file browser mounts the image, loads its WAV codec,
plays `/test.wav` and advances the elapsed-time display. A 24-second run
captured 688044 stereo frames, peak4096, with the expected 440/660 Hz
channels and less than 1% cross-channel tone leakage. The capture script
checks those tones and saves UI screenshots, register snapshots and traces.
This covers filesystem, codec, application, software volume, IRQ and DMA
integration in addition to the smaller ARM driver tests.

To attach a card in another QEMU launch, add
`-drive file=sdcard/emulator-sd.img,format=raw,if=none,id=sdcard`.
Card writes persist in that image. Omitting the backend models an absent card.

## Phone validation and DSP boundary

Copy the newly built `sdcard/progs/rockbox.bin` and `.rockbox` tree to the
phone's existing card layout and launch it through the existing loader.
Use a short track to check elapsed time, speaker output and headphones.
The phone now reports an advancing timer and audible sound through both
outputs. Test jack insertion/removal with **Speaker → Auto**, including
booting with the jack empty and with headphones already inserted. Test
the extended volume range on headphones; the speaker maximum is unchanged.

System → Debug → View HW info shows digital, analog and jack/gain pages;
Menu switches pages. Center saves all values to
`/.rockbox/audio-b310e.txt`; Back returns.
The log includes codec initialization/ADI failure state, route, software
volume, completed stereo banks and peak post-volume PCM per channel since
the last playback start. Nonzero peaks plus progressing banks distinguish
silent source/volume from an output-path problem. Analog values are read
through the bounded ADI mailbox and carry individual validity flags.
The jack/gain page and saved log include raw digital EIC data/mask, APB
clock status and the residual PCM volume. With clocks25/26 enabled and
mask bit0 set, data bit0 should be 1 with an empty jack and 0 when inserted.
It reads known control/status registers, avoiding VBC data ports and
unverified DSP shared-memory addresses. A log captured while a track is
stalled helps distinguish missing DMA progress from a codec/output issue.
Opening this screen and saving its log through the complete application
was verified; the saved text was read back from the FAT32 image afterward.
The complete player was also booted at +24 dB with a virtual headset:
its log reports `volume_tenth_db=240`, `pcm_volume_tenth_db=0`, headphone
gain `0x00cc`, valid analog reads and nonzero PCM peaks. Changing only
the external jack level causes the application's existing debounce/event
policy to switch automatically to the speaker and back, restoring `0xcc`.

The supplied `dsp/Untitled.png` shows a boot-ROM READY response and a
completed 66-block download, followed by no runtime message response.
Changing shared words alone does not identify a valid DSP service reply.
The stock download also wraps its 16-bit block offset after 64K words, so
that wrap in the old diagnostic is not sufficient evidence of a bug.
QEMU acknowledges the download and selected runtime status exchanges;
it does not copy those blocks into executable DSP program memory or run
TeakLite instructions. Its acknowledgements cannot validate the old
diagnostic's runtime handoff. The subsequent hardware listening report
establishes that this ARM-owned Rockbox playback route produces sound
without that loader; it does not validate DSP-controlled phone services.

QEMU does not emulate analog gain, PA electrical behavior or DAC ramps.
Jack hotplug, extended gain fidelity, pop suppression and hardware timing
remain to be measured on the phone. No microphone/recording
capability is advertised by this port. Vendor DSP execution is unimplemented.
