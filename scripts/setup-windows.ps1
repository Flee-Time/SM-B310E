<#
.SYNOPSIS
Install a clean MSYS2 MINGW64 build environment under D:\Toolchains.
.DESCRIPTION
Installs a checksum-verified portable MSYS2 base if missing, fully updates it,
and installs firmware/port/QEMU desktop dependencies. Does not remove other
installations or change the system PATH. The ARM compiler is separate.
#>
[CmdletBinding()]
param(
    [string]$ToolchainsRoot = 'D:\Toolchains',
    [switch]$SetUserEnvironment
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$ToolchainsRoot = [IO.Path]::GetFullPath($ToolchainsRoot)
if ($ToolchainsRoot -match '[^\x20-\x7e]|\s' -or $ToolchainsRoot.StartsWith('\\')) {
    throw 'Use a local ASCII toolchains path without spaces.'
}
New-Item -ItemType Directory -Path $ToolchainsRoot -Force | Out-Null
$msys = Join-Path $ToolchainsRoot 'msys64'
$bash = Join-Path $msys 'usr\bin\bash.exe'
if (-not (Test-Path -LiteralPath $bash)) {
    if (Test-Path -LiteralPath $msys) { throw "Refusing to extract over an incomplete directory: $msys" }
    $downloads = Join-Path $ToolchainsRoot 'downloads'
    New-Item -ItemType Directory -Path $downloads -Force | Out-Null
    $archive = Join-Path $downloads 'msys2-base-x86_64-20260927.sfx.exe'
    $url = 'https://github.com/msys2/msys2-installer/releases/download/2026-09-27/msys2-base-x86_64-20260927.sfx.exe'
    $hash = 'ad336cccfda47758b5e15cda993fbba421115cb0b126697daef1ee4dfe37209f'
    if (-not (Test-Path -LiteralPath $archive)) {
        Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $archive
    }
    if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant() -ne $hash) {
        throw "MSYS2 archive checksum mismatch: $archive"
    }
    $extract = Start-Process -FilePath $archive -ArgumentList @('-y', "-o$ToolchainsRoot") -WindowStyle Hidden -Wait -PassThru
    if ($extract.ExitCode -ne 0 -or -not (Test-Path -LiteralPath $bash)) { throw 'MSYS2 extraction failed.' }
}
$savedMsystem = $env:MSYSTEM
$savedPath = $env:PATH
try {
    $env:MSYSTEM = 'MINGW64'
    $env:PATH = (Join-Path $msys 'mingw64\bin') + ';' + (Join-Path $msys 'usr\bin') + ';' + $savedPath
    # Initialize a new portable base once; MSYS2's core upgrade may close its shell.
    & $bash -lc 'true'
    if ($LASTEXITCODE -ne 0) { throw 'MSYS2 initialization failed.' }
    & $bash -c 'export PATH=/usr/bin:/bin:$PATH; pacman -Sy --noconfirm && pacman -Su --noconfirm'
    # Start a fresh shell to finish upgrades after a possible MSYS runtime restart.
    & $bash -c 'export PATH=/usr/bin:/bin:$PATH; pacman -Syu --noconfirm'
    if ($LASTEXITCODE -ne 0) { throw 'MSYS2 update failed.' }
    $packages = @('base-devel','git','zip','unzip','p7zip','perl','curl',
        'mingw-w64-x86_64-gcc','mingw-w64-x86_64-python',
        'mingw-w64-x86_64-python-setuptools','mingw-w64-x86_64-python-wheel',
        'mingw-w64-x86_64-meson','mingw-w64-x86_64-ninja','mingw-w64-x86_64-pkgconf',
        'mingw-w64-x86_64-glib2','mingw-w64-x86_64-pixman','mingw-w64-x86_64-dtc',
        'mingw-w64-x86_64-libpng','mingw-w64-x86_64-gtk3','mingw-w64-x86_64-SDL2',
        'mingw-w64-x86_64-libepoxy')
    & $bash -c 'export PATH=/usr/bin:/bin:$PATH; exec pacman -S --needed --noconfirm "$@"' b310e @packages
    if ($LASTEXITCODE -ne 0) { throw 'Installing build dependencies failed.' }
    & $bash -c 'set -e; export PATH=/mingw64/bin:/usr/bin:$PATH; pkg-config --modversion gtk+-3.0 sdl2 epoxy; gcc --version; python --version'
    if ($LASTEXITCODE -ne 0) { throw 'Checking installed tools failed.' }
} finally {
    $env:MSYSTEM = $savedMsystem
    $env:PATH = $savedPath
}
if ($SetUserEnvironment) {
    [Environment]::SetEnvironmentVariable('B310E_MSYS', $msys, 'User')
    $arm = Join-Path $ToolchainsRoot 'arm-none-eabi\bin'
    if (Test-Path -LiteralPath (Join-Path $arm 'arm-none-eabi-gcc.exe')) {
        [Environment]::SetEnvironmentVariable('B310E_TOOLCHAIN', $arm, 'User')
    }
}
Write-Host "MSYS2 / MINGW64 ready: $msys"
Write-Host "ARM compiler directory: $ToolchainsRoot\arm-none-eabi\bin"
Write-Host "Build with: .\build.ps1 qemu --msys $msys"
