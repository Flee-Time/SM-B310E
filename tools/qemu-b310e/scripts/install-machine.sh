#!/usr/bin/env bash
# install-machine.sh - install the B310E machine sources into a QEMU source
# tree and wire the meson/Kconfig build entries. Idempotent. Bash port of
# install-machine.ps1.
#
# Copies machine/hw/arm/*.c   -> <qemu-src>/hw/arm/
#          machine/hw/misc/*.c -> <qemu-src>/hw/misc/
# and appends marker-guarded wiring blocks (# B310E-MACHINE-BEGIN/END) to
# hw/arm/meson.build, hw/arm/Kconfig, hw/misc/meson.build, hw/misc/Kconfig.
# The wiring is GENERATED from the .c files actually present in machine/, so
# Wave-3/4 todos just drop files in and re-run this script.
#
#   --uninstall  reverse a previous install (strip markers + delete copies)
#   --whatif     print what would change without touching anything
#
# Text edits are LF-preserving and BOM-free; a tree with UNBALANCED markers
# is refused (exit 1) rather than repaired, even under --whatif.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
MachineDir=""
QemuSrc=""
Uninstall=0
DRY=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    -MachineDir|--machine-dir) MachineDir="$2"; shift 2 ;;
    -QemuSrc|--qemu-src)       QemuSrc="$2";   shift 2 ;;
    -Uninstall|--uninstall)    Uninstall=1;    shift 1 ;;
    -WhatIf|--whatif)          DRY=1;          shift 1 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done

if [[ -z "$MachineDir" ]]; then MachineDir="$SCRIPT_DIR/../machine"; fi
if [[ -z "$QemuSrc" ]]; then QemuSrc="$HOME/qemu-b310e/qemu-src"; fi

