# Ghidra headless import for the B310E stock firmware dump.
# Creates/replaces the B310E Ghidra project used by the ghidra MCP server
# (opencode.json -> pyghidra-mcp --project-path <project>\B310E.gpr).
#
# Usage:  powershell -ExecutionPolicy Bypass -File tools\ghidra-import.ps1
# Overrides: $env:GHIDRA_INSTALL_DIR, $env:FIRMWARE (default dump_firmware.bin),
#            $env:PROJECT_DIR (default <user profile>\projects\B310E-ghidra)
#
# Notes:
# - SC6530C core is ARM926EJ-S (ARMv5TE) -> Ghidra language ARM:LE:32:v5t
#   (GNU name armv5tej). Handles the mixed ARM+Thumb stock code.
# - analyzeHeadless.bat defaults to a 2G heap (MAXMEM_DEFAULT=2G); a 16 MB
#   firmware OOMs at 2G -> override via GHIDRA_HEADLESS_MAXMEM=8G.
# - -analysisTimeoutPerFile is in SECONDS (int, *1000 in HeadlessTimedTaskMonitor;
#   values > ~2.1e6 overflow to a negative delay and abort). Omitted here.
# - Kill any running pyghidra-mcp/python/JVM before re-importing, and delete
#   B310E.lock if a previous run left it behind (lock file is a marker; the real
#   lock is the OS handle held by a live process).

$ErrorActionPreference = "Stop"

$ghidra  = if ($env:GHIDRA_INSTALL_DIR) { $env:GHIDRA_INSTALL_DIR } else { "$env:LOCALAPPDATA\Programs\ghidra_12.1.3\ghidra_12.1.3_PUBLIC" }
$java    = if ($env:JAVA_HOME) { $env:JAVA_HOME } else { "C:\Program Files\Eclipse Adoptium\jdk-25.0.4.7-hotspot" }
$fw      = if ($env:FIRMWARE) { $env:FIRMWARE } else { Join-Path $PSScriptRoot "..\dump_firmware.bin" }
$projDir = if ($env:PROJECT_DIR) { $env:PROJECT_DIR } else { Join-Path $env:USERPROFILE "projects\B310E-ghidra" }
$projName = "B310E"
$processor = "ARM:LE:32:v5t"

if (-not (Test-Path "$ghidra\support\analyzeHeadless.bat")) { throw "Ghidra not found at $ghidra" }
if (-not (Test-Path $fw)) { throw "Firmware not found: $fw" }

$env:JAVA_HOME = $java
$env:GHIDRA_HEADLESS_MAXMEM = "8G"

Write-Host "Importing $fw -> $projDir\$projName.gpr (processor $processor) with 8G heap..."
& "$ghidra\support\analyzeHeadless.bat" $projDir $projName `
    -import $fw `
    -processor $processor `
    -overwrite
if ($LASTEXITCODE -ne 0) { throw "analyzeHeadless failed with exit code $LASTEXITCODE" }

Write-Host "Import complete. Stale lock cleanup:"
Remove-Item -Force "$projDir\$projName.lock", "$projDir\$projName.lock~" -ErrorAction SilentlyContinue
Write-Host "Done. Restart opencode for the ghidra MCP server to pick up the project."
