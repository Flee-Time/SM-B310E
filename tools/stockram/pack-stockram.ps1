# pack-stockram.ps1 - package the ORIGINAL B310E firmware as a RAM-loadable
# image for spd_dump, with an MMU-alias shim that runs the stock main OS
# from PSRAM instead of NOR.
#
# Layout of the produced stock-ram.bin (loaded by spd_dump at 0x34000000):
#   [0x000000]  shim  (ARM, linked @0x34000000, padded to 0x800)
#   [0x000800]  dump_firmware.bin[0 .. 0x100000]  (1MB stock image copy)
#
# The shim sets up the MMU (VA 0x0-0x100000 -> 0x34000800, VA 0x04000000-
# 0x04300000 -> 0x35000800, rest identity) and jumps to the stock main OS
# boot vector at VA 0x10000.
#
# Usage:  powershell -ExecutionPolicy Bypass -File tools\stockram\pack-stockram.ps1

$ErrorActionPreference = 'Stop'

$repo    = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$armbin  = 'C:\Program Files (x86)\Arm GNU Toolchain arm-none-eabi\14.2 rel1\bin'
$src     = Join-Path $PSScriptRoot 'shim.s'
$shim    = Join-Path $PSScriptRoot 'shim.bin'
$dump    = Join-Path $repo 'dump_firmware.bin'
$out     = Join-Path $repo 'stock-ram.bin'

$SHIM_PAD  = 0x800     # image offset inside stock-ram.bin (must match shim.s IMAGE_OFF)
$IMAGE_LEN = 0x100000  # 1MB alias window (must match shim.s ALIAS_SIZE)

if (-not (Test-Path $dump)) { throw "dump_firmware.bin not found at $dump" }
if (-not (Test-Path (Join-Path $armbin 'arm-none-eabi-gcc.exe'))) {
    throw "ARM toolchain not found at $armbin"
}

# 1. assemble the shim
$env:Path = "$armbin;" + $env:Path
& arm-none-eabi-gcc.exe -c -march=armv5te -o (Join-Path $PSScriptRoot 'shim.o') $src
if ($LASTEXITCODE -ne 0) { throw 'shim.s assembly failed' }
& arm-none-eabi-objcopy.exe -O binary -j .text (Join-Path $PSScriptRoot 'shim.o') $shim
if ($LASTEXITCODE -ne 0) { throw 'shim objcopy failed' }

$shimBytes = [System.IO.File]::ReadAllBytes($shim)
if ($shimBytes.Length -gt $SHIM_PAD) {
    throw "shim.bin is $($shimBytes.Length) bytes, larger than the $SHIM_PAD-byte image offset"
}

# 2. slice the stock image (first 1MB of the dump)
$dumpBytes = [System.IO.File]::ReadAllBytes($dump)
if ($dumpBytes.Length -lt $IMAGE_LEN) { throw "dump_firmware.bin smaller than $IMAGE_LEN" }
$image = New-Object byte[] $IMAGE_LEN
[System.Array]::Copy($dumpBytes, 0, $image, 0, $IMAGE_LEN)

# 3. assemble stock-ram.bin = shim (padded) + image
$outBytes = New-Object byte[] ($SHIM_PAD + $IMAGE_LEN)
[System.Array]::Copy($shimBytes, 0, $outBytes, 0, $shimBytes.Length)
[System.Array]::Copy($image, 0, $outBytes, $SHIM_PAD, $IMAGE_LEN)
[System.IO.File]::WriteAllBytes($out, $outBytes)

Write-Host ("==> wrote {0} ({1} bytes: {2} shim + {3} image)" -f $out, $outBytes.Length, $SHIM_PAD, $IMAGE_LEN)
Write-Host '==> test with:  .\spd_dump.exe fdl nor_fdl1.bin 0x40004000 fdl ..\stock-ram.bin ram'
