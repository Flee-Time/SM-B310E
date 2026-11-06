#!/usr/bin/env bash
# Batch-query the imported stock firmware (tools/ghidra-import.sh) without a
# GUI or MCP server: decompile, disassemble, find literal and string users.
#
# Usage:  tools/ghidra-probe.sh <commands file> <output file>
# Commands, one per line (tools/ghidra/Probe.java has the full list):
#   lit 0x20300000 0x203fffff   functions whose literal pools hold a value
#   str sx_udc                  functions referencing matching strings
#   strexact USB                every NUL-terminated occurrence + pointers
#   dec 0x38b62                 decompile the function containing an address
#   range 0x1ef00 0x20c00       decompile every function in a range
#   dis 0x67c6c 40              disassemble
#   callers 0x20372             references to an address
#   words 0xc9f00 64            dump words
# The NOR is marked read-only for the run (nothing is saved), so the
# decompiler folds literal pools into real register addresses.
# Overrides: $GHIDRA_INSTALL_DIR, $JAVA_HOME, $PROJECT_DIR (holds B310E.gpr).
set -euo pipefail

repo=$(cd "$(dirname "$0")/.." && pwd)
[[ $# -eq 2 ]] || { echo "usage: $0 <commands file> <output file>" >&2; exit 2; }
ghidra=${GHIDRA_INSTALL_DIR:?set GHIDRA_INSTALL_DIR to the Ghidra root}
project=${PROJECT_DIR:-$repo/../B310E-ghidra}
export GHIDRA_HEADLESS_MAXMEM=${GHIDRA_HEADLESS_MAXMEM:-8G}

native() { if command -v cygpath >/dev/null 2>&1; then cygpath -w "$1"; else echo "$1"; fi; }
touch "$2"
cmds=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
out=$(cd "$(dirname "$2")" && pwd)/$(basename "$2")

if [[ -f "$ghidra/support/analyzeHeadless.bat" && "${OS:-}" == Windows_NT ]]; then
    launcher=(cmd //c "$(native "$ghidra/support/analyzeHeadless.bat")")
else
    launcher=("$ghidra/support/analyzeHeadless")
fi
"${launcher[@]}" "$(native "$project")" B310E -process -readOnly -noanalysis \
    -scriptPath "$(native "$repo/tools/ghidra")" \
    -postScript Probe.java "$(native "$cmds")" "$(native "$out")"
