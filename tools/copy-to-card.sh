#!/usr/bin/env bash
# Copy the staged fpdoom card build to the SD card and hash-verify.
# Usage:  tools/copy-to-card.sh
# Requires: a mounted SD card. Auto-detected under /media/$USER or
# /run/media/$USER; otherwise set CARD=/path/to/card-root.
set -euo pipefail

repo=$(cd "$(dirname "$0")/.." && pwd)
stage="$repo/sdcard/fpbin"
user=${USER:-$(id -un 2>/dev/null || echo user)}
card=${CARD:-}

# Resolve the destination fpbin dir under a given root, or accept a root that
# already IS an fpbin dir (the Windows original targeted E:\fpbin directly).
fpbin_dir() {
    local d=$1
    if [[ -d "$d/fpbin" ]]; then
        echo "$d/fpbin"
    elif [[ -f "$d/config.json" ]]; then
        echo "$d"
    else
        echo "$d/fpbin"
    fi
}

if [[ ! -d "$stage" ]]; then
    echo "ERROR: staging not found: $stage (run 'make sdcard' first)" >&2
    exit 1
fi

if [[ -z "$card" ]]; then
    # Auto-detect: prefer a mounted volume that already has an fpbin dir, then
    # any writable mount under the standard Linux automount roots.
    for d in "/media/$user/SD_CARD" "/media/$user"/* "/run/media/$user"/*; do
        [[ -d "$d" ]] || continue
        c=$(fpbin_dir "$d")
        if [[ -d "$c" && -w "$c" ]]; then
            card="$d"
            break
        fi
    done
fi

if [[ -z "$card" ]]; then
    echo "ERROR: no SD card found. Set CARD=/path/to/card-root (the mounted card," >&2
    echo "       e.g. CARD=/media/$user/SD_CARD) or plug in the card reader." >&2
    exit 1
fi
if [[ ! -d "$card" ]]; then
    echo "ERROR: card not mounted at $card - plug in the card reader" >&2
    exit 1
fi

cardDir=$(fpbin_dir "$card")
mkdir -p "$cardDir" 2>/dev/null || {
    echo "ERROR: cannot write to $cardDir (card read-only?)" >&2
    exit 1
}

files=(infones.bin chocolate-doom.bin chocolate-heretic.bin chocolate-hexen.bin gnuboy.bin config.json)
bad=0
for f in "${files[@]}"; do
    src="$stage/$f"
    dst="$cardDir/$f"
    if [[ ! -f "$src" ]]; then
        echo "ERROR: missing in staging: $f" >&2
        bad=$((bad + 1))
        continue
    fi
    cp -f "$src" "$dst"
    h1=$(sha256sum "$src" | awk '{print $1}')
    h2=$(sha256sum "$dst" | awk '{print $1}')
    if [[ "$h1" == "$h2" ]]; then
        echo "OK   $f"
    else
        echo "DIFF $f"
        bad=$((bad + 1))
    fi
done
echo ''
if [[ $bad -eq 0 ]]; then
    echo 'ALL COPIED AND VERIFIED'
else
    echo "$bad file(s) FAILED"
fi
exit $bad
