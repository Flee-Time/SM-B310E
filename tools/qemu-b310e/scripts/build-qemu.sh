#!/usr/bin/env bash
# build-qemu.sh - full QEMU build recipe for the B310E machine: clone-or-pin
# v11.1.0 -> install-machine.sh -> configure -> make -> sanity. Idempotent.
# Bash port of build-qemu.ps1 (Linux edition).
#
# Steps:
#   1. CLONE-OR-PIN  - if <QemuSrc>/.git is missing, shallow-clone
#      https://gitlab.com/qemu-project/qemu at tag v11.1.0 into it; if the
#      tree already exists, verify `git describe --tags` pins v11.1.0*
#      (our installed wiring makes it v11.1.0-dirty - tolerated). A tree at
#      a DIFFERENT tag aborts with instructions instead of clobbering.
#   2. INSTALL        - runs scripts/install-machine.sh (no-op until
#      Wave 3 drops sources into tools/qemu-b310e/machine/).
#   3. CONFIGURE      - ./configure --target-list=<arm-softmmu> --enable-png --enable-gtk
#      --disable-werror. Skipped when build/config-host.mak exists and the
#      args fingerprint matches, so re-runs do not reconfigure.
#   4. BUILD          - make -j<N> (bounded by --build-timeout; kill + log
#      tail on timeout).
#   5. SANITY         - build/qemu-system-arm(.exe) exists, prints the
#      "QEMU emulator version" banner, and CONFIG_PNG=y is confirmed.
#
# NOTE: elevation is NOT needed on Linux (the Windows ps1's
# SeCreateSymbolicLinkPrivilege/MSYS2 handling is Windows-only and skipped).
# The build needs meson, ninja, gcc, make and libpng-dev installed; the
# script checks for them with command -v and fails with clear messages.
#
# Logs land next to the tree: <parent-of-qemu-src>/{clone,configure,build,sanity}.log
#
# Flags: --skip-clone --skip-install --skip-configure --skip-build --skip-sanity
#        --jobs N --build-timeout N --qemu-src DIR --tag v11.1.0
#        --repo-url URL --target-list LIST --whatif
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
QemuSrc=""
RepoUrl="https://gitlab.com/qemu-project/qemu"
Tag="v11.1.0"
TargetList="arm-softmmu"
Jobs=0
BuildTimeoutSec=1800
SkipClone=0
SkipInstall=0
SkipConfigure=0
SkipBuild=0
SkipSanity=0
DRY=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --skip-clone)     SkipClone=1;       shift 1 ;;
    --skip-install)   SkipInstall=1;     shift 1 ;;
    --skip-configure) SkipConfigure=1;   shift 1 ;;
    --skip-build)     SkipBuild=1;       shift 1 ;;
    --skip-sanity)    SkipSanity=1;      shift 1 ;;
    --jobs)           Jobs="$2";         shift 2 ;;
    --build-timeout)  BuildTimeoutSec="$2"; shift 2 ;;
    --qemu-src)       QemuSrc="$2";      shift 2 ;;
    --tag)            Tag="$2";          shift 2 ;;
    --repo-url)       RepoUrl="$2";      shift 2 ;;
    --target-list)    TargetList="$2";   shift 2 ;;
    -WhatIf|--whatif) DRY=1;             shift 1 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done

if [[ -z "$QemuSrc" ]]; then QemuSrc="$HOME/qemu-b310e/qemu-src"; fi
if [[ "$Jobs" -lt 1 ]]; then
  Jobs="$(nproc 2>/dev/null || echo 8)"
fi

LocalDir="$(dirname "$QemuSrc")"
QemuSrc="$(cd "$LocalDir" 2>/dev/null && pwd)/$(basename "$QemuSrc")"

# ---- prereq check: git is needed for clone-or-pin and git status ---------------
command -v git >/dev/null 2>&1 || { echo "MISSING TOOL: git - install it (Linux build deps: git, python3, meson, ninja, gcc, make, libpng-dev)" >&2; exit 1; }

echo "B310E QEMU build"
echo "  QemuSrc: $QemuSrc (tag $Tag)"
echo "  Jobs:    $Jobs"
echo "  Note:    Linux build - elevation not needed (Windows-only in the ps1)."

