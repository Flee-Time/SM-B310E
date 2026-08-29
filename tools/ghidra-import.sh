#!/usr/bin/env bash
# Ghidra headless import for the B310E stock firmware dump.
# Creates/replaces the B310E Ghidra project used by the ghidra MCP server
# (opencode.json -> pyghidra-mcp --project-path <project>/B310E.gpr).
#
# Usage:  tools/ghidra-import.sh
# Overrides: $GHIDRA_INSTALL_DIR, $JAVA_HOME, $FIRMWARE (default dump_firmware.bin),
#            $PROJECT_DIR (default $HOME/projects/B310E-ghidra)
#
# Notes:
# - SC6530C core is ARM926EJ-S (ARMv5TE) -> Ghidra language ARM:LE:32:v5t
#   (GNU name armv5tej). Handles the mixed ARM+Thumb stock code.
# - analyzeHeadless defaults to a 2G heap (MAXMEM_DEFAULT=2G); a 16 MB
#   firmware OOMs at 2G -> override via GHIDRA_HEADLESS_MAXMEM=8G.
# - -analysisTimeoutPerFile is in SECONDS (int, *1000 in HeadlessTimedTaskMonitor;
#   values > ~2.1e6 overflow to a negative delay and abort). Omitted here.
# - Kill any running pyghidra-mcp/python/JVM before re-importing, and delete
#   B310E.lock if a previous run left it behind (lock file is a marker; the real
#   lock is the OS handle held by a live process).
set -euo pipefail

repo=$(cd "$(dirname "$0")/.." && pwd)

ghidra=${GHIDRA_INSTALL_DIR:-}
if [[ -z "$ghidra" ]]; then
    # Convenience default: newest /opt/ghidra* install, else require the env var.
    if compgen -G "/opt/ghidra*" >/dev/null; then
        ghidra=$(ls -d /opt/ghidra* | sort -V | tail -n1)
    else
        echo "ERROR: GHIDRA_INSTALL_DIR is not set and no /opt/ghidra* install was found." >&2
        echo "  Set GHIDRA_INSTALL_DIR to the Ghidra root (the dir containing" >&2
        echo "  support/analyzeHeadless) and re-run." >&2
        exit 1
    fi
fi

if [[ -z "${JAVA_HOME:-}" ]]; then
    if command -v java >/dev/null 2>&1; then
        echo "JAVA_HOME not set; using java from PATH: $(command -v java)"
    else
        echo "ERROR: JAVA_HOME is not set and no java found on PATH." >&2
        echo "  Set JAVA_HOME to a JDK 21+ installation (Ghidra 12.x requires JDK 21+)." >&2
        exit 1
    fi
else
    export JAVA_HOME
fi

fw=${FIRMWARE:-"$repo/dump_firmware.bin"}
projDir=${PROJECT_DIR:-"$HOME/projects/B310E-ghidra"}
projName="B310E"
processor="ARM:LE:32:v5t"

if [[ ! -f "$ghidra/support/analyzeHeadless" ]]; then
    echo "ERROR: Ghidra not found at $ghidra (missing support/analyzeHeadless)" >&2
    exit 1
fi
if [[ ! -f "$fw" ]]; then
    echo "ERROR: Firmware not found: $fw" >&2
    exit 1
fi

export GHIDRA_HEADLESS_MAXMEM=8G

echo "Importing $fw -> $projDir/$projName.gpr (processor $processor) with 8G heap..."
set +e
"$ghidra/support/analyzeHeadless" "$projDir" "$projName" \
    -import "$fw" \
    -processor "$processor" \
    -overwrite
rc=$?
set -e
if [[ $rc -ne 0 ]]; then
    echo "ERROR: analyzeHeadless failed with exit code $rc" >&2
    exit 1
fi

echo "Import complete. Stale lock cleanup:"
rm -f "$projDir/$projName.lock" "$projDir/$projName.lock~"
echo "Done. Restart opencode for the ghidra MCP server to pick up the project."
