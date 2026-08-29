# B310E-OS - tools/fpmain/rebuild-games.ps1
#
# Rebuilds the fpdoom game binaries for the B310E SD card with FAT_WRITE=0
# (read-only FAT) so the phone can never corrupt the card again. The two
# emulators that must save games - gnuboy (GBC) and snes9x/snes9x_16bit
# (SNES) - are built with FAT_WRITE=1.
#
# Prereqs (same as build-fpmain.ps1): arm-none-eabi-gcc + WinLibs gcc + make
# in PATH, git bash usr/bin in PATH for sh, pack_reloc.exe built in
# fpdoom/pack_reloc/. Network access to github.com (sources re-downloaded).
#
# Usage: powershell -ExecutionPolicy Bypass -File tools\fpmain\rebuild-games.ps1
#
# Outputs the rebuilt .bin files into tools\fpmain\games-out\ and copies
# them into sdcard\fpbin\.

$ErrorActionPreference = "Stop"
$Repo   = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
# The fpdoom clone lives in the repo's build/ tree (gitignored, removed by
# `make clean`) - cloned here automatically if missing. Override with FPDOOM_DIR.
$Fpdoom = if ($env:FPDOOM_DIR) { $env:FPDOOM_DIR } else { Join-Path $Repo "build\fpdoom" }
$Out    = Join-Path $PSScriptRoot "games-out"
$SdBin  = Join-Path $Repo "sdcard\fpbin"

if (-not (Test-Path (Join-Path $Fpdoom ".git"))) {
    Write-Output "cloning fpdoom -> $Fpdoom"
    New-Item -ItemType Directory -Force -Path (Split-Path $Fpdoom) | Out-Null
    git clone https://github.com/ilyakurdyukov/fpdoom $Fpdoom
    if ($LASTEXITCODE -ne 0) { Write-Error "fpdoom clone failed" }
}
if (-not (Test-Path (Join-Path $Fpdoom "pack_reloc\pack_reloc.exe"))) {
    Write-Output "building fpdoom pack_reloc"
    # the clone's pack_reloc needs inc/elf.h (mingw lacks it) - vendored in
    # the repo at tools/pack_reloc/inc/elf.h
    New-Item -ItemType Directory -Force -Path (Join-Path $Fpdoom "pack_reloc\inc") | Out-Null
    Copy-Item (Join-Path $Repo "tools\pack_reloc\inc\elf.h") (Join-Path $Fpdoom "pack_reloc\inc\elf.h") -Force
    Push-Location (Join-Path $Fpdoom "pack_reloc")
    try {
        make CC=gcc "CFLAGS=-O2 -Wall -Wextra -std=c99 -pedantic -Wno-unused -I inc"
        if ($LASTEXITCODE -ne 0) { Write-Error "pack_reloc build failed" }
    } finally { Pop-Location }
}
if (-not (Test-Path (Join-Path $Fpdoom "build_sc6531.make"))) {
    Write-Error "fpdoom clone incomplete at $Fpdoom - missing build_sc6531.make"
}

# git bash provides sh/unzip/patch; make + gcc come from PATH
$env:Path = "C:\Program Files\Git\usr\bin;" + $env:Path

# wget/cc shims (bash scripts in the clone's bin-shim/) - the helper.make
# download rules call wget, and some Makefiles call cc for host tools. The
# shims must be on PATH ahead of git-bash's bin (git-bash has no wget/cc).
# NOTE: single-quoted here-strings - the $ must NOT be interpolated.
$shimdir = Join-Path $Fpdoom "bin-shim"
New-Item -ItemType Directory -Force -Path $shimdir | Out-Null
@'
#!/bin/sh
# wget shim -> Windows curl (fpdoom helper.make download rules call wget -O file URL)
args=""
while [ "$#" -gt 0 ]; do
    if [ "$1" = "-O" ]; then
        args="$args -o"
        shift
    fi
    args="$args $1"
    shift
done
exec /c/Windows/system32/curl.exe -L -sS $args
'@ | Set-Content (Join-Path $shimdir 'wget') -Encoding ASCII

