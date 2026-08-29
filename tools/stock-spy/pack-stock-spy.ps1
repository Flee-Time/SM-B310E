# pack-stock-spy.ps1 — package the B310E stock firmware as a RAM-loadable
# image with the ADI-write spy: boots the STOCK OS from PSRAM and hooks its
# ADI write helper (flash 0x3038A) so every analog-register write the stock
# makes during the boot chime / song playback is logged to a PSRAM ring.
#
# Layout of stock-spy.bin (loaded by spd_dump at 0x34000000):
#   [0x000000]  shim-spy (padded to 0x800; the stockram shim + a payload
#               copy step: image[0xE000:0x10000] -> PA 0x352F0800)
#   [0x000800]  dump_firmware.bin[0..0x100000]  (1 MB stock image)
#               - offset 0xE000: the 8 KB spy payload (blank PBL region)
#               - offset 0x3038A: the ADI-helper patch
#                   ldr pc, [pc, #0] / .long 0x042F0000  (6 bytes)
#
# Recovery (no SD, no custom FDL — spd_dump's read_mem is a raw memcpy):
#   after the spy's CENTER-hold watchdog reboot re-enters download mode:
#   .\spd_dump.exe fdl nor_fdl1.bin 0x40004000 `
#       read_mem 0x352F0800 0x2000 spy-rings.bin
#
# Usage:  powershell -ExecutionPolicy Bypass -File tools\stock-spy\pack-stock-spy.ps1

$ErrorActionPreference = 'Stop'

$repo     = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$armbin   = 'C:\Program Files (x86)\Arm GNU Toolchain arm-none-eabi\14.2 rel1\bin'
$dump     = Join-Path $repo 'dump_firmware.bin'
$out      = Join-Path $repo 'stock-spy.bin'
$build    = Join-Path $PSScriptRoot 'build'

$SHIM_PAD   = 0x800
$IMAGE_LEN  = 0x100000
$SPY_IMG_OFF = 0xE000     # payload slot in the image (blank PBL region)
$SPY_LEN     = 0x2000     # 8 KB payload
$PATCH_OFF   = 0x3038A    # the ADI write helper's store (str r7, [r6])
$HOOK_VA     = 0x042F0000 # payload VA = PA 0x352F0800

if (-not (Test-Path $dump)) { throw "dump_firmware.bin not found at $dump" }
if (-not (Test-Path (Join-Path $armbin 'arm-none-eabi-gcc.exe'))) {
    throw "ARM toolchain not found at $armbin"
}
New-Item -ItemType Directory -Force -Path $build | Out-Null
$env:Path = "$armbin;" + $env:Path

# 1. assemble the shim-spy + the hook
& arm-none-eabi-gcc.exe -c -march=armv5te -o (Join-Path $build 'shim-spy.o') (Join-Path $PSScriptRoot 'shim-spy.s')
if ($LASTEXITCODE -ne 0) { throw 'shim-spy.s assembly failed' }
& arm-none-eabi-objcopy.exe -O binary -j .text (Join-Path $build 'shim-spy.o') (Join-Path $build 'shim-spy.bin')
if ($LASTEXITCODE -ne 0) { throw 'shim objcopy failed' }
$shimBytes = [System.IO.File]::ReadAllBytes((Join-Path $build 'shim-spy.bin'))
if ($shimBytes.Length -gt $SHIM_PAD) {
    throw "shim-spy.bin is $($shimBytes.Length) bytes, larger than the $SHIM_PAD-byte image offset"
}

& arm-none-eabi-gcc.exe -c -march=armv5te -mthumb -o (Join-Path $build 'spy-hook.o') (Join-Path $PSScriptRoot 'spy-hook.s')
if ($LASTEXITCODE -ne 0) { throw 'spy-hook.s assembly failed' }
& arm-none-eabi-objcopy.exe -O binary -j .text (Join-Path $build 'spy-hook.o') (Join-Path $build 'spy-hook.bin')
if ($LASTEXITCODE -ne 0) { throw 'hook objcopy failed' }
$hookBytes = [System.IO.File]::ReadAllBytes((Join-Path $build 'spy-hook.bin'))
if ($hookBytes.Length -gt 0x400) {
    throw "spy-hook.bin is $($hookBytes.Length) bytes; must fit before ring1 at payload offset 0x400"
}
Write-Host ("==> hook: {0} bytes" -f $hookBytes.Length)

& arm-none-eabi-gcc.exe -c -march=armv5te -mthumb -o (Join-Path $build 'spy-uart-hook.o') (Join-Path $PSScriptRoot 'spy-uart-hook.s')
if ($LASTEXITCODE -ne 0) { throw 'spy-uart-hook.s assembly failed' }
& arm-none-eabi-objcopy.exe -O binary -j .text (Join-Path $build 'spy-uart-hook.o') (Join-Path $build 'spy-uart-hook.bin')
if ($LASTEXITCODE -ne 0) { throw 'uart hook objcopy failed' }
$uartBytes = [System.IO.File]::ReadAllBytes((Join-Path $build 'spy-uart-hook.bin'))
if ($uartBytes.Length -gt 0x100) {
    throw "spy-uart-hook.bin is $($uartBytes.Length) bytes; must fit before ring3 at payload offset 0x1D00"
}
Write-Host ("==> uart hook: {0} bytes" -f $uartBytes.Length)

