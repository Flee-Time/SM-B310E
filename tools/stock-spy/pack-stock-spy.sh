#!/usr/bin/env bash
# pack-stock-spy.sh - bash port of tools/stock-spy/pack-stock-spy.ps1
#
# Package the B310E stock firmware as a RAM-loadable image with the ADI-write
# spy: boots the STOCK OS from PSRAM and hooks its ADI write helper (flash
# 0x3038A) so every analog-register write the stock makes during the boot
# chime / song playback is logged to a PSRAM ring.
#
# Layout of stock-spy.bin (loaded by spd_dump at 0x34000000):
#   [0x000000]  shim-spy (padded to 0x800; the stockram shim + a payload
#               copy step: image[0xE000:0x10000] -> PA 0x352F0800)
#   [0x000800]  dump_firmware.bin[0..0x100000]  (1 MB stock image)
#               - offset 0xE000: the 8 KB spy payload (blank PBL region)
#               - offset 0x3038A: the ADI-helper patch
#                   ldr pc, [pc, #0] / .long 0x042F0000  (6 bytes)
#
# Usage:  tools/stock-spy/pack-stock-spy.sh   (or bash tools/stock-spy/pack-stock-spy.sh)
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

dump="$repo/dump_firmware.bin"
out="$repo/stock-spy.bin"
build="$scriptdir/build"

SHIM_PAD=$((0x800))
IMAGE_LEN=$((0x100000))
SPY_IMG_OFF=$((0xE000))     # payload slot in the image (blank PBL region)
SPY_LEN=$((0x2000))         # 8 KB payload
PATCH_OFF=$((0x3038A))      # the ADI write helper's store (str r7, [r6])
HOOK_VA=$((0x042F0000))     # payload VA = PA 0x352F0800
UPATCH=$((0x38416))         # the UART-putc store (str r5, [r4])
UHOOK_VA=$((0x042F1C00))    # uart hook VA = PA 0x352F2400

if [ ! -f "$dump" ]; then
    echo "dump_firmware.bin not found at $dump" >&2
    exit 1
fi
if [ "$(wc -c < "$dump")" -lt "$IMAGE_LEN" ]; then
    echo "dump_firmware.bin smaller than $IMAGE_LEN" >&2
    exit 1
fi
mkdir -p "$build"

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

# 1. assemble the shim-spy + the hooks
arm-none-eabi-gcc -c -march=armv5te -o "$build/shim-spy.o" "$scriptdir/shim-spy.s"
arm-none-eabi-objcopy -O binary -j .text "$build/shim-spy.o" "$build/shim-spy.bin"
shim_len=$(wc -c < "$build/shim-spy.bin")
if [ "$shim_len" -gt "$SHIM_PAD" ]; then
    echo "shim-spy.bin is $shim_len bytes, larger than the $SHIM_PAD-byte image offset" >&2
    exit 1
fi

arm-none-eabi-gcc -c -march=armv5te -mthumb -o "$build/spy-hook.o" "$scriptdir/spy-hook.s"
arm-none-eabi-objcopy -O binary -j .text "$build/spy-hook.o" "$build/spy-hook.bin"
hook_len=$(wc -c < "$build/spy-hook.bin")
if [ "$hook_len" -gt $((0x400)) ]; then
    echo "spy-hook.bin is $hook_len bytes; must fit before ring1 at payload offset 0x400" >&2
    exit 1
fi
echo "==> hook: $hook_len bytes"

arm-none-eabi-gcc -c -march=armv5te -mthumb -o "$build/spy-uart-hook.o" "$scriptdir/spy-uart-hook.s"
arm-none-eabi-objcopy -O binary -j .text "$build/spy-uart-hook.o" "$build/spy-uart-hook.bin"
uart_len=$(wc -c < "$build/spy-uart-hook.bin")
if [ "$uart_len" -gt $((0x100)) ]; then
    echo "spy-uart-hook.bin is $uart_len bytes; must fit before ring3 at payload offset 0x1D00" >&2
    exit 1
fi
echo "==> uart hook: $uart_len bytes"

