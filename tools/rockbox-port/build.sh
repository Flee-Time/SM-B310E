#!/usr/bin/env bash
###############################################################################
# B310E-OS Rockbox port — tools/rockbox-port/build.sh (T2.2)
# (GPLv2, Rockbox-derived)
#
# Idempotent, re-runnable pipeline:
#   a. STAGE the overlay into the Rockbox clone (firmware/**, apps/** —
#      only our paths, never overwriting other Rockbox files)
#   b. APPLY the integration edits from patches/PATCHES.md (marker-guarded:
#      each patch greps for its sentinel and skips if present; every perl
#      program first normalizes CRLF -> LF so the \n anchors match)
#   c. CONFIGURE + BUILD out-of-tree under MSYS2 bash with the
#      arm-none-eabi toolchain (configure forbids in-tree builds)
#   d. COPY rockbox.bin (+ rockbox.elf) to sdcard/progs/
#   e. LOG the full output to build.log (rotated: previous run -> build.log.1)
#
# Run:  bash tools/rockbox-port/build.sh   (or build.ps1, which wraps this)
###############################################################################
set -u

# ---- MSYS paths -----------------------------------------------------------
# Repo paths are derived from this script's location (tools/rockbox-port),
# so the port builds from any checkout.
PORT_DIR="$(cd "$(dirname "$0")" && pwd)"
# The Rockbox clone lives in the repo's build/ tree (gitignored, removed by
# `make clean`) - cloned here automatically if missing.
REPO_DIR="$(cd "$PORT_DIR/../.." && pwd)"
CLONE_DIR="$REPO_DIR/build/rockbox"
BUILD_DIR="$CLONE_DIR/build-b310e"
SDCARD_PROGS="$REPO_DIR/sdcard/progs"
# The real toolchain lives under "C:\Program Files (x86)\Arm GNU Toolchain
# arm-none-eabi\14.2 rel1" — spaces AND parens break configure's arch
# detection and make's CC line. build.sh uses a space-free junction
# C:\arm-gcc -> that dir (created once by build.ps1 if missing; override
# with B310E_TOOLCHAIN to point elsewhere).
# Platform detection: on Windows/MSYS2 (LOCALAPPDATA set / /c/arm-gcc present)
# default to the junction + the WinLibs winget cache for host tools; on Linux
# take both from PATH (override either with B310E_TOOLCHAIN / B310E_HOST_CC).
if [ -d /c/arm-gcc/bin ] || [ -n "${LOCALAPPDATA:-}" ]; then
    TOOLCHAIN="${B310E_TOOLCHAIN:-/c/arm-gcc/bin}"
    # Host compiler for Rockbox's host tools (rdf2binary, convbdf, …). The
    # MSYS2 distro has no gcc by default; use the WinLibs mingw gcc that the
    # B310E-OS host tests already use. Resolved from %LOCALAPPDATA% (the winget
    # package cache - no hardcoded user path). Override with B310E_HOST_CC.
    HOST_CC="${B310E_HOST_CC:-$(ls -d "$LOCALAPPDATA"/Microsoft/WinGet/Packages/BrechtSanders.WinLibs.POSIX.UCRT_*/mingw64/bin 2>/dev/null | head -1)}"
    MSYS_LIKE=1
else
    TOOLCHAIN="${B310E_TOOLCHAIN:-$(dirname "$(command -v arm-none-eabi-gcc 2>/dev/null)" 2>/dev/null)}"
    HOST_CC="${B310E_HOST_CC:-$(dirname "$(command -v gcc 2>/dev/null)" 2>/dev/null)}"
    MSYS_LIKE=0
fi
LOG="$PORT_DIR/build.log"

# Rotate the log (keep the previous run's tail in build.log.1), then tee
# everything to the console AND the log.
[ -f "$LOG" ] && mv -f "$LOG" "$LOG.1" 2>/dev/null || true
exec > >(tee "$LOG") 2>&1

echo "=== B310E Rockbox build $(date '+%Y-%m-%d %H:%M:%S') ==="
echo "port dir   : $PORT_DIR"
echo "clone dir  : $CLONE_DIR"
echo "build dir  : $BUILD_DIR"
echo "toolchain  : $TOOLCHAIN"

fail() { echo "BUILD FAILED: $*"; exit 1; }

