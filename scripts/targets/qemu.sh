#!/usr/bin/env bash
# Pinned Linux QEMU build. Dependencies are installed by the caller.
set -euo pipefail
Repo="$(cd "$(dirname "$0")/../.." && pwd)"
Source="$1"
Jobs="$2"
Pin=84f07211cc5b4fc6a371559bf8a5de4fb068e648
if [ ! -d "$Source/.git" ]; then
    mkdir -p "$(dirname "$Source")"
    git clone --depth 1 --branch v11.1.0 https://gitlab.com/qemu-project/qemu "$Source"
fi
[ "$(git -c safe.directory="$Source" -C "$Source" rev-parse HEAD)" = "$Pin" ] || {
    echo "QEMU source must be at $Pin; existing files were not reset" >&2; exit 1;
}
bash "$Repo/emulator/qemu/scripts/install-machine.sh" --qemu-src "$Source"
bash "$Repo/scripts/targets/qemu-compile.sh" "$Source" "$Jobs"
