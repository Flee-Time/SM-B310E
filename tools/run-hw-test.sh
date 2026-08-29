#!/usr/bin/env bash
# B310E-OS hardware test harness
# Usage (from repo root):
#   tools/run-hw-test.sh
#
# What it does:
#   1. Loads os.bin into the phone's RAM via spd_dump (boot key = CENTER held
#      while connecting USB). NO flash is written - zero brick risk.
#   2. Prints exactly what to expect next on the phone screen + libc_server.
#
# Environment variables (optional overrides):
#   SPD_DIR   - spd_dump folder (default: <repo>/tools/spd_dump)
#   OS_BIN    - firmware image (default: <repo>/build/bin/os.bin)
set -euo pipefail

repo=$(cd "$(dirname "$0")/.." && pwd)
spd=${SPD_DIR:-"$repo/tools/spd_dump"}
osbin=${OS_BIN:-"$repo/build/bin/os.bin"}

if [[ ! -f "$spd/spd_dump" && ! -f "$spd/spd_dump.exe" ]]; then
    echo "ERROR: spd_dump not found in $spd (looked for spd_dump and spd_dump.exe)" >&2
    exit 1
fi
if [[ ! -f "$spd/nor_fdl1.bin" ]]; then
    echo "ERROR: nor_fdl1.bin not found in $spd" >&2
    exit 1
fi
if [[ ! -f "$osbin" ]]; then
    echo "ERROR: $osbin not found - run 'make' first (see BUILD.md)" >&2
    exit 1
fi
osbin=$(cd "$(dirname "$osbin")" && pwd)/$(basename "$osbin")

echo "=== B310E-OS hardware test ==="
echo "Image : $osbin ($(wc -c < "$osbin") bytes)"
echo "SHA256: $(sha256sum "$osbin" | awk '{print $1}')"
echo ""
echo "NOW: remove battery, hold the D-pad CENTER key, plug USB, keep holding."
echo "spd_dump is waiting for the phone (USB 1782:4d00)..."
echo ""

if (
    cd "$spd"
    if [[ -x ./spd_dump ]]; then
        ./spd_dump fdl nor_fdl1.bin 0x40004000 fdl "$osbin" ram
    else
        ./spd_dump.exe fdl nor_fdl1.bin 0x40004000 fdl "$osbin" ram
    fi
); then
    code=0
else
    code=$?
fi

echo ""
if [[ $code -ne 0 ]]; then
    echo "FAIL: spd_dump exited with code $code (see output above)."
    echo "  - No 'CHIP ID' reply -> boot key wrong / USB driver (1782:4d00)."
    echo "  - 'timeout reached'   -> code crashed before the fdl_ack (report the console)."
    exit $code
fi

echo "PASS: spd_dump completed - our firmware is running in RAM."
echo ""
echo "Next, check the PHONE SCREEN:"
echo "  Expected: blue background, white 'B310E-OS v0.4', green 'kernel: alive',"
echo "            blinking pixel bottom-right. (No colorful noise.)"
echo ""
echo "Then start the USB log console (new terminal, from <repo>/tools/libc_server):"
echo "  ./libc_server"
echo "  Expected: !!! banner: up, !!! keypad: up, !!! banner: tick 32/64...,"
echo "            !!! key: <NAME> when you press keys."
echo ""
echo "AUDIO TEST (first integration):"
echo "  Press LSOFT once -> expect a short 440 Hz square-wave beep from the"
echo "  speaker (drivers/audio.c: powers on-die codec + PA, toggles DAC gain)."
echo "  If silent: report whether the phone hangs (bad register), or hear only a"
echo "  click/pop (codec powered, amp/GPIO wrong -> run the AMP_EN GPIO probe)."
echo ""
echo "Reboot (remove battery) to return to the stock Samsung OS - nothing was flashed."
exit 0
