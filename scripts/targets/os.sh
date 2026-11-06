#!/usr/bin/env bash
# Build the B310E OS (os/) and stage it as sdcard/progs/b310e-os.bin, its
# loadable apps as sdcard/b310e-os/apps/*.app.
# FatFs R0.15a (ChaN, 1-clause BSD) is fetched once into build/fatfs and
# checked against its SHA-256; only the sources are unpacked (the OS has
# its own os/fs/ffconf.h).
set -euo pipefail
REPO_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
TOOLCHAIN="${B310E_TOOLCHAIN:-$(dirname "$(command -v arm-none-eabi-gcc)")}"
export PATH="$TOOLCHAIN:$PATH"
BUILD_DIR="$REPO_DIR/build/os"
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

make -C "$REPO_DIR/os" BUILD="$BUILD_DIR" FATFS="$FATFS_DIR/source" \
    PYTHON="${B310E_PYTHON:-python3}" -j"${B310E_BUILD_JOBS:-8}"
mkdir -p "$REPO_DIR/sdcard/progs" "$REPO_DIR/sdcard/b310e-os/apps"
cp -f "$BUILD_DIR/b310e-os.bin" "$REPO_DIR/sdcard/progs/b310e-os.bin"
cp -f "$BUILD_DIR"/modules/*.app "$REPO_DIR/sdcard/b310e-os/apps/"
ls -l "$REPO_DIR/sdcard/progs/b310e-os.bin" "$REPO_DIR"/sdcard/b310e-os/apps/*.app
