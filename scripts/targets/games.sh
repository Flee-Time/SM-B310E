#!/usr/bin/env bash
# Build every supported port from pinned sources with the B310E save/audio overlay.
set -euo pipefail
fail() { echo "error: $*" >&2; exit 1; }
Repo="$(cd "$(dirname "$0")/../.." && pwd)"
Fpdoom="${FPDOOM_DIR:-$Repo/build/fpdoom}"
Out="$Repo/build/games"
Prepared="$Repo/build/game-ports"
ScriptDir="$(cd "$(dirname "$0")" && pwd)"
Python="${B310E_PYTHON:-python3}"
HostCC=gcc # ensure_fpdoom adds the configured host compiler directory to PATH.
source "$ScriptDir/fpdoom.sh"
ensure_fpdoom
if [ -n "${B310E_TOOLCHAIN:-}" ]; then PATH="$B310E_TOOLCHAIN:$PATH"; fi
mkdir -p "$Fpdoom/pack_reloc/inc" "$Out" "$Repo/sdcard/fpbin"
cp "$Repo/tools/pack_reloc/inc/elf.h" "$Fpdoom/pack_reloc/inc/elf.h"
make -C "$Fpdoom/pack_reloc" CC="$HostCC" "CFLAGS=-O2 -Wall -Wextra -std=c99 -pedantic -Wno-unused -I inc"
bash "$ScriptDir/fetch-games.sh"
"$Python" "$Repo/ports/games/fetch.py" "$Fpdoom" --curl "${B310E_CURL:-curl}"
"$Python" "$Repo/ports/games/prepare.py" "$Fpdoom"
games=(
"fpdoom|fpdoom|"
"chocolate-doom|chocolate-doom|GAME=doom"
"chocolate-heretic|chocolate-doom|GAME=heretic"
"chocolate-hexen|chocolate-doom|GAME=hexen"
"retris|retris|"
"gnuboy|gnuboy|"
"infones|infones|"
"snes9x|snes9x|USE_16BIT=0"
"snes9x_16bit|snes9x|USE_16BIT=1"
"wolf3d|wolf3d|"
"wolf3d_sw|wolf3d|"
"wolf3d_apo|wolf3d|"
"wolf3d_v11|wolf3d|GAMEVER=UPLOAD"
"fpduke3d|fpbuild|GAME=duke3d"
"fpsw|fpbuild|GAME=sw"
"fpblood|fpbuild|GAME=blood"
)
for game in "${games[@]}"; do
    IFS='|' read -r name dir vars <<< "$game"
    echo "building $name"
    # Each variant shares upstream object directories; clean with its own flags.
    # Intentional word splitting passes the fixed make-variable list above.
    # shellcheck disable=SC2086
    make -C "$Prepared/$dir" clean CHIP=3 LIBC_SDIO=3 TOOLCHAIN=arm-none-eabi NAME="$name" $vars
    # Host table generators require the object directory before a parallel build.
    # shellcheck disable=SC2086
    make -C "$Prepared/$dir" objdir CHIP=3 LIBC_SDIO=3 TOOLCHAIN=arm-none-eabi NAME="$name" $vars
    # shellcheck disable=SC2086
    make -C "$Prepared/$dir" -j"${B310E_BUILD_JOBS:-8}" CHIP=3 LIBC_SDIO=3 \
        TOOLCHAIN=arm-none-eabi HOSTCC="$HostCC" NAME="$name" $vars
    cp "$Prepared/$dir/$name.bin" "$Out/$name.bin"
    # Preserve the matching symbols before the next variant replaces its objects.
    cp "$Prepared/$dir/obj3"*/"$name"_part2.elf "$Out/$name.elf"
done
# Stage only this build's declared outputs; an old cached binary cannot sneak in.
for game in "${games[@]}"; do
    IFS='|' read -r name _ _ <<< "$game"
    cp "$Out/$name.bin" "$Repo/sdcard/fpbin/$name.bin"
done
echo "staged game binaries into $Repo/sdcard/fpbin"
