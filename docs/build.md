# Building the B310E project

Run every supported build from the repository root with `build.ps1` on Windows,
`bash build.sh` on Linux, or `python scripts/build.py` on either platform.
`make` remains a small set of aliases to that same entry point.

## Dependencies

Python 3.10+, Git, an `arm-none-eabi` GCC toolchain, and host GCC are needed
for firmware. The native firmware build does not require Bash or Make.
Arm GNU Toolchain 14.2.Rel1 is the currently tested compiler.

Menu/game/Rockbox builds also need Bash, Make and the standard POSIX tools
(`sed`, `perl`, `curl`, `tar`). Games need 7-Zip for the pinned
upstream release binaries. Rockbox needs `zip`. Install these dependencies
before building; the scripts do not install packages or request elevation.

On Windows, ports use MSYS2 (default `C:/msys64`). Install its MINGW64 host GCC
and Python; pass `--msys` or set `B310E_MSYS` for a different installation.
The Windows download helper uses the system certificate store. On Linux,
use your distribution's host GCC and POSIX tools.

QEMU additionally needs its normal build dependencies: Ninja, Meson, GLib,
Pixman, libpng, pkg-config and host GCC. On Windows these must be the MSYS2
MINGW64 packages. The supported emulator build is headless, with PNG captures;
it does not require GTK, SDL, administrator access or firmware dumps.

## Commands

```powershell
.\build.ps1 doctor               # show detected tools
.\build.ps1 firmware             # RAM-loaded OS: build/bin/os.bin
.\build.ps1 os-sd                # USB-free SD-boot OS
.\build.ps1 fpmain               # JSON boot menu, staged into sdcard/fpbin
.\build.ps1 games                # supported game ports and pinned prebuilts
.\build.ps1 rockbox              # player binary and matching .rockbox runtime
.\build.ps1 sdcard               # complete card: OS + menu + games + Rockbox
.\build.ps1 sd-image             # FAT32 emulator image with stereo test.wav
.\build.ps1 qemu                # emulator: build/qemu/build/qemu-system-arm.exe
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

`sd-image` needs the staged menu and Rockbox files. Existing images are protected;
pass `--force` only when you want to replace the generated image. The image
contains a stereo test WAV and a test configuration; it is separate from your
physical card and its settings.

The optional `dsp-diag --dsp-blob /path/to/dsp-blob-CC874.dec.bin` target
requires the previously verified private DSP bundle and checks its SHA-256.
Ordinary diagnostics and complete card builds do not require proprietary data.
The legacy research packers remain available as `stockram` and `stockram-diag`;
see [stockram.md](stockram.md) for their stock-dump requirements.

After building, see [flashing.md](flashing.md) for RAM loading and SD boot,
[rockbox-audio.md](rockbox-audio.md) for audio checks, and
[emulator-audio.md](emulator-audio.md) for stock emulator testing.
