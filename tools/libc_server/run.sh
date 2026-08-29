#!/usr/bin/env bash
# Fast loop: prebuilt USB fpdoom + doom1.wad over USB. NO SD card.
# Then run:  madctl-test.bat 0xNN  (fptest pattern)  or edit --mac below.
set -euo pipefail

scriptDir=$(cd "$(dirname "$0")" && pwd)
cd "$scriptDir"

if ! ./spd_dump \
    fdl nor_fdl1.bin 0x40004000 \
    fdl fpdoom.bin ram; then
    rc=$?
    echo "ERROR: spd_dump failed with exit code $rc" >&2
    exit "$rc"
fi

cd workdir
../libc_server -- --bright 50 --rotate 2 --mac 0x10 doom