# ---- 0. ENSURE the Rockbox clone (cloned into build/ on first run) --------
if [ ! -d "$CLONE_DIR/.git" ]; then
    echo "--- cloning rockbox -> $CLONE_DIR ---"
    mkdir -p "$(dirname "$CLONE_DIR")" || fail "cannot mkdir $(dirname "$CLONE_DIR")"
    git clone https://github.com/rockbox/rockbox.git "$CLONE_DIR" || fail "rockbox clone"
fi
[ -f "$CLONE_DIR/tools/configure" ] || fail "rockbox clone incomplete at $CLONE_DIR"

# ---- a. STAGE the overlay into the clone ---------------------------------
echo "--- staging overlay ---"
cp -f "$PORT_DIR/firmware/export/config/b310e.h"    "$CLONE_DIR/firmware/export/config/b310e.h"
cp -f "$PORT_DIR/firmware/export/sc6530c.h"         "$CLONE_DIR/firmware/export/sc6530c.h"
cp -rf "$PORT_DIR/firmware/target/arm/sc6530c/."    "$CLONE_DIR/firmware/target/arm/sc6530c/"
cp -f "$PORT_DIR/apps/keymaps/keymap-b310e.c"       "$CLONE_DIR/apps/keymaps/keymap-b310e.c"
echo "staged:"
(cd "$PORT_DIR" && find firmware apps -type f | sort | sed 's/^/  /')

# ---- b. APPLY the integration patches (marker-guarded, idempotent) -------
# patch_file <file> <marker> <perl -0pi program>
# CRLF normalization is prepended automatically (the clone's tracked files
# are checked out with CRLF on this machine; the \n anchors need LF).
patch_file() {
    local file="$1" marker="$2" expr="$3"
    if grep -qF "$marker" "$file" 2>/dev/null; then
        echo "patch: SKIP (already applied)  ${file##*/} [$marker]"
        return 0
    fi
    perl -0pi -e "s/\r\n/\n/g; $expr" "$file" || fail "perl patch $file"
    if grep -qF "$marker" "$file" 2>/dev/null; then
        echo "patch: OK  ${file##*/} [$marker]"
    else
        echo "patch: FAILED  ${file##*/} [$marker]"
        return 1
    fi
}

echo "--- applying integration patches ---"

# 1. tools/configure — the b310e target case (before the fallback case).
# Delete-then-insert (not marker-guarded): the case body is the source of
# truth here and must always match this file — a stale body would silently
# win because configure's case is matched top-down.
export B310E_CASE='    271|b310e)
    target_id=126
    modelname="b310e"
    target="B310E"
    memory=4
    arm926ejscc
    appextra="recorder:gui"
    plugins=""
    tool="cp "
    boottool="cp "
    output="rockbox.b310e"
    bootoutput="bootloader-b310e.bin"
    bmp2rb_mono="$rootdir/tools/bmp2rb -f 0"
    bmp2rb_native="$rootdir/tools/bmp2rb -f 4"
    toolset=$scramblebitmaptools
    t_cpu="arm"
    t_manufacturer="sc6530c"
    t_model="b310e"
    ;;
'
sed -i '/    271|b310e)/,/^    ;;$/d' "$CLONE_DIR/tools/configure"
perl -0pi -e 'my $blk=$ENV{B310E_CASE}; s/(\n   \*\)\n    echo "Please select a supported target platform!")/\n$blk$1/' "$CLONE_DIR/tools/configure"
if grep -qF "bmp2rb_mono=\"\$rootdir/tools/bmp2rb -f 0\"" "$CLONE_DIR/tools/configure" 2>/dev/null; then
    echo "patch: OK  configure [271|b310e)]"
else
    echo "patch: FAILED  configure [271|b310e)]"
    exit 1
fi

# 2. firmware/export/config.h — CONFIG_CPU SC6530C
patch_file "$CLONE_DIR/firmware/export/config.h" "#define SC6530C" \
  's/(#define S3C2440[^\n]*\n)/$1#define SC6530C      6530\n/'

# 3. firmware/export/config.h — CONFIG_KEYPAD B310E_PAD
patch_file "$CLONE_DIR/firmware/export/config.h" "#define B310E_PAD" \
  's/(#define HIDIZS_AP80MAX_PAD[^\n]*\n)/$1#define B310E_PAD        81\n/'

# 4. firmware/export/config.h — CONFIG_LCD LCD_B310E
patch_file "$CLONE_DIR/firmware/export/config.h" "#define LCD_B310E" \
  's/(#define LCD_ECHO_R1[^\n]*\n)/$1#define LCD_B310E       73\n/'

