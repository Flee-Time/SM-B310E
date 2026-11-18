# B310E-OS — custom firmware for the Samsung SM-B310E (Spreadtrum SC6530C)

Custom software for the Samsung SM-B310E "Guru Music 2" feature phone — a
Spreadtrum SC6530C: ARM926EJ-S at 208 MHz, ARMv5TE (Thumb-1 only), ~4 MB
embedded PSRAM, no Linux, no vendor SDK, no off-the-shelf BSP.

- **B310E OS** ([os/](os/README.md)): our own operating system, written
  from scratch, and the future home of GSM calls and SMS. Design:
  [docs/os-design.md](docs/os-design.md). Its apps are loadable `.app`
  files; Chocolate Doom is one ([os/README.md, "Doom"](os/README.md#doom);
  in QEMU so far).
- **A Rockbox port** for music, recording and FM radio.
- **An SD boot menu** that launches either, and the stock firmware.

The low-level code is modeled on
[fpdoom](https://github.com/ilyakurdyukov/fpdoom)'s public-domain SC6530
code (Unlicense).

> **Heads-up: this is an AI-assisted reverse-engineering / firmware
> development project.** Most of the register-level ground truth, decompiled
> firmware analysis (via Ghidra), and the code itself was produced with
> substantial help from AI assistants working from the stock firmware dump.
> Expect it to look like it: the register maps and boot sequences are
> empirical (verified on hardware where noted), not from a vendor datasheet.

## What works (hardware-verified)

- **B310E OS, milestone 1** (verified on hardware): our own preemptive
  kernel (32 priorities, priority-inheritance mutexes, semaphores, queues),
  LCD, keypad, console and watchdog reboot; boots from the boot menu as
  `payloads/b310e-os.bin`. See [os/README.md](os/README.md).
- **Our own NOR boot loader** (`boot/`, see [docs/sdboot.md](docs/sdboot.md)):
  at power-on a menu in the B310E OS look lists the card's `payloads/`
  (B310E OS, Rockbox) and the stock firmware; programs can ask it for the
  menu or a payload across a watchdog reset. It replaced fpdoom's sdboot
  and fpmain menu (2026-11-07).
- **Games as apps of B310E OS**, from upstream sources: Doom, Heretic and Hexen
  (Chocolate Doom), Wolfenstein 3D, Duke Nukem 3D, Game Boy and NES emulators,
  on `os/ports/kit`. And a working **Rockbox port** for the SC6530C.
- **USB mass storage in Rockbox** (verified on hardware; reads about
  230 KB/s, writes up to about 700 KB/s):
  the SD card appears as a drive on a PC. See [docs/usb.md](docs/usb.md).
- **Microphone recording in Rockbox** (verified on hardware): Main Menu →
  Recording, through the ARM-owned ADC/VBC capture path; no DSP needed.
  See [docs/rockbox-recording.md](docs/rockbox-recording.md).
- **FM radio in Rockbox** (verified on hardware): the RTC6211N tuner on
  I2C controller 1 with an analog line-in route; the headset cable is the
  antenna. See [docs/rockbox-fmradio.md](docs/rockbox-fmradio.md).
- **SIM card reader in Rockbox** (verified on hardware): reads a card's ATR,
  ICCID and IMSI directly, without the modem. See [docs/sim.md](docs/sim.md).
  For calls and SMS, [docs/gsm-plan.md](docs/gsm-plan.md) surveys the stock
  GSM stack and lays out a plan.
- **Torch in Rockbox** (verified on hardware): hold 0 to toggle the torch
  LED on GPIO 7. See
  [docs/rockbox-controls.md](docs/rockbox-controls.md#torch).
- **A QEMU machine** (`emulator/qemu/`) for stock firmware bring-up and
  this OS on the PC. Stock boot reaches time/date setup; an experimental
  external display input allows navigation into its home screen and menus.
  Built-in stock ringtone playback now produces verified stereo audio through
  the MIDI renderer and DMA. Full phone operation and DSP-controlled audio
  remain under development. The ARM PCM path also has CPU/DMA device tests.
  See [current emulator/audio status](docs/emulator-audio.md).

## Repository map

```
os/           B310E OS: arch, kernel, drivers, lib, tests (GPLv2+)
boot/         the NOR boot loader and its menu (GPLv2+)
ports/        game-port tooling and Rockbox target overlays/tests
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
.\build.ps1 os          # B310E OS -> sdcard/payloads/b310e-os.bin
.\build.ps1 boot        # NOR boot loader -> build/boot/b310e-boot.bin
.\build.ps1 sdcard      # OS + its apps and games + Rockbox runtime
.\build.ps1 sd-image    # FAT32 emulator image with stereo test audio
.\build.ps1 qemu        # pinned GTK/SDL desktop emulator and Windows package
.\build.ps1 clean       # preserves card data and downloaded caches
```

On Linux use `bash build.sh <target>`. Individual targets include `os`,
`boot` and `rockbox`. All generated
build files stay under `build/`; the scripts stage phone files in `sdcard/`.
Bring your own game data files (WADs/ROMs/GRPs); [os/README.md](os/README.md)
explains where they go. Normal builds need no private DSP dump.

## Testing

- **QEMU:** `python os/tests/test-boot.py` boots B310E OS and checks its
  kernel self-test, keys and reboot; Rockbox has its own tests in
  `ports/rockbox/tests/`.
- **QEMU, boot loader:** `python boot/tests/test-boot.py` (needs your own
  NOR dump; it stays local).
- **QEMU, instruction counts:** `python os/tests/bench-instr.py --json run.json`
  boots the OS under `-icount` with the `b310e-prof` TCG plugin and counts the
  instructions of fixed scenarios (boot, idle lit and dark, menu and list
  navigation, dialogs, the quick panel, text input, an app opened and closed,
  settings changed), exactly and the same every run, per phase and per
  function (top functions printed; every function in the JSON and in a CSV per
  phase under `build/os/bench-instr/`). `--phase NAME` (repeatable) runs only
  some; `--compare before.json after.json` prints each phase's change and the
  functions that moved most. For "same pixels, fewer instructions": QEMU has
  no caches or wait states, so the phone stays the final check. Details:
  [emulator/qemu/README.md](emulator/qemu/README.md).
- **Host, no QEMU:** `python os/tests/test-host.py` compiles OS source files
  as they are on disk for the PC (msys64 gcc, 64-bit, UBSan in trap mode)
  against stand-ins in `os/tests/host/` (a guard allocator that catches
  overruns, double frees and writes after free and can fail the n-th
  allocation; FatFs on files in RAM with a power-cut budget; a one-thread
  kernel; klog into a buffer) and runs one case each, about 20 s in all:
  `db-power-cut` (the data store with the card's power cut at every byte,
  rename and unlink of a scripted run, torn tails clean and junk: after the
  reboot each setting and record is as before or after the operation in
  flight), `db-oom` (every allocation failing in turn, also during the load),
  `crc32`, `string`, `printf` (against the C library, and the cut on a whole
  UTF-8 character), `utf8-case` (UTF-8 helpers, Turkish i/I case),
  `gsm` (LAPDm, RR, MM, the registration FSM: self-tests, known frames,
  random and cut frames) and `art-fuzz` (the cover decoders on PIL-made
  covers and their mutations). One PASS/FAIL line a case; `--case NAME`,
  `--long` (every cut, thousands of mutations), `--prove` (each case built
  with a broken reference must fail: the checks can see a fault).
- **Stack audit:** `python os/tests/stack-audit.py` compiles the OS with
  its real ARM flags plus `-fstack-usage -fcallgraph-info=su` into a scratch
  folder and prints the largest frames and, for each thread (and interrupt
  handler), the deepest direct-call chain next to the stack it is given,
  with what it cannot see (indirect calls, recursion, assembler) flagged. A
  report, not a test.
- **Phone:** copy `sdcard/payloads/` to the card and pick the payload in the
  boot menu. Installing the loader writes NOR: [docs/sdboot.md](docs/sdboot.md).

## Documentation

- [build guide](docs/build.md) — toolchain install + build from scratch
- [flashing guide](docs/flashing.md) — download mode and the RAM-load test
- [docs/sdboot.md](docs/sdboot.md) — the NOR boot loader: design, install, restore
- [docs/stockram.md](docs/stockram.md) — booting the *stock* Samsung firmware from RAM
- [docs/b310e-qemu.md](docs/b310e-qemu.md) — the SC6530C QEMU machine
- [docs/emulator-audio.md](docs/emulator-audio.md) — current stock boot/audio evidence and repeatable checks
- [docs/audio-dsp-protocol.md](docs/audio-dsp-protocol.md) — ARM↔DSP host interface protocol
- [docs/dsp-audio-route.md](docs/dsp-audio-route.md) — complete DSP + audio signal-path map (incl. the stock-spy capture tool)
- [docs/usb.md](docs/usb.md) — USB controller register map, Rockbox mass storage driver and its QEMU test

## License

This project is released under the [MIT license](LICENSE), except B310E OS
in `os/`, which is GPLv2 or later ([os/LICENSE](os/LICENSE)). The fpdoom-derived
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
