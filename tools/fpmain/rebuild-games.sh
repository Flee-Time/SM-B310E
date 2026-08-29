#!/usr/bin/env bash
# B310E-OS - tools/fpmain/rebuild-games.sh
#
# Rebuilds the fpdoom game binaries for the B310E SD card with FAT_WRITE=0
# (read-only FAT) so the phone can never corrupt the card again. The two
# emulators that must save games - gnuboy (GBC) and snes9x/snes9x_16bit
# (SNES) - are built with FAT_WRITE=1.
#
# Linux port of tools/fpmain/rebuild-games.ps1. Prereqs: arm-none-eabi-gcc,
# make, git, curl on PATH (toolchain via $B310E_TOOLCHAIN or /usr/bin), and
# p7zip (the `7z` CLI) to extract the prebuilt fix14 binaries. fetch-sources.sh
# is delegated to as a child process for the source downloads + patches.
#
# Usage: tools/fpmain/rebuild-games.sh
#
# Outputs the rebuilt .bin files into tools/fpmain/games-out/ and copies
# them into sdcard/fpbin/.

set -euo pipefail

fail() { echo "error: $*" >&2; exit 1; }

Repo="$(cd "$(dirname "$0")/../.." && pwd)"
# The fpdoom clone lives in the repo's build/ tree (gitignored, removed by
# `make clean`) - cloned here automatically if missing. Override with FPDOOM_DIR.
Fpdoom="${FPDOOM_DIR:-$Repo/build/fpdoom}"
Out="$(cd "$(dirname "$0")" && pwd)/games-out"
SdBin="$Repo/sdcard/fpbin"
ScriptDir="$(cd "$(dirname "$0")" && pwd)"

# The prebuilt fix14 bins (infones/fpsw/fpduke3d/snes9x/snes9x_16bit) are
# extracted from a 7z archive downloaded from the fpdoom releases - that is a
# hard requirement, so fail fast with a clear message instead of after the
# multi-minute game builds.
if ! command -v 7z >/dev/null 2>&1; then
    fail "7z is required to extract the prebuilt game binaries (infones/fpsw/fpduke3d/snes9x). Install p7zip (or the 7-Zip CLI) and put 7z on PATH."
fi

if [ ! -d "$Fpdoom/.git" ]; then
    echo "cloning fpdoom -> $Fpdoom"
    mkdir -p "$(dirname "$Fpdoom")"
    git clone https://github.com/ilyakurdyukov/fpdoom "$Fpdoom" || fail "fpdoom clone failed"
fi
if [ ! -x "$Fpdoom/pack_reloc/pack_reloc" ]; then
    echo "building fpdoom pack_reloc"
    # the clone's pack_reloc needs inc/elf.h (the build host may lack it) -
    # vendored in the repo at tools/pack_reloc/inc/elf.h
    mkdir -p "$Fpdoom/pack_reloc/inc"
    cp "$Repo/tools/pack_reloc/inc/elf.h" "$Fpdoom/pack_reloc/inc/elf.h"
    ( cd "$Fpdoom/pack_reloc" && make CC=gcc "CFLAGS=-O2 -Wall -Wextra -std=c99 -pedantic -Wno-unused -I inc" ) \
        || fail "pack_reloc build failed"
fi
if [ ! -f "$Fpdoom/build_sc6531.make" ]; then
    fail "fpdoom clone incomplete at $Fpdoom - missing build_sc6531.make"
fi

# toolchain on PATH: on Linux arm-none-eabi-* usually lives in /usr/bin (already
# on PATH); an explicit override can be prepended via B310E_TOOLCHAIN.
if [ -n "${B310E_TOOLCHAIN:-}" ]; then
    PATH="$B310E_TOOLCHAIN:$PATH"
fi

# sources: dir, make targets, build NAME=... GAME=...
# NOTE: fpblood, wolf3d, wolf3d_sw removed 2026-08-28 (don't work; no prebuilt
# replacement). infones/fpsw/fpduke3d AND snes9x/snes9x_16bit are also NOT
# locally built - our builds don't work on the B310E (GCC-14 port
# incompatibilities on a fresh clone), but the fpdoom repo's prebuilt_fix14.7z
# ones do, so all five are copied from there (see the prebuilt section below).
games=(
"fpdoom|fpdoom|"
"chocolate-doom|chocolate-doom|GAME=doom"
"chocolate-heretic|chocolate-doom|GAME=heretic"
"chocolate-hexen|chocolate-doom|GAME=hexen"
"retris|retris|"
"gnuboy|gnuboy|FAT_WRITE=1"
)