# 5. firmware/export/config.h — CONFIG_RTC RTC_SC6530
patch_file "$CLONE_DIR/firmware/export/config.h" "#define RTC_SC6530" \
  's/(#define RTC_STM32H743[^\n]*\n)/$1#define RTC_SC6530      28\n/'

# 6. firmware/export/config.h — model include chain
patch_file "$CLONE_DIR/firmware/export/config.h" '#include "config/b310e.h"' \
  's/(\n#else\n#error "unknown hardware platform!")/\n#elif defined(B310E)\n#include "config\/b310e.h"$1/'

# 7. firmware/export/cpu.h — SC6530C -> sc6530c.h
patch_file "$CLONE_DIR/firmware/export/cpu.h" '#include "sc6530c.h"' \
  's/(#include "stm32h743.h"\n)/$1#elif CONFIG_CPU == SC6530C\n#include "sc6530c.h"\n/'

# 8. firmware/export/audiohw.h — HAVE_SC6530_CODEC -> audiohw-sc6530c.h
patch_file "$CLONE_DIR/firmware/export/audiohw.h" "HAVE_SC6530_CODEC" \
  's/(#include "dummy_codec.h"\n)/$1#elif defined(HAVE_SC6530_CODEC)\n#include "audiohw-sc6530c.h"\n/'

# 9. firmware/SOURCES — mmu-arm.S list gains SC6530C (commit_dcache etc.)
patch_file "$CLONE_DIR/firmware/SOURCES" "CONFIG_CPU == SC6530C" \
  's/(\|\| CONFIG_CPU == S3C2440 \|\| CONFIG_CPU == TCC7801 \\\n)/$1   || CONFIG_CPU == SC6530C \\\n/'

# 10. firmware/SOURCES — the #ifdef B310E model block (after GIGABEAT_F).
# Delete-then-insert: the block's file list grows across iterations (rtc/
# sdio were added after the first apply) and a stale block would silently
# win the #ifdef.
export B310E_SOURCES='#ifdef B310E
drivers/lcd-memframe.c
target/arm/sc6530c/adc-sc6530c.c
target/arm/sc6530c/system-sc6530c.c
target/arm/sc6530c/kernel-sc6530c.c
target/arm/sc6530c/lcd-sc6530c.c
target/arm/sc6530c/button-sc6530c.c
target/arm/sc6530c/backlight-sc6530c.c
target/arm/sc6530c/powermgmt-sc6530c.c
target/arm/sc6530c/power-sc6530c.c
target/arm/sc6530c/sdio-sc6530c.c
target/arm/sc6530c/sd-sc6530c.c
target/arm/sc6530c/rtc-sc6530c.c
target/arm/sc6530c/timer-sc6530c.c
target/arm/sc6530c/i2c-sc6530c.c
#ifndef BOOTLOADER
target/arm/sc6530c/pcm-sc6530c.c
target/arm/sc6530c/audiohw-sc6530c.c
#endif /* !BOOTLOADER */
#endif /* B310E */
'
sed -i '/^#ifdef B310E$/,/^#endif \/\* B310E \*\/$/d' "$CLONE_DIR/firmware/SOURCES"
perl -0pi -e 'my $blk=$ENV{B310E_SOURCES}; s/(#endif \/\* GIGABEAT_F \*\/\n)/$1\n$blk/' "$CLONE_DIR/firmware/SOURCES"
if grep -qF "target/arm/sc6530c/rtc-sc6530c.c" "$CLONE_DIR/firmware/SOURCES" 2>/dev/null; then
    echo "patch: OK  SOURCES [B310E block]"
else
    echo "patch: FAILED  SOURCES [B310E block]"
    exit 1
fi

# 11. apps/SOURCES — keymap line
patch_file "$CLONE_DIR/apps/SOURCES" "keymap-b310e.c" \
  's/(keymaps\/keymap-hidizsap80max\.c\n)/$1#elif CONFIG_KEYPAD == B310E_PAD\nkeymaps\/keymap-b310e.c\n/'

# 12. tools/addtargetdir.pl — Windows-drive-path normalization. On MSYS the
# host gcc emits "C:/Users/…" dep paths while $rootdir is "/c/Users/…";
# the length-based rootdir->builddir surgery then mangles every make.dep
# target, so the parallel build gets NO header ordering and races on the
# generated headers (lang.h, sysfont.h, …). One-line normalization fixes
# make.dep and the races.
if grep -qF "B310E-PORT" "$CLONE_DIR/tools/addtargetdir.pl" 2>/dev/null; then
    echo "patch: SKIP (already applied)  addtargetdir.pl [B310E-PORT]"
