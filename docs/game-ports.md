# Game ports on the B310E

Build with `python scripts/build.py games` after installing the toolchains
described in [the build guide](build.md). All sixteen binaries are compiled
from pinned sources. The build prepares an isolated `build/game-ports/` tree
and stages its declared outputs in `sdcard/fpbin/`; cached release binaries
are never substituted. `ports/games/fetch.py` records the additional core pins.
The firmware dump, ROMs and commercial game data are supplied locally and are
not included in the repository or its releases.

## Card and controls

Use a FAT32 card with `fpbin/`, `progs/`, `.rockbox/` and `games/` at the root.
Build `fpmain` for the JSON launcher. Emulator ROMs are discovered in
`games/snes`, `games/gameboy` and `games/nes`; native ports have explicit
entries in `fpbin/config.json`. The [card data guide](../ports/fpmain/games-README.md)
lists the expected resources.

Hold **END + #** to increase volume or **END + \*** to decrease it, in 3 dB
steps. All ports share the B310E codec, stereo DMA output and headphone
detection. The headphone gain range is -60 to +24 dB. The speaker follows the
same hardware limits as the Rockbox driver. These chords consume both key
press and release, so they do not trigger game actions.

Emulators retain their upstream **END + 0** save control. SRAM is also saved
on normal exit. Turning off power before a save finishes can lose progress.
Native games use their own save menus. Saves and configuration files are
written to the selected game directory, so that directory must be writable.

Wolfenstein editions require matching binaries:

| Binary | Data edition |
| --- | --- |
| `wolf3d.bin` | Registered GoodTimes 1.4 (`.WL6`) |
| `wolf3d_apo.bin` | Registered Apogee 1.4 (`.WL6`) |
| `wolf3d_sw.bin` | Shareware 1.4 (`.WL1`) |
| `wolf3d_v11.bin` | Shareware 1.1 (`.WL1`) |

