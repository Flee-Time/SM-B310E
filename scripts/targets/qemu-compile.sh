#!/usr/bin/env bash
# Shared configure/build phase; paths are positional data arguments.
set -euo pipefail
Source="$1"
Jobs="$2"
Mode="${3:-desktop}"
Executable=qemu-system-arm
Executables=(qemu-system-arm)
Dependencies=(glib-2.0 pixman-1 libpng)
Windows=0
case "$(uname -s)" in
    MINGW*|MSYS*)
        Source="$(cygpath -u "$Source")"
        export PATH=/mingw64/bin:/usr/bin:$PATH
        Executable=qemu-system-arm.exe
        Windows=1
        Executables=(qemu-system-arm.exe)
        if [ "$Mode" = desktop ]; then
            Executables+=(qemu-system-armw.exe)
        fi
        ;;
esac
cd "$Source"
options='--target-list=arm-softmmu --enable-png --disable-werror --disable-docs --disable-tools --disable-guest-agent --enable-download'
case "$Mode" in
    desktop)
        options+=' --enable-gtk --enable-sdl --enable-opengl --enable-vnc'
        if [ "$Windows" = 1 ]; then
            # DirectSound initializes capture even on playback-only machines.
            # SDL only opens streams requested by the emulated device.
            options+=' --audio-drv-list=sdl'
        fi
        Dependencies+=(gtk+-3.0 sdl2 epoxy)
        ;;
    headless) options+=' --disable-gtk --disable-sdl --disable-opengl' ;;
    *) echo "Unknown QEMU display mode: $Mode" >&2; exit 1 ;;
esac
for Dependency in "${Dependencies[@]}"; do
    pkg-config --exists "$Dependency" || {
        echo "Missing QEMU dependency: $Dependency; install the packages in docs/build.md." >&2
        exit 1
    }
done
if [ ! -f build/config-host.mak ] || [ ! -f build/b310e-configure-options ] || [ "$(cat build/b310e-configure-options)" != "$options" ]; then
    ./configure $options > b310e-configure.log 2>&1 || { tail -60 b310e-configure.log; exit 1; }
    printf '%s' "$options" > build/b310e-configure-options
fi
ninja -C build -j"$Jobs" "${Executables[@]}" > b310e-build.log 2>&1 || { tail -60 b310e-build.log; exit 1; }
tail -8 b310e-build.log
"build/$Executable" --version
