# Building the B310E project

Run every supported build from the repository root with `build.ps1` on Windows,
`bash build.sh` on Linux, or `python scripts/build.py` on either platform.
`make` remains a small set of aliases to that same entry point.

## Dependencies

Python 3.10+, Git, an `arm-none-eabi` GCC toolchain, and host GCC are needed
for firmware. The native firmware build does not require Bash or Make.
Arm GNU Toolchain 14.2.Rel1 and 15.3.Rel1 have been tested. The clean Windows
setup was validated with MSYS2 host GCC 16.2.0 and ARM GCC 15.3.1.

Menu/game/Rockbox builds also need Bash, Make and the standard POSIX tools
(`sed`, `perl`, `curl`, `tar`). Games need 7-Zip for the pinned
upstream release binaries. Rockbox needs `zip`. Install these dependencies
before building; the scripts do not install packages or request elevation.

On Windows, ports use MSYS2. The entry point detects `D:/Toolchains/msys64`
first, then `C:/msys64`. Install its MINGW64 host GCC and Python; pass `--msys`
or set `B310E_MSYS` for a different installation. MINGW64 host GCC is preferred
over unrelated WinLibs/UCRT compilers on Windows PATH. An explicit `--host-cc`
still overrides that choice.
The Windows download helper uses the system certificate store. On Linux,
use your distribution's host GCC and POSIX tools.

QEMU additionally needs Ninja, Meson, GLib, Pixman, libpng, pkg-config,
GTK3, SDL2, libepoxy (OpenGL) and host GCC. The default build includes GTK,
SDL, OpenGL, VNC, keyboard input and host audio.
`--qemu-headless` omits GTK/SDL/OpenGL and the Windows package for automated
tests. Firmware dumps are not needed to compile either build.

The recommended Windows layout is:

```text
D:/Toolchains/msys64/                 MSYS2, including mingw64/bin/
D:/Toolchains/arm-none-eabi/bin/       separate ARM compiler
D:/Toolchains/downloads/              verified installer cache
```

To bootstrap MSYS2 and all desktop/port dependencies into that layout:

```powershell
.\scripts\setup-windows.ps1 -ToolchainsRoot D:/Toolchains -SetUserEnvironment
.\build.ps1 doctor
.\build.ps1 qemu
```

The setup script checks the published SHA-256 of the MSYS2 base, fully updates
it, and installs dependencies. `-SetUserEnvironment` saves `B310E_MSYS` and,
if present, `B310E_TOOLCHAIN` for new shells; it leaves global PATH untouched.
Install the ARM compiler separately into `arm-none-eabi/`. The build scripts
do not install packages themselves. Setup does not delete older installations.

For manual setup, use one MSYS2 **MINGW64** installation.
Update MSYS2 with `pacman -Syu`, following any instruction to close/reopen
the shell and finish the update. Then install these packages in its MINGW64
shell; do not mix UCRT64 or MSYS libraries with MINGW64 libraries:

```sh
pacman -S --needed base-devel git \
  mingw-w64-x86_64-gcc mingw-w64-x86_64-python \
  mingw-w64-x86_64-python-setuptools mingw-w64-x86_64-python-wheel \
  mingw-w64-x86_64-meson mingw-w64-x86_64-ninja mingw-w64-x86_64-pkgconf \
  mingw-w64-x86_64-glib2 mingw-w64-x86_64-pixman mingw-w64-x86_64-dtc \
  mingw-w64-x86_64-libpng mingw-w64-x86_64-gtk3 \
  mingw-w64-x86_64-SDL2 mingw-w64-x86_64-libepoxy
```

On Debian/Ubuntu the corresponding desktop dependencies can be installed with
`sudo apt install build-essential git python3 python3-venv ninja-build meson
pkg-config libglib2.0-dev libpixman-1-dev libpng-dev libfdt-dev libgtk-3-dev
libsdl2-dev libepoxy-dev`. Other distributions use their corresponding packages.
QEMU downloads its pinned Meson subprojects on a fresh clone. Internet access is
therefore required for the first build; later invocations reuse the checkout.

## Commands

```powershell
.\build.ps1 doctor               # show detected tools
.\build.ps1 firmware             # RAM-loaded OS: build/bin/os.bin
.\build.ps1 os-sd                # USB-free SD-boot OS
.\build.ps1 fpmain               # JSON boot menu, staged into sdcard/fpbin
.\build.ps1 games                # all supported game ports from pinned sources
.\build.ps1 rockbox              # player binary and matching .rockbox runtime
.\build.ps1 sdcard               # complete card: OS + menu + games + Rockbox
.\build.ps1 sd-image             # FAT32 emulator image with stereo test.wav
.\build.ps1 qemu                # emulator: build/qemu/build/qemu-system-arm.exe
.\build.ps1 qemu --qemu-headless # smaller build for automated tests
.\build.ps1 check               # 182 host checks + ARM entry/text checks
.\build.ps1 debug               # LCD, rotation, SD, MMU and NOR diagnostics
.\build.ps1 clean               # generated outputs only
```

On Linux replace `.\build.ps1` with `bash build.sh`. `--jobs 4` sets the
parallel build limit; `--dry-run` shows the plan without changing files.

Compiler directories can be supplied explicitly:

```powershell
.\build.ps1 rockbox --toolchain D:/toolchains/arm/bin --host-cc C:/msys64/mingw64/bin
.\build.ps1 qemu --qemu-source D:/toolchains/qemu --jobs 8
.\build.ps1 sd-image --image sdcard/test-card.img
```

