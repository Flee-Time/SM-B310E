#!/usr/bin/env bash
# Build the B310E OS (os/) and stage it as sdcard/progs/b310e-os.bin.
set -euo pipefail
REPO_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
TOOLCHAIN="${B310E_TOOLCHAIN:-$(dirname "$(command -v arm-none-eabi-gcc)")}"
export PATH="$TOOLCHAIN:$PATH"
BUILD_DIR="$REPO_DIR/build/os"

make -C "$REPO_DIR/os" BUILD="$BUILD_DIR" -j"${B310E_BUILD_JOBS:-8}"
mkdir -p "$REPO_DIR/sdcard/progs"
cp -f "$BUILD_DIR/b310e-os.bin" "$REPO_DIR/sdcard/progs/b310e-os.bin"
ls -l "$REPO_DIR/sdcard/progs/b310e-os.bin"