Blood requires **1.21** data, including its ART files, RFF files,
lookup tables and `BLOOD.INI`, as specified by [NBlood](https://github.com/NBlood/NBlood/blob/master/README.md).
The original 1.00 release lacks QAV weapon animations used by this core and
stops during initialization. Its disc image does not upgrade those resources.
Use the installed game files. The CD installer's loose `BLOOD.PAL` is a Windows
UI palette, not the game's palette; copying it into the game directory overrides
the palette in `BLOOD.RFF` and corrupts the colors.

## Audio coverage and limits

SNES runs the SPC700/APU and submits stereo PCM. NES mixes its five APU
channels, and Game Boy submits stereo PCM. Wolfenstein uses the integer
DOSBox OPL2 emulator for AdLib effects and IMF music, plus digitized VSWAP
effects. Doom, Heretic and Hexen play DMX effects and MUS/MIDI music. Duke,
Shadow Warrior and Blood connect their PCM effects and MIDI music to the
shared mixer. Retris has drop and line-clear tones.

The shared native-game mixer has eight voices and copies samples before
playback. Its WAV/VOC support currently covers unsigned 8-bit mono PCM,
not compressed ADPCM or arbitrary WAV formats. MUS and MIDI format 0/1
use an approximate integer wavetable synth, not a General MIDI sound bank.
Pitch bend, MIDI panning, reverb and full positional Build audio remain
unimplemented. This is audible music and effects support, not sound fidelity
equivalent to the original PC engines. CPU-heavy games still need phone
performance testing.

Build effects use a 96 KiB LRU cache, so repeated shots and menu sounds do
not reread their VOC/WAV files from the card. PCM and wavetable gains are
computed when controls change, leaving multiplication and shifts in the
sample loop. These two hot mixer files and Wolf's OPL generator compile as
optimized ARM code. File loading remains outside the DMA interrupt.

Wolf's fixed 22050 Hz OPL attack calibration is calculated on the host;
startup uses those exact values. OPL generation batches samples up to the
next 700 Hz IMF or 140 Hz effect event. SNES batches pending PCM until a DSP
read/write or 64 frames, keeps its SPC700 execution synchronized, and allows
up to five consecutive skipped display frames when behind. NES envelopes
and pulse sweeps use 240/120 Hz clocks, independent of the sample rate;
constant volume, pulse-two negative sweep and stale muted buffers are fixed.
Duke retains CON sound filenames and its reachable sound-options screen,
with working sound/music toggles and volume controls. Its positional effects
also apply a simple stereo pan and the reverse-stereo setting.

## Save and startup repairs

The common FAT layer implements long ASCII save filenames, append, overwrite,
remove, rename and directory enumeration. Full-card allocation stops after a
bounded scan. Failed writes retain dirty buffers and propagate errors rather
than discarding uncommitted data. SDIO waits for both transfer completion and
the card's DAT0 busy period before shutting its clock off. Heap exhaustion
returns NULL to the engines instead of terminating the process.
Directory expansion clears newly allocated clusters, including their first
sector, so deleted file contents cannot become spurious directory entries.

SNES executes the previously skipped SPC700 instructions and reserves enough
heap for the APU and 16-bit renderer on the 4 MiB phone. Build engines keep
512 KiB of heap outside their resource caches. Doom-family zone sizes also
leave 512 KiB for graphics and audio; Hexen otherwise exhausted its heap while
creating the display. The keypad map used for volume
chords is copied before the stock NOR mapping is removed. Startup console
messages are retained in a bounded RAM ring for emulator inspection.
The shared `strncat` implementation appends at the terminating NUL and always
terminates its result. Previously it wrote the suffix after the NUL, leaving
Wolfenstein's save path as `savegam0.`. Wolfenstein also reports failed save
opens instead of dereferencing a NULL file.
Doom-family engines dispatch sound to the new backend instead of their old
embedded no-op functions. SC6530 startup preserves loaded bytes at the two
SMC mode-command addresses for emulators that treat those cycles as RAM writes.

## Reproducing validation

After building the games, run these checks with the host compiler and ARM
toolchain available on PATH:

```text
python scripts/tests/test-build.py
python ports/games/tests/test-fat.py --cc gcc
python ports/games/tests/test-audio.py --cc gcc
python ports/games/tests/test-performance.py --cc gcc --cxx g++
python ports/games/tests/test-strings.py --cc gcc
python ports/games/tests/test-arm-save.py --qemu build/qemu/build/qemu-system-arm.exe --nor /path/to/stock-dump.bin
```

The FAT suite exercises the actual prepared writer, including full-card and
failed-write cases. The audio suite checks the actual mixer and sequencer.
The performance regression suite compares the fixed OPL calibration and
batched PCM with the original generator, checks cache reads/eviction/bounds,
and exercises InfoNES envelopes, sweep and timed writes at 11025, 22050 and
44100 Hz. Host benchmark timings measure the calibration workload; they
are not phone frame-rate measurements.
The ARM test boots twice through the SD loader, writes and independently
verifies a 72 KiB long-name save, replacement saves and append data, exhausts
and recovers the heap, and checks stereo audio while SD writes run.

`ports/games/tests/run-game.py` can launch any packed game on a disposable
FAT32 image, capture screenshots, RAM and audio, and inject timestamped
keys. For example:

```text
python ports/games/tests/run-game.py --qemu build/qemu/build/qemu-system-arm.exe --nor /path/to/stock-dump.bin --binary build/games/snes9x_16bit.bin --rom /path/to/SuperMetroid.sfc --output build/validation/snes --seconds 30
```

Use `--data` with `--data-path games/doom1` and `--command` to test a native
port; `--rom-path` sets another emulator ROM's card path. `--actions` accepts
a JSON array such as `[{"seconds": 8, "key": "esc-0", "hold": 300}]`.
`--reuse-image` restarts the same saved card, and `--trace-int` records CPU
exceptions. Images default to 64 MiB; use `--size 128` for Blood, whose resources
leave too little room for a large save on a 64 MiB test card.
The harness adjusts only its disposable SD loader for QEMU's NOR
alias; production game binaries run unchanged. Validation artifacts stay
under ignored `build/validation/` and require user-owned firmware/game data.

These checks do not establish real-phone performance, routing or reliability.
After installing on a card, verify gameplay, saving and restarting, headphone
insertion/removal and volume chords on the phone before treating a port as
hardware-validated.

The current changes built all sixteen binaries both locally and on a fresh
remote Windows checkout. QEMU checks reached Super Metroid's game menu with
audio and an 8 KiB SRAM save, Pokemon Silver with audio and a 32 KiB battery
save, and Super Mario Bros gameplay with audio. Wolfenstein saved a 21 KiB
file and loaded it after restarting. Blood 1.21 reached gameplay, wrote a
438 KB save on a 128 MiB test card and loaded it after restarting. Heretic,
Hexen and Retris reached gameplay with nonzero audio. Doom, Duke and Shadow
Warrior also produced nonzero audio. Each game's full save-menu, level and
exit coverage still needs further testing on the phone.
