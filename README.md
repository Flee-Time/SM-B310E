# B310E-OS — custom firmware for the Samsung SM-B310E (Spreadtrum SC6530C)

B310E-OS is a from-scratch, bare-metal operating system for the Samsung
SM-B310E "Guru Music 2" feature phone — a Spreadtrum SC6530C: ARM926EJ-S at
208 MHz, ARMv5TE (Thumb-1 only), ~4 MB embedded PSRAM, no Linux, no vendor
SDK, no off-the-shelf BSP.

Everything from the boot stub to the demo apps is written for this one chip,
modeled on [fpdoom](https://github.com/ilyakurdyukov/fpdoom)'s public-domain
SC6530 code (Unlicense). The firmware is RAM-loaded through the phone's USB
download mode with `spd_dump`, and can also boot from an SD card.

> **Heads-up: this is an AI-assisted reverse-engineering / firmware
> development project.** Most of the register-level ground truth, decompiled
> firmware analysis (via Ghidra), and the code itself was produced with
> substantial help from AI assistants working from the stock firmware dump.
> Expect it to look like it: the register maps and boot sequences are
> empirical (verified on hardware where noted), not from a vendor datasheet.

## What works (hardware-verified)

- **Custom micro-kernel**: cooperative round-robin scheduler + preemptive
  1 ms tick, message queues, module framework, bump allocator, printk,
  IRQ infrastructure with an MMU high-vector map.
- **Boots from RAM** via `spd_dump` (zero NOR writes, stock OS intact after
  reboot): FDL1 → `os.bin` at `0x34000000`.
- **Boots from an SD card**: a clean fpdoom `sdboot3.bin` NOR loader reads a
  ported fpdoom boot menu (`fpmain.bin`) that lists `progs/` and `games/`
  and launches them — including this OS, Rockbox, and the fpdoom game ports.
- **Hardware drivers**: ST7735S LCD (framebuffer + LCDC DMA), keypad matrix
  with the real extracted keymap, SD/SDIO + read-only FAT32, USB debug
  channel (`libc_server` protocol), keypad light, vibrator, battery gauge,
  RTC, and a complete (but DSP-gated) audio register chain.
- **Ports on the SD card**: fpdoom + chocolate-doom/heretic/hexen, gnuboy,
  retris, infones, fpsw, fpduke3d, snes9x — and a working **Rockbox port**
  for the SC6530C.
- **A QEMU machine** (`emulator/qemu/`) for stock firmware bring-up and
  this OS on the PC. Stock boot reaches time/date setup; an experimental
  external display input allows navigation into its home screen and menus.
  Built-in stock ringtone playback now produces verified stereo audio through
  the MIDI renderer and DMA. Full phone operation and DSP-controlled audio
  remain under development. The ARM PCM path also has CPU/DMA device tests.
  See [current emulator/audio status](docs/emulator-audio.md).

## Repository map

```
firmware/     bare-metal OS: arch, kernel, drivers, app, include, link, tests
ports/        fpmain JSON boot menu and Rockbox target overlays/tests
emulator/     QEMU machine models and emulator capture/regression tools
scripts/      shared build dispatcher and canonical target implementations
tools/        flashing/debug tools, DSP analysis and stock-firmware research
docs/         build, boot, controls, power and reverse-engineering notes
build/        ignored generated outputs, upstream caches and build logs
sdcard/       ignored card staging, local music, ROMs and settings
build.ps1     Windows build entry point
build.sh      Linux build entry point
Makefile      optional aliases to the shared dispatcher
```

## Build

Start with [the build guide](docs/build.md) for dependencies and tool detection.

```powershell
.\build.ps1 doctor
.\build.ps1 sdcard      # OS + JSON menu + games + Rockbox runtime
.\build.ps1 sd-image    # FAT32 emulator image with stereo test audio
.\build.ps1 qemu        # pinned GTK/SDL desktop emulator and Windows package
.\build.ps1 check       # host tests and ARM boot-address checks
.\build.ps1 clean       # preserves card data and downloaded caches
```

On Linux use `bash build.sh <target>`. Individual targets include `firmware`,
`os-sd`, `fpmain`, `games`, `rockbox`, `debug` and `hosttest`. All generated
build files stay under `build/`; the scripts stage phone files in `sdcard/`.
Bring your own game data files (WADs/ROMs/GRPs); the generated game README
explains where they go. Normal builds need no private DSP dump.

## Hardware test (RAM load — zero brick risk)

Five-step procedure in [flashing guide](docs/flashing.md): enter download mode (D-pad
CENTER held while plugging USB), load `os.bin` with `spd_dump`, watch the
kernel log in `libc_server`, check the LCD. Nothing writes NOR.

## Documentation

- [build guide](docs/build.md) — toolchain install + build from scratch
- [flashing guide](docs/flashing.md) — hardware boot test (RAM load) + sdboot3.bin NOR install
- [docs/sdboot.md](docs/sdboot.md) — SD-card boot chain (sdboot → boot menu → progs)
- [docs/stockram.md](docs/stockram.md) — booting the *stock* Samsung firmware from RAM
- [docs/b310e-qemu.md](docs/b310e-qemu.md) — the SC6530C QEMU machine
- [docs/emulator-audio.md](docs/emulator-audio.md) — current stock boot/audio evidence and repeatable checks
- [docs/audio-dsp-protocol.md](docs/audio-dsp-protocol.md) — ARM↔DSP host interface protocol
- [docs/dsp-audio-route.md](docs/dsp-audio-route.md) — complete DSP + audio signal-path map (incl. the stock-spy capture tool)

## License

This project is released under the [MIT license](LICENSE). The fpdoom-derived
portions are Unlicense/public domain (fpdoom itself is Unlicense). The
Rockbox port is GPLv2 (Rockbox's license) — see
`ports/rockbox/patches/PATCHES.md`.

**Provenance notes** (important):

- **The host tools are NOT our code.** `spd_dump.exe`, `fphelper.exe`,
  `fphelper_t117.exe`, `unpac.exe`, and `nor_fdl1.bin` (in `tools/spd_dump/`)
  are built from [ilyakurdyukov/spreadtrum_flash](https://github.com/ilyakurdyukov/spreadtrum_flash)
  (the SC6530 download/flash tooling). `libc_server.exe` (in
  `tools/libc_server/`) is built from
  [ilyakurdyukov/fpdoom](https://github.com/ilyakurdyukov/fpdoom) (its USB
  debug console). Both are distributed as-is for this project's use; the
  source lives in those upstream repos.
- `dump_firmware.bin` (the 16 MB stock firmware dump used for all the
  analysis) is **not** in this repository.
- The `tools/mocor-zw217/` vendor SDK referenced in some docs is a leaked
  internal Samsung/Spreadtrum SDK — it is **never committed** and is used
  only as a read-only reference for register ground truth.
- Reverse-engineering was done for interoperability/research on hardware the
  author owns. No proprietary code is copied into this repository.
