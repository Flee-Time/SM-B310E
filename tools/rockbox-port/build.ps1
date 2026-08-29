# B310E-OS Rockbox port - tools/rockbox-port/build.ps1 (T2.2)
# Wrapper that runs build.sh under MSYS2 bash (configure is POSIX sh).
# Usage: powershell -ExecutionPolicy Bypass -File tools\rockbox-port\build.ps1
# NOTE: ASCII-only. PowerShell 5.1 reads a BOM-less UTF-8 file as ANSI, and
# a UTF-8 em-dash (0xE2 0x80 0x94) decodes as cp1252 '"' - breaking parsing.

$ErrorActionPreference = "Stop"

$bash = "C:\msys64\usr\bin\bash.exe"
$portDir = Join-Path $PSScriptRoot "."

if (-not (Test-Path $bash)) {
    Write-Error "MSYS2 bash not found at $bash - build.sh needs POSIX sh (configure is not cmd-compatible)."
    exit 1
}

# Space-free junction to the arm toolchain (its real path has spaces AND
# parens, which break configure's arch detection and make's CC line).
$toolchain = "C:\arm-gcc"
$toolchainReal = "C:\Program Files (x86)\Arm GNU Toolchain arm-none-eabi\14.2 rel1"
if (-not $env:B310E_TOOLCHAIN -and -not (Test-Path "$toolchain\bin\arm-none-eabi-gcc.exe")) {
    if (Test-Path "$toolchainReal\bin\arm-none-eabi-gcc.exe") {
        New-Item -ItemType Junction -Path $toolchain -Target $toolchainReal | Out-Null
        Write-Host "created junction: $toolchain -> $toolchainReal"
    } else {
        Write-Error "toolchain not found at $toolchainReal"
        exit 1
    }
}

$shPath = (Join-Path $portDir "build.sh").Replace("\", "/")
# C:\... -> /c/... (MSYS path)
$msysPath = "/" + $shPath.Substring(0, 1).ToLower() + $shPath.Substring(2)

Write-Host "== B310E Rockbox build via MSYS2 bash =="
Write-Host "  build.sh: $shPath"

& $bash -lc "$msysPath"
exit $LASTEXITCODE