else
    sed -i 's|^for (split(/\[\\s\\\\\]+/m, <STDIN>)) {|&\n# B310E-PORT: normalize Windows drive paths (gcc on MSYS emits C:/...)\n    s{^([A-Za-z]):}{"/" . lc($1)}e;|' "$CLONE_DIR/tools/addtargetdir.pl"
    if grep -qF "B310E-PORT" "$CLONE_DIR/tools/addtargetdir.pl" 2>/dev/null; then
        echo "patch: OK  addtargetdir.pl [B310E-PORT]"
    else
        echo "patch: FAILED  addtargetdir.pl [B310E-PORT]"
        exit 1
    fi
fi

# 13. firmware/target/arm/mmu-arm.S — SC6530C cache geometry. The ARM926EJ-S
# has the same 16 KB 4-way/32-byte-line D-cache as the S3C2440's ARM920T;
# without a case here mmu-arm.S #errors "Cache settings unknown". Restore
# first (a stale half-applied state would defeat the marker guard).
cd "$CLONE_DIR" && git checkout -- firmware/target/arm/mmu-arm.S 2>/dev/null || true
if grep -qF "CONFIG_CPU == SC6530C" "$CLONE_DIR/firmware/target/arm/mmu-arm.S" 2>/dev/null; then
    echo "patch: SKIP (already applied)  mmu-arm.S [CONFIG_CPU == SC6530C]"
else
    sed -z -i 's|#else\n#error Cache settings unknown for this CPU !|#elif CONFIG_CPU == SC6530C\n#define USE_MMU\n#define CACHE_SIZE 16\n\n#else\n#error Cache settings unknown for this CPU !|' "$CLONE_DIR/firmware/target/arm/mmu-arm.S"
    if grep -qF "CONFIG_CPU == SC6530C" "$CLONE_DIR/firmware/target/arm/mmu-arm.S" 2>/dev/null; then
        echo "patch: OK  mmu-arm.S [CONFIG_CPU == SC6530C]"
    else
        echo "patch: FAILED  mmu-arm.S [CONFIG_CPU == SC6530C]"
        exit 1
    fi
fi

# 14. apps/plugins/plugin.lds — SC6530C DRAM map. The codec/plugin link
# scripts #error without a per-CPU DRAMORIG/DRAMSIZE; ours mirrors the
# app.lds budget (4 MB PSRAM minus plugin/codec/LCD/TTB).
if grep -qF "CONFIG_CPU == SC6530C" "$CLONE_DIR/apps/plugins/plugin.lds" 2>/dev/null; then
    echo "patch: SKIP (already applied)  plugin.lds [CONFIG_CPU == SC6530C]"
else
    sed -i 's|#elif CONFIG_CPU==DM320|#elif CONFIG_CPU == SC6530C\n#define DRAMORIG 0x04000000\n#define DRAMSIZE ((MEMORYSIZE * 0x100000) - PLUGIN_BUFFER_SIZE \\\n                  - CODEC_SIZE - LCD_BUFFER_SIZE - TTB_SIZE)\n\n#elif CONFIG_CPU==DM320|' "$CLONE_DIR/apps/plugins/plugin.lds"
    if grep -qF "CONFIG_CPU == SC6530C" "$CLONE_DIR/apps/plugins/plugin.lds" 2>/dev/null; then
        echo "patch: OK  plugin.lds [CONFIG_CPU == SC6530C]"
    else
        echo "patch: FAILED  plugin.lds [CONFIG_CPU == SC6530C]"
        exit 1
    fi
fi

# 15. tools/functions.make — the asmdefs extractor must strip CR. gcc.exe
# (native Windows) writes CRLF assembly on MSYS, so every line ends "\r",
# the AD_<name>/.word regexes never match and every *_asmdefs.h comes out
# EMPTY — then every .S file using the exported symbols fails to assemble
# (jpeg_idct_arm.S: "invalid operands (*ABS* and *UND* sections)").
if grep -qF 's/\r//g;if(/^_?AD_' "$CLONE_DIR/tools/functions.make" 2>/dev/null; then
    echo "patch: SKIP (already applied)  functions.make [CR-strip]"
