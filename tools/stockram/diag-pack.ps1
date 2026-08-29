# diag-pack.ps1 - build the INSTRUMENTED stock-ram image (hardware debug)
#
# Builds stock-ram.bin (via pack-stockram.ps1) and splices three diagnostic
# pieces into the image:
#   1. vectors.s   at image offset 0x000  - exception vector table (all
#      exceptions -> keylight OFF + hang). The PBL boot vector lives here
#      but never runs in our RAM-boot, so the region is free.
#   2. aliastest.s at image offset 0x100 - alias self-test entered by the
#      shim's `bx 0x100`: keylight OFF ~0.8s then ON, then jumps to the
#      real boot vector at 0x10000.
#   3. diag-stub.s at image offset 0xE000 - the instrumented stock entry
#      (boot vector 0x10020 patched 0xbf150 -> 0xE000).
# Produces stock-ram-diag.bin. See docs/stockram.md for the marker table.
#
# Usage: powershell -ExecutionPolicy Bypass -File tools\stockram\diag-pack.ps1

$ErrorActionPreference = 'Stop'

$repo    = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$armbin  = 'C:\Program Files (x86)\Arm GNU Toolchain arm-none-eabi\14.2 rel1\bin'
$imgoff  = 0x800          # image starts at this file offset (shim size)
$vec     = $imgoff + 0x10020   # main OS boot vector entry pointer

$env:Path = "$armbin;" + $env:Path

function AssembleS($name) {
    $s = Join-Path $PSScriptRoot "$name.s"
    $o = Join-Path $PSScriptRoot "$name.o"
    $b = Join-Path $PSScriptRoot "$name.bin"
    & arm-none-eabi-gcc.exe -c -march=armv5te -o $o $s
    if ($LASTEXITCODE -ne 0) { throw "$name.s assembly failed" }
    & arm-none-eabi-objcopy.exe -O binary -j .text $o $b
    if ($LASTEXITCODE -ne 0) { throw "$name objcopy failed" }
    return [System.IO.File]::ReadAllBytes($b)
}

# 1. fresh base image
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'pack-stockram.ps1')
if ($LASTEXITCODE -ne 0) { throw 'pack-stockram.ps1 failed' }

# 2. assemble the diagnostic pieces
$vectors = AssembleS 'vectors'
$aliast  = AssembleS 'aliastest'
$stub    = AssembleS 'diag-stub'

# 3. splice into the image
$src = [System.IO.File]::ReadAllBytes((Join-Path $repo 'stock-ram.bin'))
[System.Array]::Copy($vectors, 0, $src, $imgoff + 0x000, $vectors.Length)
[System.Array]::Copy($aliast,  0, $src, $imgoff + 0x100, $aliast.Length)
[System.Array]::Copy($stub,    0, $src, $imgoff + 0xE000, $stub.Length)
[BitConverter]::GetBytes([uint32]0xE000).CopyTo($src, $vec)
$out = Join-Path $repo 'stock-ram-diag.bin'
[System.IO.File]::WriteAllBytes($out, $src)

Write-Host ("==> wrote {0} ({1} bytes)" -f $out, $src.Length)
Write-Host ("==> boot vector -> 0xE000; vectors @0x0 ({0}B), alias test @0x100 ({1}B), stub @0xE000 ({2}B)" -f $vectors.Length, $aliast.Length, $stub.Length)
Write-Host '==> test with:  .\spd_dump.exe fdl nor_fdl1.bin 0x40004000 fdl ..\stock-ram-diag.bin ram'
Write-Host '==> KEYLIGHT (watch ~2s): ON ~0.3s then stays ON = aliased exec BROKEN;'
Write-Host '==>   ON -> OFF ~0.6s -> ON(final) = WORKS + match/assert; -> OFF(final) = WORKS + mismatch/full-boot;'
Write-Host '==>   ON -> OFF immediately = exception (vector table)'
