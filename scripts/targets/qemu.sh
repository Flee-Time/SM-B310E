#!/usr/bin/env bash
# Pinned Linux QEMU build. Dependencies are installed by the caller.
set -euo pipefail
Repo="$(cd "$(dirname "$0")/../.." && pwd)"
Source="$1"
Jobs="$2"
Mode="${3:-desktop}"
"${B310E_PYTHON:-python3}" "$Repo/emulator/qemu/scripts/prepare-build.py" "$Source"
bash "$Repo/emulator/qemu/scripts/install-machine.sh" --qemu-src "$Source"
bash "$Repo/scripts/targets/qemu-compile.sh" "$Source" "$Jobs" "$Mode"
