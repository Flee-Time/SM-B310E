# tools/qemu-b310e - SC6530C QEMU machine (emulator testbed)

QEMU machine sources and build scripts for the B310E emulator work plan
(`.omo/plans/b310e-qemu-machine.md`). The machine models the Spreadtrum
SC6530C so the stock Samsung B310E firmware and our own `os.bin` run on the
PC - the durable testbed for scheduler/audio/Rockbox work that never touches
the real phone.

The usage doc (build recipe, run commands, memory map, trace recipes,
limitations) lives at the repo root: **`docs/b310e-qemu.md`** (created in
Wave 5/7 - do NOT add a `docs/` subdir here).

## Layout

```
tools/qemu-b310e/
|-- machine/              # our QEMU sources (tracked)
|   `-- hw/
|       |-- arm/          #   b310e.c (Wave 3) - copied to qemu-src/hw/arm/
|       `-- misc/         #   sc6530_{aux,intc,timer,adi,lcdc,lcm,keypad,dsp,usb,sdio}.c
|                         #   (Waves 3-4) - copied to qemu-src/hw/misc/
|-- scripts/              # build/install helpers (tracked)
|   |-- install-machine.ps1   # copy machine/ -> qemu-src + wire meson/Kconfig (idempotent)
|   `-- build-qemu.ps1        # full recipe: clone-or-pin -> install -> configure -> build -> sanity
`-- logs/                 # evidence logs (GITIGNORED - never commit)
```

**Convention:** Wave-3/4 todos just drop `.c` files into `machine/hw/arm/` and
`machine/hw/misc/`, then re-run `install-machine.ps1`. The meson/Kconfig
wiring is generated from the files actually present, so new sources are wired
automatically (no manual meson edits).

## The QEMU source tree

**Never inside this repo.** Meson breaks on the UNC share, so the clone lives
in the local dir `<home>\qemu-b310e\qemu-src` (outside the repo; pinned to **v11.1.0**).
`tools/qemu-b310e/qemu-src/` is gitignored as a belt-and-suspenders guard.

## install-machine.ps1

Copies `machine/hw/arm/*.c` -> `qemu-src/hw/arm/`, `machine/hw/misc/*.c` ->
`qemu-src/hw/misc/`, and appends marker-guarded wiring blocks
(`# B310E-MACHINE-BEGIN` / `# B310E-MACHINE-END`) to:

- `hw/arm/meson.build` - `arm_common_ss.add(when: 'CONFIG_<SYM>', if_true: files('<file>.c'))`
- `hw/arm/Kconfig`     - `config <SYM> / bool / default y / depends on TCG && ARM`
- `hw/misc/meson.build` - `system_ss.add(when: 'CONFIG_<SYM>', if_true: files('<file>.c'))`
- `hw/misc/Kconfig`    - `config <SYM> / bool / default y / depends on ARM`

where `<SYM>` = the file name uppercased (`b310e.c` -> `B310E`,
`sc6530_aux.c` -> `SC6530_AUX`). Optional `machine/hw/{arm,misc}/trace-events`
files are appended (marker-guarded) to the qemu-src `trace-events` files -
Wave-3 trace-event prep (todo 15).

Idempotent: a re-run strips the old block and re-appends an identical one -
no file changes. An **empty `machine/` dir is a no-op** (copies nothing, wires
nothing, exit 0). `-Uninstall` reverses everything (strip markers + delete the
copied files). Use `-WhatIf` for a dry run. Text edits are LF-preserving,
BOM-free, and refuse to touch a tree with unbalanced markers.

```powershell
# from the repo root (runs from plain PowerShell 5.1, UNC-safe):
powershell -NoProfile -ExecutionPolicy Bypass -File tools\qemu-b310e\scripts\install-machine.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File tools\qemu-b310e\scripts\install-machine.ps1 -QemuSrc "$env:USERPROFILE\qemu-b310e\qemu-src" -WhatIf
powershell -NoProfile -ExecutionPolicy Bypass -File tools\qemu-b310e\scripts\install-machine.ps1 -Uninstall
```

## build-qemu.ps1

Idempotent full recipe, defaults per the plan (todos 6-8):

1. **Clone-or-pin** - if `qemu-src` is missing, shallow-clone
   `https://gitlab.com/qemu-project/qemu` at tag `v11.1.0`; if present, verify
   `git describe --tags` pins `v11.1.0*` (a wrong tree aborts with
   instructions instead of clobbering).
2. **Install** - runs `install-machine.ps1` (no-op until Wave 3 drops files).
3. **Configure** - `./configure --target-list=arm-softmmu --enable-png
   --disable-werror` (PNG is required for `screendump`; libpng is an MSYS2
   `mingw-w64-x86_64-libpng` package). Skipped when `build/config-host.mak`
   exists and the args fingerprint matches - re-runs don't reconfigure.
4. **Build** - `make -j<N>` inside the MINGW64 shell (MSYS2, default
   `C:\msys64`). Long-running steps have bounded waits (`-BuildTimeoutSec`,
   default 1800 s) with a kill + log tail on timeout; failures print the log
   tail and exit nonzero.
5. **Sanity** - `build/qemu-system-arm.exe` exists and prints the
   `QEMU emulator version 11.1.0` banner; `CONFIG_PNG=y` confirmed.

**Elevation is required for the build** (todo-8 lesson): meson's postconf
`symlink-install-tree.py` calls `os.symlink()` and fails with WinError 1314
unless the token has SeCreateSymbolicLinkPrivilege (run the shell elevated).
The script detects a non-elevated session, prints a prominent warning, and
continues; use `-AutoElevate` to relaunch elevated (UAC prompt) automatically.

```powershell
# full build (run from an elevated PowerShell, or add -AutoElevate):
powershell -NoProfile -ExecutionPolicy Bypass -File tools\qemu-b310e\scripts\build-qemu.ps1
# partial / dry runs:
... build-qemu.ps1 -WhatIf                      # print the plan, run nothing
... build-qemu.ps1 -SkipConfigure -SkipBuild    # only clone-or-pin + install
... build-qemu.ps1 -Jobs 16 -BuildTimeoutSec 3600
```

Build logs land in `<home>\qemu-b310e\` (`clone.log`, `configure.log`,
`build.log`, `sanity.log`) - outside the repo, consistent with the todo-8
artifacts.
