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
- **A QEMU machine** (`tools/qemu-b310e/`) that boots the stock firmware and
  this OS on the PC, used for the audio/DSP work.

## Repository map

```
arch/         startup asm, C entry, SC6530C chip init, boot stubs
kernel/       scheduler, queues, module framework, bump allocator, printk, IRQ
drivers/      LCD, keypad, USB debug, SD/SDIO, FAT32, audio, LED, battery, RTC
app/          demo tasks (banner, keypad echo, SD probe)
include/      shared public headers (os.h, arch.h, kernel.h)
link/         linker scripts (os.ld, menu.ld)
tests/        host-side unit tests (run on the PC, not the phone)
tools/        host tools (spd_dump, libc_server, fpdoom port build, rockbox port,
              stock-ram shim, stock-spy, QEMU machine, DSP analysis scripts)
docs/         boot, stock-firmware analysis, audio/DSP protocol, QEMU usage
Makefile      build + test targets
```

## Build

Windows 10/11, three winget packages (Arm GNU Toolchain 14.2, GNU Make,
WinLibs gcc) — no Linux, no WSL, no MSYS2 for the core build. Full toolchain
install + PATH notes in [BUILD.md](BUILD.md).

```powershell
make            # os.bin (main firmware) -> build/bin/os.bin
make os-sd      # os-sd.bin (USB-free SD-boot OS variant)
make debug      # all diag images -> build/bin/ (LCD/rot/SD/MMU/NOR/DSP tests)
make sdcard     # stage the COMPLETE SD card (boot menu + games + os + rockbox)
make hosttest   # 182 PC-side unit checks
make check      # host tests + ELF assertions (entry 0x14000010, .text 0x14000000)
make clean      # remove build artifacts + clones (build/)
```

`make sdcard` builds everything (it auto-clones fpdoom/rockbox into `build/`
on first use, rebuilds the game ports, fetches the prebuilt game binaries,
and stages the card: `fpbin/` boot menu + games, `progs/os.bin`,
`progs/rockbox.bin`, `.rockbox/` runtime data). Game *data files* (WADs,
ROMs, GRPs) are **not** shipped — see `sdcard/games/README.md` after staging.

## Hardware test (RAM load — zero brick risk)

Five-step procedure in [FLASH.md](FLASH.md): enter download mode (D-pad
CENTER held while plugging USB), load `os.bin` with `spd_dump`, watch the
kernel log in `libc_server`, check the LCD. Nothing writes NOR.

## Documentation

- [BUILD.md](BUILD.md) — toolchain install + build from scratch
- [FLASH.md](FLASH.md) — hardware boot test (RAM load) + sdboot3.bin NOR install
- [docs/sdboot.md](docs/sdboot.md) — SD-card boot chain (sdboot → boot menu → progs)
- [docs/stockram.md](docs/stockram.md) — booting the *stock* Samsung firmware from RAM
- [docs/b310e-qemu.md](docs/b310e-qemu.md) — the SC6530C QEMU machine
- [docs/audio-dsp-protocol.md](docs/audio-dsp-protocol.md) — ARM↔DSP host interface protocol
- [docs/dsp-audio-route.md](docs/dsp-audio-route.md) — complete DSP + audio signal-path map (incl. the stock-spy capture tool)

## License

This project is released under the [MIT license](LICENSE). The fpdoom-derived
portions are Unlicense/public domain (fpdoom itself is Unlicense). The
Rockbox port is GPLv2 (Rockbox's license) — see
`tools/rockbox-port/patches/PATCHES.md`.

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
