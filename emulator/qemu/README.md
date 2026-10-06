# SC6530C QEMU machine

`machine/hw/arm/b310e.c` defines the board. `machine/hw/misc/` contains
the peripheral models. `scripts/` holds machine-install helpers, captures
and regression tests. Build implementations live in `scripts/targets/`
at the repository root.

Build with `.\build.ps1 qemu` on Windows or `bash build.sh qemu` on Linux.
The shared build guide lists dependencies and `--qemu-source` overrides:
[build.md](../../docs/build.md). QEMU is pinned to v11.1.0 and builds headless
with PNG captures; no administrator privileges are required.

The stock ROM is local research data and is not committed. Current stock boot,
audio evidence and capture commands are in [emulator-audio.md](../../docs/emulator-audio.md).
The memory map and older boot-path research are in [b310e-qemu.md](../../docs/b310e-qemu.md).
Rockbox SD-image and playback checks are in [rockbox-audio.md](../../docs/rockbox-audio.md).
The DSP model implements the ARM-facing protocol, not a Teak instruction CPU.
