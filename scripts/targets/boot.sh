#!/usr/bin/env bash
# Build the B310E boot loader (boot/) as build/boot/b310e-boot.bin, the
# image for NOR 0x3fc000 (docs/sdboot.md has the install). B310E_BOOT_DEFAULT
# sets what starts with no key held: menu (the default), stock, or
# program[:path].
# FatFs R0.15a (ChaN, 1-clause BSD) is fetched once into build/fatfs and
# checked against its SHA-256, as for the OS; the loader has its own
# boot/ffconf.h.
set -euo pipefail
REPO_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
TOOLCHAIN="${B310E_TOOLCHAIN:-$(dirname "$(command -v arm-none-eabi-gcc)")}"
export PATH="$TOOLCHAIN:$PATH"
FATFS_DIR="$REPO_DIR/build/fatfs"
FATFS_URL="http://elm-chan.org/fsw/ff/arc/ff15a.zip"
FATFS_SHA256="74737b1cafa1a67a3f722dd0d1a44767c5b54d37b6300ad3825a904cbe88fc3c"

if [ ! -f "$FATFS_DIR/source/ff.c" ]; then
    mkdir -p "$FATFS_DIR"
    if [ ! -f "$FATFS_DIR/ff15a.zip" ]; then
        echo "fetching FatFs R0.15a"
        curl -sSfL -o "$FATFS_DIR/ff15a.zip.part" "$FATFS_URL"
        mv "$FATFS_DIR/ff15a.zip.part" "$FATFS_DIR/ff15a.zip"
    fi
    echo "$FATFS_SHA256  $FATFS_DIR/ff15a.zip" | sha256sum -c -
    unzip -q -o -j "$FATFS_DIR/ff15a.zip" source/ff.c source/ff.h \
        source/diskio.h source/ffunicode.c LICENSE.txt -d "$FATFS_DIR/source"
fi

make -C "$REPO_DIR/boot" BUILD="$REPO_DIR/build/boot" FATFS="$FATFS_DIR/source" \
    PYTHON="${B310E_PYTHON:-python3}" DEFAULT="${B310E_BOOT_DEFAULT:-menu}"
