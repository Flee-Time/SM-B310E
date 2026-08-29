#!/usr/bin/env bash
# diag-pack.sh - bash port of tools/stockram/diag-pack.ps1
#
# Builds stock-ram.bin (via pack-stockram.sh) and splices three diagnostic
# pieces into the image:
#   1. vectors.s   at image offset 0x000  - exception vector table (all
#      exceptions -> keylight OFF + hang). The PBL boot vector lives here
#      but never runs in our RAM-boot, so the region is free.
#   2. aliastest.s at image offset 0x100 - alias self-test entered by the
#      shim's `bx 0x100`: keylight OFF ~0.8s then ON, then jumps to the
#      real boot vector at 0x10000.
#   3. diag-stub.s at image offset 0xE000 - the instrumented stock entry
#      (boot vector 0x10020 patched 0xbf150 -> 0xE000).
# Produces stock-ram-diag.bin. See docs/stockram.md for the marker table.
#
# Usage:  tools/stockram/diag-pack.sh   (or bash tools/stockram/diag-pack.sh)
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

imgoff=$((0x800))              # image starts at this file offset (shim size)
vec=$((imgoff + 0x10020))      # main OS boot vector entry pointer

# assemble a .s file into a .bin (returns the byte size on stdout)
assemble_s() {
    local name=$1
    local s="$scriptdir/$name.s"
    local o="$scriptdir/$name.o"
    local b="$scriptdir/$name.bin"
    arm-none-eabi-gcc -c -march=armv5te -o "$o" "$s"
    arm-none-eabi-objcopy -O binary -j .text "$o" "$b"
    wc -c < "$b"
}

# 1. fresh base image
bash "$scriptdir/pack-stockram.sh"

# 2. assemble the diagnostic pieces
vectors_len=$(assemble_s vectors)
aliast_len=$(assemble_s aliastest)
stub_len=$(assemble_s diag-stub)

# 3. splice into the image
src="$repo/stock-ram.bin"
out="$repo/stock-ram-diag.bin"
cp "$src" "$out"
dd if="$scriptdir/vectors.bin" of="$out" bs=1 seek=$((imgoff + 0x000)) conv=notrunc status=none
dd if="$scriptdir/aliastest.bin" of="$out" bs=1 seek=$((imgoff + 0x100)) conv=notrunc status=none
dd if="$scriptdir/diag-stub.bin" of="$out" bs=1 seek=$((imgoff + 0xE000)) conv=notrunc status=none
printf '\x00\xe0\x00\x00' | dd of="$out" bs=1 seek="$vec" conv=notrunc status=none

echo "==> wrote $out ($(wc -c < "$out") bytes)"
echo "==> boot vector -> 0xE000; vectors @0x0 (${vectors_len}B), alias test @0x100 (${aliast_len}B), stub @0xE000 (${stub_len}B)"
echo '==> test with:  .\spd_dump.exe fdl nor_fdl1.bin 0x40004000 fdl ..\stock-ram-diag.bin ram'
echo '==> KEYLIGHT (watch ~2s): ON ~0.3s then stays ON = aliased exec BROKEN;'
echo '==>   ON -> OFF ~0.6s -> ON(final) = WORKS + match/assert; -> OFF(final) = WORKS + mismatch/full-boot;'
echo '==>   ON -> OFF immediately = exception (vector table)'
