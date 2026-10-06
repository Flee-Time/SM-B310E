#!/usr/bin/env bash
# Build in a dedicated directory; never overwrite or reset the fpdoom sources.
set -euo pipefail
Repo="$(cd "$(dirname "$0")/../.." && pwd)"
ScriptDir="$Repo/tools/fpmain"
Fpdoom="${FPDOOM_DIR:-$Repo/build/fpdoom}"
Ref=04f19d6d54430693d00029970c51cd988c083b05
if [ -n "${B310E_TOOLCHAIN:-}" ]; then PATH="$B310E_TOOLCHAIN:$PATH"; fi
if [ -n "${B310E_HOST_CC:-}" ]; then PATH="$B310E_HOST_CC:$PATH"; fi
export PATH
command -v arm-none-eabi-gcc >/dev/null || { echo "arm-none-eabi-gcc missing; set B310E_TOOLCHAIN" >&2; exit 1; }
if [ ! -d "$Fpdoom/.git" ]; then
    mkdir -p "$(dirname "$Fpdoom")"
    git clone https://github.com/ilyakurdyukov/fpdoom "$Fpdoom"
    git -C "$Fpdoom" checkout --detach "$Ref"
fi
if [ "$(git -c safe.directory="$(cd "$Fpdoom" && pwd)" -C "$Fpdoom" rev-parse HEAD)" != "$Ref" ]; then
    echo "fpdoom revision differs from $Ref; set FPDOOM_DIR to a clone at that revision" >&2
    exit 1
fi
Fpmenu="$Fpdoom/fpmain-b310e"
mkdir -p "$Fpmenu/sys" "$Fpdoom/pack_reloc/inc"
cp "$Repo/tools/pack_reloc/inc/elf.h" "$Fpdoom/pack_reloc/inc/elf.h"
make -C "$Fpdoom/pack_reloc" CC=gcc "CFLAGS=-O2 -Wall -Wextra -std=c99 -pedantic -Wno-unused -I inc"
for name in Makefile start3.s start3_t117.s readbin.c; do cp "$Fpdoom/fpmenu/$name" "$Fpmenu/$name"; done
for name in main.c menu_stub.s font5x7.h launchargs.h jsonconf.h; do cp "$ScriptDir/$name" "$Fpmenu/$name"; done
cp -R "$Fpdoom/fpdoom/." "$Fpmenu/sys/"
cp "$ScriptDir/jsonconf.h" "$Fpmenu/sys/jsonconf.h"
git apply --ignore-space-change --unsafe-paths --directory="$Fpmenu/sys" "$ScriptDir/entry-json.patch"
sed -i 's/^APP_SRCS = main[[:space:]]*$/APP_SRCS = main menu_stub/' "$Fpmenu/Makefile"
# Force a full rebuild: neither stale objects nor a previous LIBC_SDIO value
# can enter the card build. CHIP=3 explicitly selects the B310E SC6530 path.
make -B -C "$Fpmenu" NAME=fpmain CHIP=3 LIBC_SDIO=3 SYSDIR=sys TOOLCHAIN=arm-none-eabi NM=arm-none-eabi-nm
cp "$Fpmenu/fpmain.bin" "$ScriptDir/fpmain.bin"
Card="$Repo/sdcard/fpbin"
mkdir -p "$Card"
cp "$Fpmenu/fpmain.bin" "$Card/fpmain.bin"
cp "$ScriptDir/config.json" "$Card/config.json"
echo "built and staged: $Card/fpmain.bin (JSON only, SC6530, SD boot)"
