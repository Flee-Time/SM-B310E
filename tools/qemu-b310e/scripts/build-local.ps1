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
    [ValidateRange(1,128)][int]$Jobs = 8,
    [switch]$SkipConfigure
)
$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
if (-not $QemuSrc) {
    $QemuSrc = Join-Path (Split-Path $repoRoot -Parent) '.tools\qemu-b310e-src'
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
& (Join-Path $PSScriptRoot 'install-machine.ps1') -QemuSrc $QemuSrc
& $python (Join-Path $PSScriptRoot 'prepare-headless-build.py') $QemuSrc
if ($LASTEXITCODE -ne 0) { throw 'Preparing the QEMU build failed.' }
$savedMsystem = $env:MSYSTEM
$savedPath = $env:PATH
try {
    $env:MSYSTEM = 'MINGW64'
    $env:PATH = (Join-Path $Msys64 'mingw64\bin') + ';' + $savedPath
    $env:B310E_QEMU_SOURCE = $QemuSrc
    $env:B310E_BUILD_JOBS = $Jobs.ToString()
    $env:B310E_SKIP_CONFIGURE = if ($SkipConfigure) { '1' } else { '0' }
    # All paths are passed as environment data and quoted inside the shell.
    $buildScript = @'
set -e
export PATH=/mingw64/bin:/usr/bin:$PATH
cd "$(cygpath -u "$B310E_QEMU_SOURCE")"
options='--target-list=arm-softmmu --enable-png --disable-gtk --disable-sdl --disable-werror --disable-docs --disable-tools --disable-guest-agent --disable-download'
if [ "$B310E_SKIP_CONFIGURE" = 0 ]; then
    if [ ! -f build/config-host.mak ] || [ ! -f build/b310e-configure-options ] || [ "$(cat build/b310e-configure-options)" != "$options" ]; then
        ./configure $options > b310e-configure.log 2>&1 || { tail -60 b310e-configure.log; exit 1; }
        printf '%s' "$options" > build/b310e-configure-options
    fi
fi
ninja -C build -j"$B310E_BUILD_JOBS" qemu-system-arm.exe > b310e-build.log 2>&1 || { tail -60 b310e-build.log; exit 1; }
tail -8 b310e-build.log
build/qemu-system-arm.exe --version
'@
    & $bash -c $buildScript
    if ($LASTEXITCODE -ne 0) { throw "Build failed; logs are in $QemuSrc." }
    Write-Host "Ready: $QemuSrc\build\qemu-system-arm.exe"
} finally {
    $env:MSYSTEM = $savedMsystem
    $env:PATH = $savedPath
    Remove-Item Env:B310E_QEMU_SOURCE, Env:B310E_BUILD_JOBS, Env:B310E_SKIP_CONFIGURE -ErrorAction SilentlyContinue
}