# ---------------------------------------------------------------------------
# helper: run a command with a bounded wait; kill + fail on timeout; fail on
# nonzero exit, printing the log tail.
# ---------------------------------------------------------------------------
run_bounded() {
  local timeout_sec="$1" what="$2" log="$3"
  shift 3
  if [[ "$DRY" -eq 1 ]]; then
    echo "  [dry run] (${what})"
    return 0
  fi
  echo "==> $what  (bounded: ${timeout_sec}s)"
  : > "$log"
  if command -v timeout >/dev/null 2>&1; then
    local rc=0
    timeout "$timeout_sec" "$@" >> "$log" 2>&1 || rc=$?
    if [[ "$rc" -ne 0 ]]; then
      if [[ "$rc" -eq 124 ]]; then
        echo "TIMEOUT after ${timeout_sec}s: $what" >&2
      else
        echo "FAILED (exit $rc): $what" >&2
      fi
      echo "--- log tail ---" >&2
      tail -n 30 "$log" >&2 || true
      return 1
    fi
  else
    # background + kill pattern (no coreutils `timeout`)
    "$@" >> "$log" 2>&1 &
    local pid=$!
    local secs=0
    while kill -0 "$pid" 2>/dev/null && [[ "$secs" -lt "$timeout_sec" ]]; do
      sleep 1
      secs=$((secs + 1))
    done
    if kill -0 "$pid" 2>/dev/null; then
      kill "$pid" 2>/dev/null || true
      wait "$pid" 2>/dev/null || true
      echo "TIMEOUT after ${timeout_sec}s: $what" >&2
      echo "--- log tail ---" >&2
      tail -n 30 "$log" >&2 || true
      return 1
    fi
    if ! wait "$pid"; then
      echo "FAILED: $what" >&2
      echo "--- log tail ---" >&2
      tail -n 30 "$log" >&2 || true
      return 1
    fi
  fi
  echo "    OK ($what)"
  return 0
}

# ---------------------------------------------------------------------------
# 1) clone-or-pin
# ---------------------------------------------------------------------------
if [[ ! -d "$QemuSrc/.git" ]]; then
  if [[ "$SkipClone" -eq 1 ]]; then
    echo "QemuSrc missing at $QemuSrc and --skip-clone given" >&2
    exit 1
  fi
  if [[ -e "$QemuSrc" ]]; then
    echo "qemu-src exists at $QemuSrc but has no .git - remove the dir or point --qemu-src elsewhere" >&2
    exit 1
  fi
  cloneLog="$LocalDir/clone.log"
  echo "==> git clone (tag $Tag)  (bounded: ${BuildTimeoutSec}s)"
  if [[ "$DRY" -eq 1 ]]; then
    echo "  [dry run] would clone $RepoUrl @ $Tag -> $QemuSrc"
  else
    mkdir -p "$LocalDir"
    if ! GIT_TERMINAL_PROMPT=0 run_bounded "$BuildTimeoutSec" "git clone (tag $Tag)" "$cloneLog" git clone --branch "$Tag" --depth 1 "$RepoUrl" "$QemuSrc"; then
      echo "clone failed - see $cloneLog (tail above)" >&2
      exit 1
    fi
  fi
else
  echo "  qemu-src present - verifying pin"
  desc="$(git -C "$QemuSrc" describe --tags 2>/dev/null || true)"
  desc="${desc%$'\r'}"
  case "$desc" in
    "$Tag"*)
      echo "  pin OK: $desc"
      ;;
    *)
      echo "qemu-src is pinned to '$desc', expected '$Tag*'. Refusing to auto-reset (the tree may hold our installed wiring). Fix manually: git -C $QemuSrc checkout $Tag, or remove the dir and re-run." >&2
      exit 1
      ;;
  esac
fi

# ---------------------------------------------------------------------------
# 2) install our machine sources + wiring
# ---------------------------------------------------------------------------
if [[ "$SkipInstall" -eq 0 ]]; then
  echo "==> install-machine.sh"
  install_args=("--qemu-src" "$QemuSrc")
  if [[ "$DRY" -eq 1 ]]; then install_args+=(--whatif); fi
  if [[ "$DRY" -eq 1 ]]; then
    echo "  [dry run] would run: ${SCRIPT_DIR}/install-machine.sh ${install_args[*]}"
  else
    if ! "${SCRIPT_DIR}/install-machine.sh" "${install_args[@]}"; then
      echo "install-machine.sh failed (exit $?)" >&2
      exit 1
    fi
  fi
fi

# ---------------------------------------------------------------------------
# 3) configure (fingerprint-gated: re-runs do not reconfigure)
# ---------------------------------------------------------------------------
BuildDir="$QemuSrc/build"
FingerprintFile="$BuildDir/.b310e-configure-fingerprint"
Fingerprint="target-list=$TargetList;png=1;gtk=1;werror=0"