# Bins copied from the fpdoom repo's prebuilt_fix14.7z release asset (our
# builds of these don't work on the B310E; the prebuilt ones do). Pinned to
# release 1.20251101. Downloaded once and cached; extracted into $Out so the
# staging loop below picks them up.
PrebuiltRelease="1.20251101"
PrebuiltUrl="https://github.com/ilyakurdyukov/fpdoom/releases/download/$PrebuiltRelease/prebuilt_fix14.7z"
PrebuiltCache="$Repo/build/prebuilt_fix14.7z"
PrebuiltBins=(infones.bin fpsw.bin fpduke3d.bin snes9x.bin snes9x_16bit.bin)
PrebuiltExtract="$Out/prebuilt-extract"

mkdir -p "$Out"

# 0. download + patch all game sources. This is DELEGATED to
# tools/fpmain/fetch-sources.sh - the old in-line `make -f helper.make
# ZIPDIR=. all patch` loop is BROKEN (helper.make's patch target re-extracts
# with partial src/* globs that miss subdirs like src/doom, overwriting the
# full tree; fetch-sources.sh does a full unzip + direct patch -p1, which is
# the only path that works on a fresh clone). fetch-sources.sh skips sources
# already present, so re-runs are no-ops.
# Run as a CHILD process so its `set -e`/fail behavior cannot abort THIS
# script's error handling and it returns a real exit code.
"$ScriptDir/fetch-sources.sh" || fail "fetch-sources.sh failed (exit $?)"

# 1. build each game
for g in "${games[@]}"; do
    IFS='|' read -r name makedir vars <<< "$g"
    dir="$Fpdoom/$makedir"
    bin="$dir/$name.bin"
    dest="$Out/$name.bin"
    echo "=== build $name [$vars] ==="
    (
        cd "$dir" || fail "cannot cd to $dir"
        # GCC 14 makes -Wincompatible-pointer-types a hard ERROR; the fpdoom
        # ports were written for older GCCs where it was a warning. The clone
        # is not restored between game builds, so patch each port's Makefile
        # once (marker-guarded, idempotent) to downgrade it. The fpdoom
        # release CI hits the same wall on modern GCC.
        mk="$dir/Makefile"
        if [ -f "$mk" ] && ! grep -q 'Wno-error=incompatible-pointer-types' "$mk"; then
            echo 'CFLAGS += -Wno-error=incompatible-pointer-types' >> "$mk"
            echo "  patched Makefile: -Wno-error=incompatible-pointer-types (GCC 14 compat)"
        fi
        # snes9x port defines time() but omits <time.h> (works on old glibc
        # where stdio.h pulled it in; fails on the B310E toolchain).
        sfp="$dir/snes9x_fp.c"
        if [ -f "$sfp" ] && ! grep -q '#include <time.h>' "$sfp"; then
            sed -i '0,/#include <stdio.h>/s//#include <stdio.h>\n#include <time.h>/' "$sfp"
            echo "  patched snes9x_fp.c: #include <time.h>"
        fi
        make clean LIBC_SDIO=3 TOOLCHAIN=arm-none-eabi CHIP=3 || true
        # $vars splits on whitespace so multi-token vars (e.g. "FAT_WRITE=1
        # NAME=snes9x_16bit") reach make as SEPARATE arguments - an unquoted
        # expansion performs the word-splitting PowerShell lacks.
        # shellcheck disable=SC2086
        make all LIBC_SDIO=3 TOOLCHAIN=arm-none-eabi CHIP=3 $vars || fail "build failed (exit $?)"
    )
    cp "$bin" "$dest"
    echo "  -> $dest ($(stat -c %s "$dest") B)"
done

# 1.5 prebuilt bins (infones/fpsw/fpduke3d - our builds don't work on the
# B310E, the fpdoom prebuilt_fix14 release ones do). Download once (cached),
# extract into $Out so the staging loop picks them up.
have_prebuilt=true
for b in "${PrebuiltBins[@]}"; do
    if [ ! -f "$Out/$b" ]; then have_prebuilt=false; break; fi
done
if [ "$have_prebuilt" != true ]; then
    echo "=== prebuilt: fetching $PrebuiltUrl ==="
    if [ ! -f "$PrebuiltCache" ]; then
        curl -sL -o "$PrebuiltCache" "$PrebuiltUrl" || fail "prebuilt download failed (exit $?)"
    fi
    mkdir -p "$PrebuiltExtract"
    for b in "${PrebuiltBins[@]}"; do
        7z x "$PrebuiltCache" -o"$PrebuiltExtract" "sdcard/fpbin/$b" >/dev/null || fail "prebuilt extract failed: $b"
        cp "$PrebuiltExtract/sdcard/fpbin/$b" "$Out/$b"
        echo "  -> $b ($(stat -c %s "$Out/$b") B, prebuilt)"
    done
    rm -rf "$PrebuiltExtract"
else
    echo "=== prebuilt: already present (infones/fpsw/fpduke3d) ==="
fi

# 2. stage into sdcard/fpbin
mkdir -p "$SdBin"
for f in "$Out"/*.bin; do
    [ -e "$f" ] || continue
    cp "$f" "$SdBin/$(basename "$f")"
done
echo "staged into: $SdBin"
