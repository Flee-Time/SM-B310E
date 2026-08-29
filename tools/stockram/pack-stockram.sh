#!/usr/bin/env bash
# pack-stockram.sh - bash port of tools/stockram/pack-stockram.ps1
#
# Package the ORIGINAL B310E firmware as a RAM-loadable image for spd_dump,
# with an MMU-alias shim that runs the stock main OS from PSRAM instead of NOR.
#
# Layout of the produced stock-ram.bin (loaded by spd_dump at 0x34000000):
#   [0x000000]  shim  (ARM, linked @0x34000000, padded to 0x800)
#   [0x000800]  dump_firmware.bin[0 .. 0x100000]  (1MB stock image copy)
#
# Usage:  tools/stockram/pack-stockram.sh   (or bash tools/stockram/pack-stockram.sh)
# Toolchain: arm-none-eabi-* from PATH, or set B310E_TOOLCHAIN to a dir
# containing bin/ (its bin/ is prepended to PATH).

set -euo pipefail

scriptdir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$scriptdir/../.." && pwd)"

# toolchain resolution: $B310E_TOOLCHAIN overrides PATH; else rely on PATH
TOOLCHAIN="${B310E_TOOLCHAIN:-}"
if [ -n "$TOOLCHAIN" ]; then
    PATH="$TOOLCHAIN/bin:$PATH"
fi
if ! command -v arm-none-eabi-gcc >/dev/null 2>&1; then
    echo "ARM toolchain not found (set B310E_TOOLCHAIN to a dir containing bin/ or add arm-none-eabi-* to PATH)" >&2
    exit 1
fi
if ! command -v arm-none-eabi-objcopy >/dev/null 2>&1; then
    echo "ARM toolchain not found (arm-none-eabi-objcopy missing)" >&2
    exit 1
fi

src="$scriptdir/shim.s"
shim="$scriptdir/shim.bin"
dump="$repo/dump_firmware.bin"
out="$repo/stock-ram.bin"

SHIM_PAD=$((0x800))      # image offset inside stock-ram.bin (must match shim.s IMAGE_OFF)
IMAGE_LEN=$((0x100000))  # 1MB alias window (must match shim.s ALIAS_SIZE)

if [ ! -f "$dump" ]; then
    echo "dump_firmware.bin not found at $dump" >&2
    exit 1
fi
if [ "$(wc -c < "$dump")" -lt "$IMAGE_LEN" ]; then
    echo "dump_firmware.bin smaller than $IMAGE_LEN" >&2
    exit 1
fi

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

# 1. assemble the shim
arm-none-eabi-gcc -c -march=armv5te -o "$scriptdir/shim.o" "$src"
arm-none-eabi-objcopy -O binary -j .text "$scriptdir/shim.o" "$shim"

shim_len=$(wc -c < "$shim")
if [ "$shim_len" -gt "$SHIM_PAD" ]; then
    echo "shim.bin is $shim_len bytes, larger than the $SHIM_PAD-byte image offset" >&2
    exit 1
fi

# 2. slice the stock image (first 1MB of the dump)
head -c "$IMAGE_LEN" "$dump" > "$tmpdir/image.bin"

# 3. assemble stock-ram.bin = shim (padded) + image
head -c "$SHIM_PAD" /dev/zero > "$out"
dd if="$shim" of="$out" bs=1 conv=notrunc status=none
dd if="$tmpdir/image.bin" of="$out" bs=1 seek="$SHIM_PAD" conv=notrunc status=none

out_len=$(wc -c < "$out")
expected=$((SHIM_PAD + IMAGE_LEN))
if [ "$out_len" -ne "$expected" ]; then
    echo "error: $out is $out_len bytes, expected $expected" >&2
    exit 1
fi

echo "==> wrote $out ($out_len bytes: $SHIM_PAD shim + $IMAGE_LEN image)"
echo '==> test with:  .\spd_dump.exe fdl nor_fdl1.bin 0x40004000 fdl ..\stock-ram.bin ram'