if [[ "$SkipConfigure" -eq 0 ]]; then

  # Auto-provision GTK3
  if ! dpkg -l | grep -q libgtk-3-dev 2>/dev/null; then
    if command -v apt-get >/dev/null 2>&1; then
      echo "  Installing libgtk-3-dev..."
      sudo apt-get update && sudo apt-get install -y libgtk-3-dev || echo "  warning: failed to install libgtk-3-dev"
    fi
  fi
  # Linux build-tool prereq check (meson/ninja/gcc/make + libpng via pkg-config)
  echo "  Linux build deps: meson, ninja, gcc, make, libpng-dev (checking...)"
  for t in python3 meson ninja gcc make; do
    command -v "$t" >/dev/null 2>&1 || { echo "MISSING TOOL: $t - install it (Linux build deps: git, python3, meson, ninja, gcc, make, libpng-dev)" >&2; exit 1; }
  done
  if command -v pkg-config >/dev/null 2>&1; then
    pkg-config --exists libpng || { echo "MISSING: libpng dev package (pkg-config --exists libpng failed) - PNG is required for screendump" >&2; exit 1; }
  else
    echo "  note: pkg-config not found - cannot verify libpng; CONFIG_PNG sanity may fail later"
  fi

  need_configure=0
  if [[ ! -f "$BuildDir/config-host.mak" ]]; then
    need_configure=1
  elif [[ -f "$FingerprintFile" ]]; then
    stored="$(cat "$FingerprintFile" 2>/dev/null || true)"
    stored="${stored%$'\r'}"
    if [[ "$stored" != "$Fingerprint" ]]; then need_configure=1; fi
  else
    need_configure=1   # foreign build dir: args unknown, reconfigure
  fi

  if [[ "$need_configure" -eq 1 ]]; then
    cfgLog="$LocalDir/configure.log"
    echo "==> configure --target-list=$TargetList --enable-png --enable-gtk --disable-werror  (bounded: ${BuildTimeoutSec}s)"
    if [[ "$DRY" -eq 1 ]]; then
      echo "  [dry run] would run: cd $QemuSrc && ./configure --target-list=$TargetList --enable-png --enable-gtk --disable-werror"
    else
      if run_bounded "$BuildTimeoutSec" "configure" "$cfgLog" bash -c 'cd "$1" && shift && exec ./configure "$@"' _ "$QemuSrc" --target-list="$TargetList" --enable-png --enable-gtk --disable-werror; then
        printf '%s' "$Fingerprint" > "$FingerprintFile"
        echo "  wrote configure fingerprint: $FingerprintFile"
      else
        echo "configure failed - see $cfgLog (tail above)" >&2
        exit 1
      fi
    fi
  else
    echo "  configure up to date (fingerprint matches) - skipping"
  fi
fi

# ---------------------------------------------------------------------------
# 4) build
# ---------------------------------------------------------------------------
if [[ "$SkipBuild" -eq 0 ]]; then
  buildLog="$LocalDir/build.log"
  echo "==> make -j$Jobs  (bounded: ${BuildTimeoutSec}s)"
  if [[ "$DRY" -eq 1 ]]; then
    echo "  [dry run] would run: cd $QemuSrc && make -j$Jobs"
  else
    if ! run_bounded "$BuildTimeoutSec" "make" "$buildLog" bash -c 'cd "$1" && shift && exec make -j"$1"' _ "$QemuSrc" "$Jobs"; then
      echo "build failed - see $buildLog (tail above)" >&2
      exit 1
    fi
  fi
fi

# ---------------------------------------------------------------------------
# 5) sanity
# ---------------------------------------------------------------------------
if [[ "$SkipSanity" -eq 0 ]]; then
  echo "==> sanity checks"
  exe=""
  if [[ -f "$BuildDir/qemu-system-arm" ]]; then exe="$BuildDir/qemu-system-arm"; fi
  if [[ -f "$BuildDir/qemu-system-arm.exe" ]]; then exe="$BuildDir/qemu-system-arm.exe"; fi
  if [[ "$DRY" -eq 1 ]]; then
    echo "  [dry run] would check $BuildDir/qemu-system-arm + --version banner + CONFIG_PNG=y"
  elif [[ -z "$exe" ]]; then
    echo "SANITY FAILED: $BuildDir/qemu-system-arm missing (build incomplete or --skip-build?)" >&2
    exit 1
  else
    sanityLog="$LocalDir/sanity.log"
    ver="$("$exe" --version 2>&1 || true)"
    case "$ver" in
      *"QEMU emulator version"*) : ;;
      *)
        echo "SANITY FAILED: unexpected --version output:" >&2
        printf '%s\n' "$ver" >&2
        exit 1
        ;;
    esac
    pngLine="$(grep -E '^CONFIG_PNG=' "$BuildDir/config-host.mak" 2>/dev/null || true)"
    if [[ -n "$pngLine" && "${pngLine#*=}" != "y" ]]; then
      echo "SANITY FAILED: CONFIG_PNG not enabled (screendump writes PPM): $pngLine" >&2
      exit 1
    fi
    {
      echo ""
      echo "  SANITY OK:"
      printf '%s\n' "$ver"
      if [[ -n "$pngLine" ]]; then echo "  $pngLine"; fi
      head="$(git -C "$QemuSrc" rev-parse --short HEAD 2>/dev/null || true)"
      if [[ -n "$head" ]]; then echo "  qemu-src HEAD: $head"; fi
    } | tee "$sanityLog"
  fi
fi

echo ""
echo "Done. Usage doc: docs/b310e-qemu.md (Wave 5/7)."
exit 0
