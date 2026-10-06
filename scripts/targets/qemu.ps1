<#
.SYNOPSIS
Build the pinned B310E QEMU headless on Windows, without elevation.
.DESCRIPTION
Uses an existing MSYS2 MINGW64 installation. Creates a QEMU checkout if
missing, verifies the pinned commit, installs the machine, and builds ARM.
The optional QEMU install bundle is skipped if Windows denies symlinks.
The firmware and proprietary reference source are not build dependencies.
#>
[CmdletBinding()]
param(
    [string]$QemuSrc = '',
    [string]$Msys64 = 'C:\msys64',
    [ValidateRange(1,128)][int]$Jobs = 8
)
$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if (-not $QemuSrc) {
    $QemuSrc = Join-Path $repoRoot 'build\qemu'
}
$QemuSrc = [IO.Path]::GetFullPath($QemuSrc)
$bash = Join-Path $Msys64 'usr\bin\bash.exe'
$python = Join-Path $Msys64 'mingw64\bin\python.exe'
if (-not (Test-Path -LiteralPath $bash) -or -not (Test-Path -LiteralPath $python)) {
    throw 'MSYS2 bash and MINGW64 Python are required; see docs/emulator-audio.md.'
}
if (-not (Test-Path -LiteralPath $QemuSrc)) {
    New-Item -ItemType Directory -Path (Split-Path $QemuSrc -Parent) -Force | Out-Null
    & git clone --depth 1 --branch v11.1.0 https://gitlab.com/qemu-project/qemu $QemuSrc
    if ($LASTEXITCODE -ne 0) { throw 'QEMU clone failed.' }
}
$pin = & git -c "safe.directory=$QemuSrc" -C $QemuSrc rev-parse HEAD
if ($LASTEXITCODE -ne 0 -or $pin -ne '84f07211cc5b4fc6a371559bf8a5de4fb068e648') {
    throw 'QEMU must be at the pinned v11.1.0 commit. No files were reset.'
}
& (Join-Path $repoRoot 'emulator\qemu\scripts\install-machine.ps1') -QemuSrc $QemuSrc
& $python (Join-Path $repoRoot 'emulator\qemu\scripts\prepare-headless-build.py') $QemuSrc
if ($LASTEXITCODE -ne 0) { throw 'Preparing the QEMU build failed.' }
$savedMsystem = $env:MSYSTEM
$savedPath = $env:PATH
try {
    $env:MSYSTEM = 'MINGW64'
    $env:PATH = (Join-Path $Msys64 'mingw64\bin') + ';' + $savedPath
    $compileScript = (Join-Path $repoRoot 'scripts/targets/qemu-compile.sh').Replace('\', '/')
    & $bash $compileScript $QemuSrc $Jobs
    if ($LASTEXITCODE -ne 0) { throw "Build failed; logs are in $QemuSrc." }
    Write-Host "Ready: $QemuSrc\build\qemu-system-arm.exe"
} finally {
    $env:MSYSTEM = $savedMsystem
    $env:PATH = $savedPath
}
