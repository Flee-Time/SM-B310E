#!/usr/bin/env bash
# Build the B310E OS (os/) and stage it as sdcard/payloads/b310e-os.bin, its
# loadable apps as sdcard/b310e-os/apps/*.app (Doom in apps/Doom/), its codec modules
# as sdcard/b310e-os/codecs/*.cdc.
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
mkdir -p "$REPO_DIR/sdcard/payloads" "$REPO_DIR/sdcard/b310e-os/apps"
cp -f "$BUILD_DIR/b310e-os.bin" "$REPO_DIR/sdcard/payloads/b310e-os.bin"
cp -f "$BUILD_DIR"/modules/*.app "$REPO_DIR/sdcard/b310e-os/apps/"
# Doom, Heretic, Hexen (os/ports/chocolate): built when build/chocolate-doom
# has upstream's source (python os/ports/sources.py chocolate-doom); each in
# a folder of its own, its WAD to go beside it (os/README.md, "Games")
for game in Doom Heretic Hexen; do
    app="$(echo "$game" | tr 'A-Z' 'a-z').app"
    if [ -f "$BUILD_DIR/ports/$app" ]; then
        mkdir -p "$REPO_DIR/sdcard/b310e-os/apps/$game"
        rm -f "$REPO_DIR/sdcard/b310e-os/apps/$app"
        cp -f "$BUILD_DIR/ports/$app" "$REPO_DIR/sdcard/b310e-os/apps/$game/"
    else
        echo "no $app: run python os/ports/sources.py chocolate-doom first"
    fi
done
# the emulators (os/ports/gnuboy, os/ports/infones): their ROMs go beside them
# Wolf3D (os/ports/wolf4sdl): its *.WL6 go beside it
# (a folder with an .app of its own name is an app; any other, a category)
for pair in "gnuboy:Game Boy" "infones:NES" "wolf3d:Wolf3D"; do
    app="${pair%%:*}.app" folder="${pair#*:}"
    if [ -f "$BUILD_DIR/ports/$app" ]; then
        mkdir -p "$REPO_DIR/sdcard/b310e-os/apps/$folder"
        rm -f "$REPO_DIR/sdcard/b310e-os/apps/$folder/$app"
        cp -f "$BUILD_DIR/ports/$app" "$REPO_DIR/sdcard/b310e-os/apps/$folder/$folder.app"
    else
        echo "no $app: run python os/ports/sources.py first (gnuboy, infones, wolf4sdl)"
    fi
done
ls -l "$REPO_DIR/sdcard/payloads/b310e-os.bin" "$REPO_DIR"/sdcard/b310e-os/apps/*.app "$REPO_DIR"/sdcard/b310e-os/apps/*/*.app 2>/dev/null || true
# codec modules: built from the Rockbox tree (build/rockbox, fetched by the
# rockbox target) when it is there
if ls "$BUILD_DIR"/codecs/*.cdc >/dev/null 2>&1; then
    mkdir -p "$REPO_DIR/sdcard/b310e-os/codecs"
    cp -f "$BUILD_DIR"/codecs/*.cdc "$REPO_DIR/sdcard/b310e-os/codecs/"
    ls -l "$REPO_DIR"/sdcard/b310e-os/codecs/*.cdc
else
    echo "no codec modules: build the rockbox target first (it fetches build/rockbox)"
fi
