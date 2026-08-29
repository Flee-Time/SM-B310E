#!/usr/bin/env bash
# B310E-OS - tools/fpmain/build-fpmain.sh
#
# Builds the PORTED fpmain.bin: our boot menu (main.c + menu_stub.s +
# font8x16.h from this directory) staged into the clean fpdoom clone's
# fpmenu/ app, built with the fpdoom framework, then copied back here and
# staged on the card (sdcard/fpbin/fpmain.bin). The clone is restored to
# pristine after the build (git checkout), so the reference stays clean.
#
# Linux port of tools/fpmain/build-fpmain.ps1. Prereqs: arm-none-eabi-gcc,
# make, git, gcc and sh on PATH (the toolchain bin dir is prepended to PATH
# automatically; override with B310E_TOOLCHAIN). pack_reloc is built once in
# fpdoom/pack_reloc/ (the clone's Makefile needs inc/elf.h, vendored in the
# repo at tools/pack_reloc/inc/elf.h).
#
# Usage:  tools/fpmain/build-fpmain.sh

set -euo pipefail

fail() { echo "error: $*" >&2; exit 1; }

Repo="$(cd "$(dirname "$0")/../.." && pwd)"
# The fpdoom clone lives in the repo's build/ tree (gitignored, removed by
# `make clean`) - cloned here automatically if missing. Override the location
# with FPDOOM_DIR.
Fpdoom="${FPDOOM_DIR:-$Repo/build/fpdoom}"
Fpmenu="$Fpdoom/fpmenu"
ScriptDir="$(cd "$(dirname "$0")" && pwd)"

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
if [ ! -f "$Fpmenu/Makefile" ]; then
    fail "fpdoom clone incomplete at $Fpdoom - missing fpmenu/Makefile"
fi

# 1. stage our sources over the stock fpmenu app sources
cp "$ScriptDir/main.c"      "$Fpmenu/main.c"
cp "$ScriptDir/menu_stub.s" "$Fpmenu/menu_stub.s"
cp "$ScriptDir/font8x16.h"  "$Fpmenu/font8x16.h"
cp "$ScriptDir/font5x7.h"   "$Fpmenu/font5x7.h"
cp "$ScriptDir/readconf.h"  "$Fpmenu/readconf.h"
cp "$ScriptDir/jsonconf.h"  "$Fpmenu/jsonconf.h"

# 2. add menu_stub to the part1 app sources (stub symbols then get the
#    part1 relocations, so main.c's memcpy sees the runtime addresses)
mk="$Fpmenu/Makefile"
sed -i 's/^APP_SRCS = main[[:space:]]*$/APP_SRCS = main menu_stub/' "$mk"

# 3. build (sh on PATH provides the Makefile's mkdir -p/cat)
( cd "$Fpmenu" && make NAME=fpmain LIBC_SDIO=3 TOOLCHAIN=arm-none-eabi ) \
    || fail "make failed (exit $?)"

# 4. copy the result back + stage on the card
Out="$ScriptDir/fpmain.bin"
cp "$Fpmenu/fpmain.bin" "$Out"
Card="$Repo/sdcard/fpbin"
mkdir -p "$Card"
cp "$Out" "$Card/fpmain.bin"
# stage the menu configs too (config.json is the primary, config.txt legacy)
cp "$ScriptDir/config.json" "$Card/config.json"
# config.txt (the legacy line-based PORTS menu) is NOT staged by default -
# a fallback the user can find and copy manually if wanted.
# if [ -f "$ScriptDir/config.txt" ]; then
#     cp "$ScriptDir/config.txt" "$Card/config.txt"
# fi
echo "built: $Out"

# 5. restore the clone (tracked files) + drop build artifacts
( cd "$Fpdoom" && git checkout -- fpmenu ) || fail "clone restore (git checkout) failed"
rm -rf "$Fpmenu/obj0"
rm -f  "$Fpmenu/fpmain.bin" "$Fpmenu/menu_stub.s" "$Fpmenu/font5x7.h"
# NOTE: readconf.h is NOT removed here - it is a TRACKED file in the
# fpdoom repo (git checkout -- fpmenu above restored it).
echo "clone restored; card staged: $Card/fpmain.bin"
