<#
.SYNOPSIS
Build the pinned B310E QEMU desktop on Windows, without elevation.
.DESCRIPTION
Uses an existing MSYS2 MINGW64 installation. Creates a QEMU checkout if
missing, verifies the pinned commit, installs the machine, and builds ARM.
GTK, SDL, OpenGL and native audio are included by default.
The firmware and proprietary reference source are not build dependencies.
#>
[CmdletBinding()]
param(
    [string]$QemuSrc = '',
    [string]$Msys64 = '',
    [ValidateRange(1,128)][int]$Jobs = 8,
    [ValidateSet('desktop','headless')][string]$DisplayMode = 'desktop'
)
$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if (-not $QemuSrc) {
    $QemuSrc = Join-Path $repoRoot 'build\qemu'
}
$QemuSrc = [IO.Path]::GetFullPath($QemuSrc)
if (-not $Msys64) {
    if ($env:B310E_MSYS) { $Msys64 = $env:B310E_MSYS }
    elseif (Test-Path -LiteralPath 'D:\Toolchains\msys64\usr\bin\bash.exe') { $Msys64 = 'D:\Toolchains\msys64' }
    else { $Msys64 = 'C:\msys64' }
}
$bash = Join-Path $Msys64 'usr\bin\bash.exe'
$python = Join-Path $Msys64 'mingw64\bin\python.exe'
if (-not (Test-Path -LiteralPath $bash) -or -not (Test-Path -LiteralPath $python)) {
    throw 'MSYS2 bash and MINGW64 Python are required; see docs/emulator-audio.md.'
}
& $python (Join-Path $repoRoot 'emulator\qemu\scripts\prepare-build.py') $QemuSrc
if ($LASTEXITCODE -ne 0) { throw 'Preparing the QEMU build failed; see the reason above.' }
& (Join-Path $repoRoot 'emulator\qemu\scripts\install-machine.ps1') -QemuSrc $QemuSrc
$savedMsystem = $env:MSYSTEM
$savedPath = $env:PATH
try {
    $env:MSYSTEM = 'MINGW64'
    $env:PATH = (Join-Path $Msys64 'mingw64\bin') + ';' + $savedPath
    $compileScript = (Join-Path $repoRoot 'scripts/targets/qemu-compile.sh').Replace('\', '/')
    & $bash $compileScript $QemuSrc $Jobs $DisplayMode
    if ($LASTEXITCODE -ne 0) { throw "Build failed; logs are in $QemuSrc." }
    Write-Host "Ready: $QemuSrc\build\qemu-system-arm.exe"
    if ($DisplayMode -eq 'desktop') {
        & $python (Join-Path $repoRoot 'emulator\qemu\scripts\package-windows.py') --qemu-source $QemuSrc --prefix (Join-Path $Msys64 'mingw64') --output (Join-Path $repoRoot 'build\qemu-desktop')
        if ($LASTEXITCODE -ne 0) { throw 'Packaging the desktop build failed.' }
    }
} finally {
    $env:MSYSTEM = $savedMsystem
    $env:PATH = $savedPath
}
