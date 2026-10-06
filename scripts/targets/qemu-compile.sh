#!/usr/bin/env bash
# Shared configure/build phase; paths are positional data arguments.
set -euo pipefail
Source="$1"
Jobs="$2"
Executable=qemu-system-arm
case "$(uname -s)" in
    MINGW*|MSYS*)
        Source="$(cygpath -u "$Source")"
        export PATH=/mingw64/bin:/usr/bin:$PATH
        Executable=qemu-system-arm.exe
        ;;
esac
cd "$Source"
options='--target-list=arm-softmmu --enable-png --disable-gtk --disable-sdl --disable-werror --disable-docs --disable-tools --disable-guest-agent --disable-download'
if [ ! -f build/config-host.mak ] || [ ! -f build/b310e-configure-options ] || [ "$(cat build/b310e-configure-options)" != "$options" ]; then
    ./configure $options > b310e-configure.log 2>&1 || { tail -60 b310e-configure.log; exit 1; }
    printf '%s' "$options" > build/b310e-configure-options
fi
ninja -C build -j"$Jobs" "$Executable" > b310e-build.log 2>&1 || { tail -60 b310e-build.log; exit 1; }
tail -8 b310e-build.log
"build/$Executable" --version