# WinLibs gcc + make + ARM toolchain, resolved from LOCALAPPDATA (the winget
# package cache - no hardcoded user path). MSYS-style for the bash shim.
function Get-WinGetBin($pattern, $exe) {
    $p = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\$pattern\$exe" `
             -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($p) { $p.DirectoryName } else { $null }
}
function To-MsysPath($winPath) {
    if (-not $winPath) { return $null }
    return "/" + $winPath.Substring(0,1).ToLower() + $winPath.Substring(2).Replace('\','/')
}
$makeDir = Get-WinGetBin 'ezwinports.make*' 'bin\make.exe'
$gccDir  = Get-WinGetBin 'BrechtSanders.WinLibs.POSIX.UCRT*' 'mingw64\bin\gcc.exe'
if (-not $makeDir) { Write-Error "ezwinports.make not found under LOCALAPPDATA\Microsoft\WinGet\Packages" }
if (-not $gccDir)  { Write-Error "WinLibs gcc not found under LOCALAPPDATA\Microsoft\WinGet\Packages" }

$gccMsys = To-MsysPath (Join-Path $gccDir 'gcc.exe')
@"
#!/bin/sh
# cc shim -> WinLibs gcc (retris Makefile uses HOSTCC=cc)
exec $gccMsys `"`$@`"
"@ | Set-Content (Join-Path $shimdir 'cc') -Encoding ASCII
$env:Path = "$shimdir;" + $env:Path

# full toolchain PATH
$env:Path = "C:\Program Files (x86)\Arm GNU Toolchain arm-none-eabi\14.2 rel1\bin;" +
            "$makeDir;" +
            "$gccDir;" +
            $env:Path

# sources: dir, make targets, build NAME=... GAME=...
# NOTE: fpblood, wolf3d, wolf3d_sw removed 2026-08-28 (don't work; no prebuilt
# replacement). infones/fpsw/fpduke3d AND snes9x/snes9x_16bit are also NOT
# locally built - our builds don't work on the B310E (GCC-14 port
# incompatibilities on a fresh clone), but the fpdoom repo's prebuilt_fix14.7z
# ones do, so all five are copied from there (see $PrebuiltBins).
$games = @(
    @{ Name = "fpdoom";          Make = "fpdoom";          Vars = "" },
    @{ Name = "chocolate-doom";  Make = "chocolate-doom";  Vars = "GAME=doom" },
    @{ Name = "chocolate-heretic"; Make = "chocolate-doom"; Vars = "GAME=heretic" },
    @{ Name = "chocolate-hexen"; Make = "chocolate-doom";  Vars = "GAME=hexen" },
    @{ Name = "retris";          Make = "retris";          Vars = "" },
    @{ Name = "gnuboy";          Make = "gnuboy";          Vars = "FAT_WRITE=1" }
)

# Bins copied from the fpdoom repo's prebuilt_fix14.7z release asset (our
# builds of these don't work on the B310E; the prebuilt ones do). Pinned to
# release 1.20251101. Downloaded once and cached; extracted into $Out so the
# staging loop below picks them up.
$PrebuiltRelease = "1.20251101"
$PrebuiltUrl     = "https://github.com/ilyakurdyukov/fpdoom/releases/download/$PrebuiltRelease/prebuilt_fix14.7z"
$PrebuiltCache   = Join-Path $Repo "build\prebuilt_fix14.7z"
$PrebuiltBins    = @("infones.bin", "fpsw.bin", "fpduke3d.bin", "snes9x.bin", "snes9x_16bit.bin")
$PrebuiltExtract = Join-Path $Out "prebuilt-extract"

New-Item -ItemType Directory -Force -Path $Out | Out-Null

# 0. download + patch all game sources. This is DELEGATED to
# tools/fpmain/fetch-sources.ps1 - the old in-line `make -f helper.make
# ZIPDIR=. all patch` loop is BROKEN (helper.make's patch target re-extracts
# with partial src/* globs that miss subdirs like src/doom, overwriting the
# full tree; fetch-sources.ps1 does a full unzip + direct patch -p1, which is
# the only path that works on a fresh clone). fetch-sources.ps1 skips sources
# already present, so re-runs are no-ops.
# Run as a CHILD process: fetch-sources.ps1 sets $ErrorActionPreference=Stop
# and uses Write-Error, which in a same-session & invocation would terminate
# THIS script too; a child process isolates it and returns a real exit code.
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "fetch-sources.ps1")
if ($LASTEXITCODE -ne 0) { throw "fetch-sources.ps1 failed (exit $LASTEXITCODE)" }