# ---- path helpers -----------------------------------------------------------
# Lexically normalize to an absolute path (does not require existence).
abs_path() {
  local p="$1" out="" seg
  case "$p" in
    /*) : ;;
    *)  p="$(pwd)/$p" ;;
  esac
  IFS='/' read -r -a segs <<< "$p"
  for seg in "${segs[@]}"; do
    if [[ -z "$seg" || "$seg" == "." ]]; then
      continue
    elif [[ "$seg" == ".." ]]; then
      out="${out%/*}"
    else
      out="$out/$seg"
    fi
  done
  [[ -n "$out" ]] || out="/"
  echo "$out"
}

MarkerBegin='# B310E-MACHINE-BEGIN'
MarkerEnd='# B310E-MACHINE-END'

# ConvertTo-SymbolName: sc6530_aux.c -> SC6530_AUX ; b310e.c -> B310E
sym_of() {
  basename "$1" .c | tr '[:lower:]' '[:upper:]'
}

# Generate the wiring block texts from the .c file lists.
make_meson_arm() {
  local block="" f
  for f in "$@"; do
    block+="arm_common_ss.add(when: 'CONFIG_$(sym_of "$f")', if_true: files('$(basename "$f")'))"$'\n'
  done
  echo "${block%$'\n'}"
}
make_kconfig_arm() {
  local block="" first=1 f
  for f in "$@"; do
    if [[ $first -eq 1 ]]; then first=0; else block+=$'\n\n'; fi
    block+="config $(sym_of "$f")"$'\n'"    bool"$'\n'"    default y"$'\n'"    depends on TCG && ARM"
  done
  echo "$block"
}
make_meson_misc() {
  local block="" f
  for f in "$@"; do
    block+="system_ss.add(when: 'CONFIG_$(sym_of "$f")', if_true: files('$(basename "$f")'))"$'\n'
  done
  echo "${block%$'\n'}"
}
make_kconfig_misc() {
  local block="" first=1 f
  for f in "$@"; do
    if [[ $first -eq 1 ]]; then first=0; else block+=$'\n\n'; fi
    block+="config $(sym_of "$f")"$'\n'"    bool"$'\n'"    default y"$'\n'"    depends on ARM"
  done
  echo "$block"
}

# Replace (or append) the marker-guarded block in a text file. $block=''
# strips the block. Prints "wired" / "stripped" / "up to date (no change)".
update_marker_block() {
  local path="$1" block="$2" action="wired" tmp
  if [[ -z "$block" ]]; then action="stripped"; fi
  tmp="$(mktemp)"
  if ! B310E_BLOCK="$block" perl -0 -e '
    my $f = <>;
    my $begins = () = $f =~ /^# B310E-MACHINE-BEGIN\r?$/mg;
    my $ends   = () = $f =~ /^# B310E-MACHINE-END\r?$/mg;
    if ($begins != $ends) {
      die "Marker state corrupt in $ARGV (BEGIN=$begins END=$ends) - refusing to modify\n";
    }
    # strip ALL existing blocks (the ps1 [regex]::Replace semantics)
    $f =~ s/^# B310E-MACHINE-BEGIN\r?\n.*?^# B310E-MACHINE-END(?:\r?\n|\z)//msg;
    my $b = $ENV{"B310E_BLOCK"};
    if ($b ne "") {
      $f .= "\n" unless $f eq "" || substr($f, -1, 1) eq "\n";
      $f .= "# B310E-MACHINE-BEGIN\n$b\n# B310E-MACHINE-END\n";
    }
    print $f;
  ' "$path" > "$tmp"; then
    rm -f "$tmp"
    echo "    $(basename "$path"): REFUSED (marker state corrupt)" >&2
    exit 1
  fi
  if cmp -s "$path" "$tmp"; then
    rm -f "$tmp"
    echo "    $(basename "$path"): up to date (no change)"
    return 0
  fi
  if [[ "$DRY" -eq 1 ]]; then
    rm -f "$tmp"
    echo "    $(basename "$path"): would $action B310E wiring block"
    return 0
  fi
  mv "$tmp" "$path"
  echo "    $(basename "$path"): $action"
}

# ---- resolve + validate paths -------------------------------------------------
MachineDir="$(abs_path "$MachineDir")"
QemuSrc="$(abs_path "$QemuSrc")"

if [[ ! -d "$QemuSrc" ]]; then
  echo "QEMU source tree not found: $QemuSrc" >&2
  exit 1
fi
for rel in hw/arm/meson.build hw/arm/Kconfig hw/misc/meson.build hw/misc/Kconfig; do
  if [[ ! -f "$QemuSrc/$rel" ]]; then
    echo "Not a QEMU source tree (missing $rel) in: $QemuSrc" >&2
    exit 1
  fi
done

ArmDir="$MachineDir/hw/arm"
MiscDir="$MachineDir/hw/misc"
ArmTrace="$ArmDir/trace-events"
MiscTrace="$MiscDir/trace-events"

arm_files=()
misc_files=()
if [[ -d "$ArmDir" ]]; then
  shopt -s nullglob
  for f in "$ArmDir"/*.c; do arm_files+=("$f"); done
fi
if [[ -d "$MiscDir" ]]; then
  shopt -s nullglob
  for f in "$MiscDir"/*.c; do misc_files+=("$f"); done
fi
if [[ ${#arm_files[@]} -gt 0 ]]; then
  mapfile -t arm_files < <(printf '%s\n' "${arm_files[@]}" | sort)
fi
if [[ ${#misc_files[@]} -gt 0 ]]; then
  mapfile -t misc_files < <(printf '%s\n' "${misc_files[@]}" | sort)
fi

arm_meson_block="$(make_meson_arm "${arm_files[@]}")"
arm_kconfig_block="$(make_kconfig_arm "${arm_files[@]}")"
misc_meson_block="$(make_meson_misc "${misc_files[@]}")"
misc_kconfig_block="$(make_kconfig_misc "${misc_files[@]}")"

echo "B310E machine installer"
echo "  MachineDir: $MachineDir  (arm: ${#arm_files[@]} file(s), misc: ${#misc_files[@]} file(s))"
echo "  QemuSrc:    $QemuSrc"

if [[ "$Uninstall" -eq 0 ]]; then
  if [[ ${#arm_files[@]} -eq 0 && ${#misc_files[@]} -eq 0 &&
        ! -f "$ArmTrace" && ! -f "$MiscTrace" ]]; then
    echo "  Machine dir is empty - nothing to copy or wire (no-op, exit 0)."
  fi

  # 1) copy the .c sources
  for f in "${arm_files[@]}"; do
    if [[ "$DRY" -eq 1 ]]; then
      echo "  would copy hw/arm/$(basename "$f")"
    else
      cp -f "$f" "$QemuSrc/hw/arm/$(basename "$f")"
      echo "    copied hw/arm/$(basename "$f")"
    fi
  done
  for f in "${misc_files[@]}"; do
    if [[ "$DRY" -eq 1 ]]; then
      echo "  would copy hw/misc/$(basename "$f")"
    else
      cp -f "$f" "$QemuSrc/hw/misc/$(basename "$f")"
      echo "    copied hw/misc/$(basename "$f")"
    fi
  done

  # 2) wire meson/Kconfig (marker-guarded, generated from the file list)
  if [[ -n "$arm_meson_block" ]]; then
    update_marker_block "$QemuSrc/hw/arm/meson.build" "$arm_meson_block"
  else
    echo "    meson.build: no sources - wiring skipped"
  fi
  if [[ -n "$arm_kconfig_block" ]]; then
    update_marker_block "$QemuSrc/hw/arm/Kconfig" "$arm_kconfig_block"
  else
    echo "    Kconfig: no sources - wiring skipped"
  fi
  if [[ -n "$misc_meson_block" ]]; then
    update_marker_block "$QemuSrc/hw/misc/meson.build" "$misc_meson_block"
  else
    echo "    meson.build: no sources - wiring skipped"
  fi
  if [[ -n "$misc_kconfig_block" ]]; then
    update_marker_block "$QemuSrc/hw/misc/Kconfig" "$misc_kconfig_block"
  else
    echo "    Kconfig: no sources - wiring skipped"
  fi

  # 3) optional trace-events files (Wave-3 prep; appended marker-guarded)
  apply_trace() {
    local src="$1" dst="$2" block
    block="$(perl -0 -e 'my $t = <>; $t =~ s/\r?\n+\z//; $t =~ s/\r+\z//; print $t' "$src")"
    update_marker_block "$dst" "$block"
  }
  if [[ -f "$ArmTrace" ]]; then
    apply_trace "$ArmTrace" "$QemuSrc/hw/arm/trace-events"
  fi
  if [[ -f "$MiscTrace" ]]; then
    apply_trace "$MiscTrace" "$QemuSrc/hw/misc/trace-events"
  fi
else
  # --uninstall: strip wiring + trace blocks, remove the copied files
  echo "  Uninstall mode"
  for rel in hw/arm/meson.build hw/arm/Kconfig hw/misc/meson.build hw/misc/Kconfig; do
    update_marker_block "$QemuSrc/$rel" ""
  done
  if [[ -f "$ArmTrace" || -f "$QemuSrc/hw/arm/trace-events" ]]; then
    update_marker_block "$QemuSrc/hw/arm/trace-events" ""
  fi
  if [[ -f "$MiscTrace" || -f "$QemuSrc/hw/misc/trace-events" ]]; then
    update_marker_block "$QemuSrc/hw/misc/trace-events" ""
  fi
  for f in "${arm_files[@]}" "${misc_files[@]}"; do
    dst=""
    if [[ "$(dirname "$f")" == "$ArmDir" ]]; then
      dst="$QemuSrc/hw/arm/$(basename "$f")"
    else
      dst="$QemuSrc/hw/misc/$(basename "$f")"
    fi
    if [[ -f "$dst" ]]; then
      if [[ "$DRY" -eq 1 ]]; then
        echo "  would remove $(basename "$dst")"
      else
        rm -f "$dst"
        echo "    removed $(basename "$dst")"
      fi
    fi
  done
fi

# ---- summary: what changed in the qemu tree -----------------------------------
if [[ -d "$QemuSrc/.git" ]]; then
  echo ""
  echo "  --- qemu-src git status (our additions only) ---"
  (cd "$QemuSrc" && git status --short) || true
  echo "  --- end git status ---"
fi

echo "Done."
exit 0