else
    cat > /tmp/asmdefs-crlf.pl <<'PERLEOF'
my $old = "perl -ne 'if(/^_?AD_";
my $new = "perl -ne 's/\\r//g;if(/^_?AD_";
my $i = index($_, $old);
substr($_, $i, length($old), $new) if $i >= 0;
PERLEOF
    perl -0pi /tmp/asmdefs-crlf.pl "$CLONE_DIR/tools/functions.make"
    rm -f /tmp/asmdefs-crlf.pl
    if grep -qF 's/\r//g;if(/^_?AD_' "$CLONE_DIR/tools/functions.make" 2>/dev/null; then
        echo "patch: OK  functions.make [CR-strip]"
    else
        echo "patch: FAILED  functions.make [CR-strip]"
        exit 1
    fi
fi

# 16. tools/genlang — the generated lang.h must include lang_enum.h
# RELATIVELY. genlang bakes "${prefix}_enum.h" (an absolute path); on MSYS
# that prefix is "/c/..." which the native arm-none-eabi-gcc preprocessor
# cannot open, so every TU that includes lang.h fails on lang_enum.h.
if grep -qF "B310E-PORT" "$CLONE_DIR/tools/genlang" 2>/dev/null; then
    echo "patch: SKIP (already applied)  genlang [B310E-PORT]"
else
    # \$\{ escapes break MSYS sed (treats \{ as a repetition operator) - the
    # braces are literal, so leave them unescaped.
    sed -i 's|#include "${prefix}_enum.h"|#include "../lang_enum.h"   /* B310E-PORT: relative */|' "$CLONE_DIR/tools/genlang"
    if grep -qF "../lang_enum.h" "$CLONE_DIR/tools/genlang" 2>/dev/null; then
        echo "patch: OK  genlang [B310E-PORT]"
    else
        echo "patch: FAILED  genlang [B310E-PORT]"
        exit 1
    fi
fi

# 17. firmware/SOURCES — our crt0.S must be in the per-CPU crt0 list.
# Without it SC6530C falls through to the generic target/arm/crt0.S, so
# crt0.o is never built and app.lds's STARTUP(...crt0.o) fails at link.
if grep -qF "target/arm/sc6530c/crt0.S" "$CLONE_DIR/firmware/SOURCES" 2>/dev/null; then
    echo "patch: SKIP (already applied)  SOURCES [sc6530c/crt0.S]"
else
    sed -i 's|#elif CONFIG_CPU==RK27XX|#elif CONFIG_CPU==SC6530C\ntarget/arm/sc6530c/crt0.S\n#elif CONFIG_CPU==RK27XX|' "$CLONE_DIR/firmware/SOURCES"
    if grep -qF "target/arm/sc6530c/crt0.S" "$CLONE_DIR/firmware/SOURCES" 2>/dev/null; then
        echo "patch: OK  SOURCES [sc6530c/crt0.S]"
    else
        echo "patch: FAILED  SOURCES [sc6530c/crt0.S]"
        exit 1
    fi
fi