# 1. build each game
foreach ($g in $games) {
    $dir = Join-Path $Fpdoom $g.Make
    $bin = Join-Path $dir "$($g.Name).bin"
    $dest = Join-Path $Out "$($g.Name).bin"
    Write-Output "=== build $($g.Name) [$($g.Vars)] ==="
    Push-Location $dir
    try {
        # GCC 14 makes -Wincompatible-pointer-types a hard ERROR; the fpdoom
        # ports were written for older GCCs where it was a warning. The clone
        # is not restored between game builds, so patch each port's Makefile
        # once (marker-guarded, idempotent) to downgrade it. The fpdoom
        # release CI hits the same wall on modern GCC.
        $mk = Join-Path $dir "Makefile"
        if ((Test-Path $mk) -and -not (Select-String -Path $mk -Pattern 'Wno-error=incompatible-pointer-types' -Quiet)) {
            Add-Content -Path $mk -Value "CFLAGS += -Wno-error=incompatible-pointer-types" -Encoding ASCII
            Write-Output "  patched Makefile: -Wno-error=incompatible-pointer-types (GCC 14 compat)"
        }
        # snes9x port defines time() but omits <time.h> (works on old glibc
        # where stdio.h pulled it in; fails on the B310E toolchain).
        $sfp = Join-Path $dir "snes9x_fp.c"
        if ((Test-Path $sfp) -and -not (Select-String -Path $sfp -Pattern '#include <time.h>' -Quiet)) {
            $c = Get-Content $sfp -Raw
            $c = $c -replace '#include <stdio.h>', "#include <stdio.h>`r`n#include <time.h>"
            Set-Content $sfp -Value $c -NoNewline
            Write-Output "  patched snes9x_fp.c: #include <time.h>"
        }
        make clean LIBC_SDIO=3 TOOLCHAIN=arm-none-eabi CHIP=3
        # Split Vars on whitespace so multi-token vars (e.g. "FAT_WRITE=1
        # NAME=snes9x_16bit") reach make as SEPARATE arguments - PS 5.1 passes
        # an unquoted expression as ONE argv (no word-splitting), and make then
        # parses "FAT_WRITE=1 NAME=snes9x_16bit" as a single definition with
        # FAT_WRITE = "1 NAME=snes9x_16bit", leaking the literal into CFLAGS.
        $vars = @()
        if ($g.Vars) { $vars = $g.Vars -split '\s+' }
        make all   LIBC_SDIO=3 TOOLCHAIN=arm-none-eabi CHIP=3 @vars
        if ($LASTEXITCODE -ne 0) { throw "build failed (exit $LASTEXITCODE)" }
        if (-not (Test-Path $bin)) {
            # some Makefiles name the bin from NAME itself
            $bin = Join-Path $dir "$($g.Name).bin"
        }
        Copy-Item $bin $dest -Force
        Write-Output "  -> $dest ($((Get-Item $dest).Length) B)"
    } finally { Pop-Location }
}

# 1.5 prebuilt bins (infones/fpsw/fpduke3d - our builds don't work on the
# B310E, the fpdoom prebuilt_fix14 release ones do). Download once (cached),
# extract into $Out so the staging loop picks them up.
# Windows System32 tar.exe (bsdtar) reads 7z; the git-bash tar on PATH cannot.
$havePrebuilt = $true
foreach ($b in $PrebuiltBins) {
    if (-not (Test-Path (Join-Path $Out $b))) { $havePrebuilt = $false; break }
}
if (-not $havePrebuilt) {
    Write-Output "=== prebuilt: fetching $PrebuiltUrl ==="
    if (-not (Test-Path $PrebuiltCache)) {
        curl.exe -sL -o $PrebuiltCache $PrebuiltUrl
        if ($LASTEXITCODE -ne 0) { throw "prebuilt download failed (exit $LASTEXITCODE)" }
    }
    New-Item -ItemType Directory -Force -Path $PrebuiltExtract | Out-Null
    foreach ($b in $PrebuiltBins) {
        & "$env:SystemRoot\System32\tar.exe" -xf $PrebuiltCache -C $PrebuiltExtract "sdcard/fpbin/$b"
        if ($LASTEXITCODE -ne 0) { throw "prebuilt extract failed: $b" }
        Copy-Item (Join-Path $PrebuiltExtract "sdcard\fpbin\$b") (Join-Path $Out $b) -Force
        Write-Output "  -> $b ($((Get-Item (Join-Path $Out $b)).Length) B, prebuilt)"
    }
    Remove-Item -Recurse -Force $PrebuiltExtract -ErrorAction SilentlyContinue
} else {
    Write-Output "=== prebuilt: already present (infones/fpsw/fpduke3d) ==="
}

# 2. stage into sdcard/fpbin
New-Item -ItemType Directory -Force -Path $SdBin | Out-Null
Get-ChildItem $Out -Filter *.bin | ForEach-Object {
    Copy-Item $_.FullName (Join-Path $SdBin $_.Name) -Force
}
Write-Output "staged into: $SdBin"
