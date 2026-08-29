# BUILD.md — building B310E-OS from scratch

This page walks you from a bare Windows 10/11 machine to a bootable `os.bin`.
You need three tools, all installable with `winget`. No Linux, no WSL, no
MSYS2.

## Prerequisites

- Windows 10 or 11 with `winget` (the App Installer, present by default).
- An internet connection for the one-time tool install.
- The SM-B310E phone and a USB cable, only if you plan to run the hardware
  test in `FLASH.md`. Building the image does not need the phone.

## 1. Install the toolchain

Three packages:

| Tool                    | Pinned version | winget id                               |
|-------------------------|----------------|-----------------------------------------|
| Arm GNU Toolchain       | 14.2.Rel1      | `Arm.GnuArmEmbeddedToolchain`           |
| GNU Make                | 4.4.1          | `ezwinports.make`                       |
| MinGW-W64 (WinLibs)     | 16.1.0         | `BrechtSanders.WinLibs.POSIX.UCRT`      |

- **Arm GNU Toolchain** is the cross compiler (`arm-none-eabi-gcc`) for the
  ARM926EJ-S.
- **GNU Make** drives the build. Run plain `make`, NOT `mingw32-make`.
- **MinGW-W64** provides a host `gcc`, used only to build and run the PC-side
  unit tests.

Open PowerShell and run:

```powershell
winget install --id Arm.GnuArmEmbeddedToolchain
winget install --id ezwinports.make
winget install --id BrechtSanders.WinLibs.POSIX.UCRT
```

### PATH note

winget does **not** add any of these to PATH. You have two options:

1. Add the folders yourself. The ARM toolchain bin is typically
   `C:\Program Files (x86)\Arm GNU Toolchain arm-none-eabi\14.2 rel1\bin`.
2. Do nothing. The Makefile probes for each tool with `where`/`which` first;
   if a tool is missing from PATH, it prepends a pinned install directory
   automatically (the ARM dir above, and `mingw64\bin` under the WinLibs
   package folder in `%LOCALAPPDATA%\Microsoft\WinGet\Packages`) and prints a
   line saying which fallback it used. This is the intended path on a fresh
   machine.

## 2. Build

From the repository root, run:

```powershell
make            # build os.bin (also produces os.elf)
make dis        # disassemble os.elf
make size       # section sizes
make clean      # remove build artifacts
```

`make` produces `os.bin` in `build/bin/`. That is the file you load onto the
phone (see `FLASH.md`).

Expected sizes from `make size`: text about 6 KB (6272 B), bss about 46 KB
(46747 B, mostly the 40 KB LCD framebuffer).

Notes:

- The repo may live on a network share. The Makefile handles that: it runs
  recipes in a way that works from a UNC path, and `clean` uses PowerShell
  instead of cmd builtins.
- The host test binary is written to `build/hosttest.exe`. If you edit a file
  that host tests `#include` directly, delete that binary to force a rebuild
  (the Makefile does not track `#include` dependencies).

## 3. Test

```powershell
make hosttest   # build and run the PC-side unit tests (182 checks)
make check      # host tests + ELF assertions on os.elf
```

`make check` also asserts the ARM image itself:

- entry point `_start` at `0x14000010`
- first LOAD segment at `0x14000000`

Both checks print PASS, ending with `check: ALL PASS`.

## 4. Beyond the basic build

```powershell
make os-sd      # os-sd.bin - USB-free SD-boot OS variant
make debug      # all diag images (LCD/rot/SD/MMU/NOR/DSP tests) -> build/bin/
make sdcard     # stage the complete SD card (menu + games + os + rockbox)
make clean      # remove build/ (artifacts + auto-cloned fpdoom/rockbox trees)
```

`make sdcard` auto-clones the fpdoom and Rockbox sources into `build/` on
first use, rebuilds the game ports, fetches the prebuilt game binaries from
the fpdoom release, and stages the card. The only manual step afterwards is
copying your own game data files (WADs/ROMs/GRPs) into `sdcard/games/` per
the README it generates there.

## 5. Next step

The image is built. Follow `FLASH.md` to RAM-load it on the phone and watch it
boot.

## 6. Building on Linux

The same Makefile drives the build on Linux — it detects the platform
(`OS` unset ⇒ not Windows) and switches the shell commands accordingly
(`/bin/sh`, `command -v` toolchain probes, `mkdir`/`cp`/`cat`/`dd` coreutils
recipes instead of PowerShell). All the tool scripts have bash equivalents
(`tools/**/*.sh`) that the Makefile's `sdcard`/`stockram`/`games`/`rockbox`
targets invoke automatically on Linux.

### Prerequisites (Debian/Ubuntu)

```bash
sudo apt install gcc-arm-none-eabi make gcc unzip p7zip curl git \
                 meson ninja-build libpng-dev     # meson/ninja/libpng only for the QEMU machine
```

Other distros: `gcc-arm-none-eabi`, `make`, `gcc`, `unzip`, `p7zip`,
`curl`, `git`, and for QEMU `meson`/`ninja`/`libpng-dev` (Fedora:
`arm-none-eabi-gcc`; Arch: `arm-none-eabi-gcc` + `p7zip`).

### Build

```bash
make            # os.bin -> build/bin/os.bin
make check      # host tests (182) + ELF assertions
make sdcard     # full card: boot menu + games + os + rockbox (clones into build/)
make clean      # remove build/
```

Differences from Windows:

- **Rockbox** (`make rockbox` / part of `make sdcard`): runs
  `tools/rockbox-port/build.sh` directly (no MSYS2 wrapper). It detects Linux
  (`/c/arm-gcc` absent) and takes `arm-none-eabi-gcc`/`gcc` from PATH.
  Requires `zip` for the runtime-data package and `unzip` for extraction.
- **Game ports** (`make games`): `tools/fpmain/rebuild-games.sh` needs `7z`
  (p7zip) to extract the prebuilt game binaries from the fpdoom release — it
  fails with a clear message if `7z` is missing. The ports' own builds use
  `cc` (present on Linux; absent in git-bash on Windows).
- **stockram / stock-spy / stockram-diag**: the `.sh` equivalents assemble
  the same images; outputs are byte-identical to the PowerShell versions
  (verified with `cmp`).
- **Ghidra import**: `tools/ghidra-import.sh` — set `GHIDRA_INSTALL_DIR`
  (default probes `/opt/ghidra*`) and `JAVA_HOME` (or a JDK 21+ `java` on
  PATH). Requires `dump_firmware.bin` at the repo root.
- **Hardware test**: `tools/run-hw-test.sh` looks for `spd_dump` (no `.exe`)
  in `tools/spd_dump`; the Windows binaries in that folder are cross-compatible
  under wine, but the native build is preferred.
- **Card copy**: `tools/copy-to-card.sh` — set `CARD=/path/to/card-root`
  (default probes `/media/$USER/SD_CARD` and other mounts for a writable
  `fpbin` dir), then it copies + SHA-256 verifies.
- **QEMU machine**: `tools/qemu-b310e/scripts/build-qemu.sh` (needs
  `meson`/`ninja`/`libpng-dev`; no elevation dance — that is Windows-only) and
  `install-machine.sh`/`run-b310e.sh`/`extract-audio-trace.sh` have `.sh`
  twins in the same directory.

Every `.sh` script mirrors its `.ps1` twin's behavior; run them from a
git-bash/login shell if testing on Windows (a bare `bash -c` from PowerShell
does not put coreutils on PATH).