`B310E_TOOLCHAIN` and `B310E_HOST_CC` specify compiler directories;
`B310E_PYTHON` selects Python for the root wrappers; `B310E_QEMU_SOURCE`
overrides the QEMU source directory. Tools on PATH are preferred; Windows
also detects MSYS2 host GCC and the standard Arm 14.2 installation.
Rockbox's upstream Make/configure requires an ARM toolchain path without
spaces. Native firmware builds accept paths with spaces.

For Make aliases use `make rockbox`, `make check`, etc. Set `PYTHON` if necessary:
`make PYTHON=C:/msys64/mingw64/bin/python.exe check`.

## Outputs, downloads and local data

- `build/bin/`: firmware binaries, ELF/relocation files and Rockbox package.
- `build/firmware/`, `build/host/`, `build/fpmain/`, `build/games/`: build outputs.
- `build/fpdoom/`, `build/rockbox/`, `build/qemu/`: upstream source/build caches.
- `build/logs/`: complete logs, one per target.
- `build/qemu-desktop/`, `build/qemu-desktop.zip`: portable Windows GUI package.
- `sdcard/`: generated card staging plus your local music, ROMs and settings.

The upstream revisions are checked before use and existing checkouts are never
automatically reset: fpdoom `04f19d6d54430693d00029970c51cd988c083b05`,
Rockbox `ecdeb02dda6dbb94c1c3b01b8406203eda225f9f`, QEMU v11.1.0
`84f07211cc5b4fc6a371559bf8a5de4fb068e648`.

Native firmware is compiled fresh each invocation; objects never go into source
folders. This avoids stale flags and missed header dependencies. Port builds
use the same canonical scripts on Windows and Linux.

`clean` retains `sdcard/` and downloaded upstream caches. Add `--downloads`
to also remove the three upstream caches inside this checkout's `build/`;
external `--qemu-source` trees are retained. Rockbox packaging updates runtime
files without deleting your saved configuration, playlists or music. Menu
builds stage the tracked default `ports/fpmain/config.json`, so retain any
custom menu configuration separately before rebuilding the menu.

All 16 game binaries now compile from pinned source with the B310E save/audio
overlay. `ports/games/fetch.py` fetches the additional cores, then
`ports/games/prepare.py` creates an isolated `build/game-ports/` tree. Cached
upstream sources stay unchanged. Every variant is cleaned before compilation;
only the declared outputs are staged, so old prebuilt binaries cannot silently
replace an updated port. See [game controls, data and testing](game-ports.md).

`sd-image` needs the staged menu and Rockbox files. Existing images are protected;
pass `--force` only when you want to replace the generated image. The image
contains a stereo test WAV and a test configuration; it is separate from your
physical card and its settings.

## Running the Windows desktop package

After `.\build.ps1 qemu`, extract `build/qemu-desktop.zip` and keep its
`bin/`, `lib/`, `share/` and `etc/` folders together. End users do not need
MSYS2 or Python. Drag a stock `.bin` dump onto `run-stock.cmd`, or run:

```bat
run-stock.cmd "C:\phone\stock.bin" "C:\phone\sdcard.img"
```

The SD image is optional. The launcher opens GTK and enables SDL host audio;
it protects the NOR dump with `readonly=on`. The SD image is writable.
GTK provides fullscreen, zoom and input grab. Arrow
keys are the D-pad, Enter is OK, F1/F2 are the soft keys and Escape is END.
`Ctrl+Alt+G` releases grabbed input. See `emulator-guide.md` in the package
for the remaining keypad mappings and advanced options. The phone firmware
and proprietary reference sources are not included.

The package contains both `bin/qemu-system-arm.exe` (with a console for
startup/serial output) and `bin/qemu-system-armw.exe` (without a console).
Advanced users can select `-display sdl` instead of GTK. Native builds on
Linux run from `build/qemu/build/qemu-system-arm` with `-display gtk`.

An absent or empty source directory is cloned automatically. Existing Git
checkouts, including linked worktrees, are checked against the pinned commit
before machine files are installed. Version errors report the actual commit;
archives without `.git` are identified explicitly. No existing checkout is
reset. Windows builds copy needed resources when symlink privileges are absent,
so enabling Developer Mode or running the build as administrator is unnecessary.

The fresh-build regression checks are `python scripts/tests/test-build.py`
and `python scripts/tests/test-qemu-build.py`. Full configure/build logs are
`b310e-configure.log` and `b310e-build.log` in the QEMU source directory.
`python emulator/qemu/scripts/test-desktop-package.py --package build/qemu-desktop
--output build/validation/desktop-package` relocates the package into a folder
with spaces and checks GTK and SDL with MSYS2 removed from PATH, LCD captures
and soft-key press/release delivery. It uses null audio by default for hosts
without a sound device; `--audio-backend sdl` tests native output startup.
`--opengl` additionally requests a GTK GL context. Accelerated GTK and SDL
both crashed on the local Windows test host and remain unverified on the remote
host. The default launcher explicitly uses `gtk,gl=off`; this 2D display path
passed on both hosts and supports the phone framebuffer and keyboard input.

The optional `dsp-diag --dsp-blob /path/to/dsp-blob-CC874.dec.bin` target
requires the previously verified private DSP bundle and checks its SHA-256.
Ordinary diagnostics and complete card builds do not require proprietary data.
The legacy research packers remain available as `stockram` and `stockram-diag`;
see [stockram.md](stockram.md) for their stock-dump requirements.

After building, see [flashing.md](flashing.md) for RAM loading and SD boot,
[rockbox-audio.md](rockbox-audio.md) for audio checks, and
[emulator-audio.md](emulator-audio.md) for stock emulator testing.