# 2. build the 8 KB payload: ADI hook + rings + UART hook + ring3 + header
head -c "$SPY_LEN" /dev/zero > "$tmpdir/payload.bin"
dd if="$build/spy-hook.bin" of="$tmpdir/payload.bin" bs=1 seek=0 conv=notrunc status=none
dd if="$build/spy-uart-hook.bin" of="$tmpdir/payload.bin" bs=1 seek=$((0x1C00)) conv=notrunc status=none
# header at payload offset 0x1F00 (VA 0x042f1f00): all zeroed except the magic
printf '\x31\x53\x50\x59' | dd of="$tmpdir/payload.bin" bs=1 seek=$((0x1F18)) conv=notrunc status=none

# 3. slice + patch the stock image
head -c "$IMAGE_LEN" "$dump" > "$tmpdir/image.bin"

# 3a. verify the ADI helper store + pop at the patch site (0x6037 str r7,[r6] / 0xBDF8 pop)
site=$(dd if="$tmpdir/image.bin" bs=1 skip="$PATCH_OFF" count=4 2>/dev/null | od -An -tx1 -v | tr -d '[:space:]')
if [ "$site" != "3760f8bd" ]; then
    echo "unexpected bytes at 0x3038A: $site (expected 3760f8bd = 0x6037 str r7,[r6] / 0xBDF8 pop)" >&2
    exit 1
fi

# 3b. apply the ADI-helper patch: ldr pc, [pc, #0] + .long HOOK_VA (0x042F0000)
printf '\x00\x4f\x00\x00\x2f\x04' | dd of="$tmpdir/image.bin" bs=1 seek="$PATCH_OFF" conv=notrunc status=none

# 3c. apply the UART-putc patch at 0x38416 (store str r5,[r4]): verify + patch
usite=$(dd if="$tmpdir/image.bin" bs=1 skip="$UPATCH" count=2 2>/dev/null | od -An -tx1 -v | tr -d '[:space:]')
if [ "$usite" != "2560" ]; then
    echo "unexpected bytes at 0x38416: $usite (expected 2560 = 0x6025 str r5,[r4])" >&2
    exit 1
fi
printf '\x00\x4f\x00\x1c\x2f\x04' | dd of="$tmpdir/image.bin" bs=1 seek="$UPATCH" conv=notrunc status=none

# 3c. the payload slot (blank PBL region) must be empty flash (0xFF)
if ! dd if="$tmpdir/image.bin" bs=1 skip="$SPY_IMG_OFF" count="$SPY_LEN" 2>/dev/null \
     | od -An -tx1 -v \
     | awk '{ for (i = 1; i <= NF; i++) if ($i != "ff") { printf "  payload slot not blank: byte 0x%s\n", $i; exit 1 } }'
then
    echo "payload slot 0x$(printf '%05X' "$SPY_IMG_OFF")-0x$(printf '%05X' $((SPY_IMG_OFF + SPY_LEN))) is not blank flash" >&2
    exit 1
fi

dd if="$tmpdir/payload.bin" of="$tmpdir/image.bin" bs=1 seek="$SPY_IMG_OFF" conv=notrunc status=none

# 4. assemble stock-spy.bin = shim + image
head -c "$SHIM_PAD" /dev/zero > "$out"
dd if="$build/shim-spy.bin" of="$out" bs=1 conv=notrunc status=none
dd if="$tmpdir/image.bin" of="$out" bs=1 seek="$SHIM_PAD" conv=notrunc status=none

out_len=$(wc -c < "$out")
expected=$((SHIM_PAD + IMAGE_LEN))
if [ "$out_len" -ne "$expected" ]; then
    echo "error: $out is $out_len bytes, expected $expected" >&2
    exit 1
fi

echo "==> wrote $out ($out_len bytes: $SHIM_PAD shim + $IMAGE_LEN image, patch at 0x3038A, payload at 0xE000)"
echo '==> test with:  .\spd_dump.exe fdl nor_fdl1.bin 0x40004000 fdl ..\stock-spy.bin ram'
echo '==> after the chime, HOLD CENTER 3 s (spy watchdog-reboots into download mode), then:'
echo '==>   .\spd_dump.exe fdl nor_fdl1.bin 0x40004000 read_mem 0x352F0800 0x2000 spy-rings.bin'