# 2. build the 8 KB payload: ADI hook + rings + UART hook + ring3 + header
$payload = New-Object byte[] $SPY_LEN
[System.Array]::Copy($hookBytes, 0, $payload, 0, $hookBytes.Length)
[System.Array]::Copy($uartBytes, 0, $payload, 0x1C00, $uartBytes.Length)
# header at payload offset 0x1F00 (VA 0x042f1f00): all zeroed except the magic
$magic = 0x59505331   # "1SPY"
$payload[0x1F18] = $magic -band 0xFF
$payload[0x1F19] = ($magic -shr 8) -band 0xFF
$payload[0x1F1A] = ($magic -shr 16) -band 0xFF
$payload[0x1F1B] = ($magic -shr 24) -band 0xFF

# 3. slice + patch the stock image
$dumpBytes = [System.IO.File]::ReadAllBytes($dump)
if ($dumpBytes.Length -lt $IMAGE_LEN) { throw "dump_firmware.bin smaller than $IMAGE_LEN" }
$image = New-Object byte[] $IMAGE_LEN
[System.Array]::Copy($dumpBytes, 0, $image, 0, $IMAGE_LEN)

# 3a. verify the ADI helper store + pop at the patch site
$w1 = $image[$PATCH_OFF] + ($image[$PATCH_OFF + 1] * 256)
$w2 = $image[$PATCH_OFF + 2] + ($image[$PATCH_OFF + 3] * 256)
if ($w1 -ne 0x6037 -or $w2 -ne 0xBDF8) {
    throw "unexpected bytes at 0x3038A: 0x{0:X4}/0x{1:X4} (expected 0x6037 str r7,[r6] / 0xBDF8 pop)" -f $w1, $w2
}
# 3b. apply the ADI-helper patch: ldr pc, [pc, #0] + .long HOOK_VA
$image[$PATCH_OFF] = 0x00
$image[$PATCH_OFF + 1] = 0x4F
$image[$PATCH_OFF + 2] = $HOOK_VA -band 0xFF
$image[$PATCH_OFF + 3] = ($HOOK_VA -shr 8) -band 0xFF
$image[$PATCH_OFF + 4] = ($HOOK_VA -shr 16) -band 0xFF
$image[$PATCH_OFF + 5] = ($HOOK_VA -shr 24) -band 0xFF

# 3c. apply the UART-putc patch at 0x38416 (store str r5,[r4]): verify + patch
$UPATCH = 0x38416
$UHOOK_VA = 0x042F1C00
$uw1 = $image[$UPATCH] + ($image[$UPATCH + 1] * 256)
if ($uw1 -ne 0x6025) {
    throw "unexpected bytes at 0x38416: 0x{0:X4} (expected 0x6025 str r5,[r4])" -f $uw1
}
$image[$UPATCH] = 0x00
$image[$UPATCH + 1] = 0x4F
$image[$UPATCH + 2] = $UHOOK_VA -band 0xFF
$image[$UPATCH + 3] = ($UHOOK_VA -shr 8) -band 0xFF
$image[$UPATCH + 4] = ($UHOOK_VA -shr 16) -band 0xFF
$image[$UPATCH + 5] = ($UHOOK_VA -shr 24) -band 0xFF

# 3c. the payload slot (blank PBL region) must be empty flash (0xFF)
for ($i = $SPY_IMG_OFF; $i -lt ($SPY_IMG_OFF + $SPY_LEN); $i++) {
    if ($image[$i] -ne 0xFF) {
        throw "payload slot 0x{0:X5}-0x{1:X5} is not blank flash (byte 0x{2:X2} at 0x{3:X5})" -f `
            $SPY_IMG_OFF, ($SPY_IMG_OFF + $SPY_LEN), $image[$i], $i
    }
}
[System.Array]::Copy($payload, 0, $image, $SPY_IMG_OFF, $SPY_LEN)

# 4. assemble stock-spy.bin = shim + image
$outBytes = New-Object byte[] ($SHIM_PAD + $IMAGE_LEN)
[System.Array]::Copy($shimBytes, 0, $outBytes, 0, $shimBytes.Length)
[System.Array]::Copy($image, 0, $outBytes, $SHIM_PAD, $IMAGE_LEN)
[System.IO.File]::WriteAllBytes($out, $outBytes)

Write-Host ("==> wrote {0} ({1} bytes: {2} shim + {3} image, patch at 0x3038A, payload at 0xE000)" -f `
    $out, $outBytes.Length, $SHIM_PAD, $IMAGE_LEN)
Write-Host '==> test with:  .\spd_dump.exe fdl nor_fdl1.bin 0x40004000 fdl ..\stock-spy.bin ram'
Write-Host '==> after the chime, HOLD CENTER 3 s (spy watchdog-reboots into download mode), then:'
Write-Host '==>   .\spd_dump.exe fdl nor_fdl1.bin 0x40004000 read_mem 0x352F0800 0x2000 spy-rings.bin'
