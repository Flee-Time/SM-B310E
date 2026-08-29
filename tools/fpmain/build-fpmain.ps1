# B310E-OS - tools/fpmain/build-fpmain.ps1
#
# Builds the PORTED fpmain.bin: our boot menu (main.c + menu_stub.s +
# font8x16.h from this directory) staged into the clean fpdoom clone's
# fpmenu/ app, built with the fpdoom framework, then copied back here and
# staged on the card (sdcard/fpbin/fpmain.bin). The clone is restored to
# pristine after the build (git checkout), so the reference stays clean.
#
# Prereqs (see docs/sdboot.md): arm-none-eabi-gcc + WinLibs gcc + make in
# PATH, git bash usr/bin in PATH for sh, pack_reloc.exe built once in
# fpdoom/pack_reloc/.
#
# Usage:  powershell -ExecutionPolicy Bypass -File tools\fpmain\build-fpmain.ps1

$ErrorActionPreference = "Stop"
$Repo  = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
# The fpdoom clone lives in the repo's build/ tree (gitignored, removed by
# `make clean`) - cloned here automatically if missing. Override the location
# with FPDOOM_DIR.
$Fpdoom = if ($env:FPDOOM_DIR) { $env:FPDOOM_DIR } else { Join-Path $Repo "build\fpdoom" }
$Fpmenu = Join-Path $Fpdoom "fpmenu"

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
if (-not (Test-Path (Join-Path $Fpmenu "Makefile"))) {
    Write-Error "fpdoom clone incomplete at $Fpdoom - missing fpmenu/Makefile"
}

# 1. stage our sources over the stock fpmenu app sources
Copy-Item (Join-Path $PSScriptRoot "main.c")     (Join-Path $Fpmenu "main.c")     -Force
Copy-Item (Join-Path $PSScriptRoot "menu_stub.s") (Join-Path $Fpmenu "menu_stub.s") -Force
Copy-Item (Join-Path $PSScriptRoot "font8x16.h") (Join-Path $Fpmenu "font8x16.h") -Force
Copy-Item (Join-Path $PSScriptRoot "font5x7.h")  (Join-Path $Fpmenu "font5x7.h")  -Force
Copy-Item (Join-Path $PSScriptRoot "readconf.h") (Join-Path $Fpmenu "readconf.h") -Force
Copy-Item (Join-Path $PSScriptRoot "jsonconf.h") (Join-Path $Fpmenu "jsonconf.h") -Force

# 2. add menu_stub to the part1 app sources (stub symbols then get the
#    part1 relocations, so main.c's memcpy sees the runtime addresses)
$mk = Join-Path $Fpmenu "Makefile"
$text = Get-Content $mk -Raw
$text = $text -replace '(?m)^APP_SRCS = main\r?$', 'APP_SRCS = main menu_stub'
Set-Content -Path $mk -Value $text -NoNewline

# 3. build (git bash on PATH provides sh for the Makefile's mkdir -p/cat)
$env:Path = "C:\Program Files\Git\usr\bin;" + $env:Path
Push-Location $Fpmenu
try {
    make NAME=fpmain LIBC_SDIO=3 TOOLCHAIN=arm-none-eabi
    if ($LASTEXITCODE -ne 0) { throw "make failed (exit $LASTEXITCODE)" }
} finally {
    Pop-Location
}

# 4. copy the result back + stage on the card
$Out = Join-Path $PSScriptRoot "fpmain.bin"
Copy-Item (Join-Path $Fpmenu "fpmain.bin") $Out -Force
$Card = Join-Path $Repo "sdcard\fpbin"
New-Item -ItemType Directory -Force -Path $Card | Out-Null
Copy-Item $Out (Join-Path $Card "fpmain.bin") -Force
# stage the menu configs too (config.json is the primary, config.txt legacy)
Copy-Item (Join-Path $PSScriptRoot "config.json") (Join-Path $Card "config.json") -Force
# config.txt (the legacy line-based PORTS menu) is NOT staged by default -
# a fallback the user can find and copy manually if wanted.
#if (Test-Path (Join-Path $PSScriptRoot "config.txt")) {
#    Copy-Item (Join-Path $PSScriptRoot "config.txt") (Join-Path $Card "config.txt") -Force
#}
Write-Output "built: $Out"

# 5. restore the clone (tracked files) + drop build artifacts
Push-Location $Fpdoom
try {
    git checkout -- fpmenu
    Remove-Item -Recurse -Force (Join-Path $Fpmenu "obj0")   -ErrorAction SilentlyContinue
    Remove-Item -Force (Join-Path $Fpmenu "fpmain.bin")      -ErrorAction SilentlyContinue
    Remove-Item -Force (Join-Path $Fpmenu "menu_stub.s")     -ErrorAction SilentlyContinue
    Remove-Item -Force (Join-Path $Fpmenu "font5x7.h")       -ErrorAction SilentlyContinue
    # NOTE: readconf.h is NOT removed here - it is a TRACKED file in the
    # fpdoom repo (git checkout -- fpmenu above restored it).
} finally {
    Pop-Location
}
Write-Output "clone restored; card staged: $Card\fpmain.bin"
