#!/usr/bin/env bash
set -euo pipefail
script="${BASH_SOURCE[0]}"
[[ "$script" == */* ]] || script="./$script"
root="$(cd -- "${script%/*}" && pwd)"
exec "${B310E_PYTHON:-python3}" "$root/scripts/build.py" "$@"