# ---- c. CONFIGURE + BUILD (out-of-tree) ----------------------------------
export PATH="$TOOLCHAIN:$HOST_CC:$PATH"
command -v arm-none-eabi-gcc >/dev/null 2>&1 || fail "arm-none-eabi-gcc not on PATH"
command -v gcc >/dev/null 2>&1 || fail "host gcc not on PATH (Rockbox host tools need it)"
# Rockbox's lang/voice pipeline builds a voicestrings.zip with `zip`. The
# MSYS2 base install lacks it (bootstrap via pacman); on Linux it is a
# distro package - fail with a clear message instead of guessing.
command -v zip >/dev/null 2>&1 || { if [ "$MSYS_LIKE" -eq 1 ]; then pacman -S --noconfirm zip; else fail "zip not found (install the distro zip package)"; fi; } || fail "zip unavailable"
mkdir -p "$BUILD_DIR" || fail "cannot mkdir $BUILD_DIR"
# Full clean every run: make.dep (generated with the CURRENT dep tooling)
# and stale objects must not leak across runs — the header-ordering fixes
# only take effect when make.dep is regenerated from scratch.
rm -rf "$BUILD_DIR"/*
echo "--- configure (out-of-tree) ---"
cd "$BUILD_DIR" || fail "cannot cd $BUILD_DIR"
../tools/configure --target=b310e --type=N --compiler-prefix=arm-none-eabi- --no-ccache < /dev/null || fail "configure"

echo "--- make -j8 (up to 3 passes) ---"
# Serial pre-pass: materialize the GENERATED headers first (lang chain,
# fonts, bitmaps, version header). A truly-fresh -j8 build otherwise races
# app .o files against them: the -MG dep scan cannot see through a not-yet-
# existing header (menu.o -> lang.h -> lang_enum.h), so make.dep has no
# edge for the second-level files and every parallel pass re-races.
make -j1 \
    "$BUILD_DIR/lang/lang_core.o" \
    "$BUILD_DIR/sysfont.h" \
    "$BUILD_DIR/rbversion.h" \
    "$BUILD_DIR/bitmaps/rockboxlogo.h" \
    "$BUILD_DIR/lang_enum.h" \
    "$BUILD_DIR/fontbundle.h" 2>&1 || true
make_ok=0
for pass in 1 2 3; do
    if make -j8; then
        make_ok=1
        break
    fi
    echo "make pass $pass failed; retrying"
done
[ "$make_ok" -eq 1 ] || fail "make"

# ---- d. COPY results ------------------------------------------------------
# The final image is rockbox.b310e (the configure "output" name — NOT
# "rockbox.bin", which collides with Rockbox's internal objcopy
# intermediate and silently yields an ELF-headed "bin").
[ -f "$BUILD_DIR/rockbox.b310e" ] || fail "rockbox.b310e not produced in $BUILD_DIR"
mkdir -p "$SDCARD_PROGS" || fail "cannot mkdir $SDCARD_PROGS"
cp -f "$BUILD_DIR/rockbox.b310e" "$SDCARD_PROGS/rockbox.bin"
# rockbox.elf stays in $BUILD_DIR (debug only) - the boot menu enumerates ALL
# progs/* files and would show a bogus ROCKBOX.ELF entry that launches an ELF.
SIZE=$(stat -c %s "$BUILD_DIR/rockbox.b310e" 2>/dev/null || wc -c < "$BUILD_DIR/rockbox.b310e")
echo "=== BUILD OK ==="
echo "rockbox.bin size: $SIZE bytes ($((SIZE/1024)) KiB)"
ls -l "$SDCARD_PROGS/rockbox.bin" 2>/dev/null

# ---- e. PACKAGE the runtime data (.rockbox/ on the card root) -------------
# Rockbox at runtime needs its data directory on the volume root:
# .rockbox/{codecs,langs,fonts,themes,wps,eqs,codepages,rocks,...} plus the
# .ignore files. `make zip` builds rockbox.zip = a .rockbox/ tree assembled by
# tools/buildzip.pl from the build + the source tree. Extract it onto the card
# root so sdcard/.rockbox/ appears next to sdcard/progs/.
# buildzip.pl prints cp errors from its wpsbuild step (icons/ dir, fonts/- glob)
# - WPS-theme packaging noise that does NOT abort the zip; the produced
# codecs/langs/fonts/themes are complete.
echo "--- packaging .rockbox runtime data ---"
make zip >/dev/null 2>&1 || fail "make zip"
ZIP="$BUILD_DIR/rockbox.zip"
[ -f "$ZIP" ] || fail "rockbox.zip not produced in $BUILD_DIR"
SDCARD_ROOT="$REPO_DIR/sdcard"
mkdir -p "$SDCARD_ROOT"
# bsdtar (the Windows System32 tar) reads zips; MSYS GNU tar and unzip are
# not reliably present in that env. On Linux GNU tar cannot read zips, so use
# Info-ZIP unzip there.
if [ "$MSYS_LIKE" -eq 1 ]; then
    (cd "$SDCARD_ROOT" && rm -rf .rockbox && /c/Windows/System32/tar.exe -xf "$ZIP" '.rockbox/' 2>/dev/null)
else
    (cd "$SDCARD_ROOT" && rm -rf .rockbox && unzip -q -o "$ZIP" '.rockbox/*' 2>/dev/null)
fi
if [ -d "$SDCARD_ROOT/.rockbox" ]; then
    RBSIZE=$(du -sh "$SDCARD_ROOT/.rockbox" 2>/dev/null | cut -f1)
    echo "staged .rockbox/ ($RBSIZE) - codecs/langs/fonts/themes/wps/eqs"
else
    echo "WARNING: .rockbox/ extraction produced no directory - Rockbox will run with builtin data only"
fi
echo "log: $LOG (previous run: $LOG.1)"
exit 0
