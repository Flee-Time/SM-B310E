# B310E-OS hardware test harness
# Usage (from repo root):
#   powershell -ExecutionPolicy Bypass -File tools\run-hw-test.ps1
#
# What it does:
#   1. Loads os.bin into the phone's RAM via spd_dump (boot key = CENTER held
#      while connecting USB). NO flash is written - zero brick risk.
#   2. Prints exactly what to expect next on the phone screen + libc_server.
#
# Environment variables (optional overrides):
#   $env:SPD_DIR   - spd_dump folder (default: <repo>\tools\spd_dump)
#   $env:OS_BIN    - firmware image (default: <repo>\build\bin\os.bin)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$spd = $env:SPD_DIR
if (-not $spd) { $spd = Join-Path $repo "tools\spd_dump" }
$osbin = $env:OS_BIN
if (-not $osbin) { $osbin = Join-Path $repo "build\bin\os.bin" }

if (-not (Test-Path (Join-Path $spd "spd_dump.exe"))) {
    Write-Host "ERROR: spd_dump.exe not found in $spd" -ForegroundColor Red
    exit 1
}
if (-not (Test-Path $osbin)) {
    Write-Host "ERROR: $osbin not found - run 'make' first (see BUILD.md)" -ForegroundColor Red
    exit 1
}

Write-Host "=== B310E-OS hardware test ===" -ForegroundColor Cyan
Write-Host "Image : $osbin ($((Get-Item $osbin).Length) bytes)"
Write-Host "SHA256: $((Get-FileHash $osbin -Algorithm SHA256).Hash)"
Write-Host ""
Write-Host "NOW: remove battery, hold the D-pad CENTER key, plug USB, keep holding." -ForegroundColor Yellow
Write-Host "spd_dump is waiting for the phone (USB 1782:4d00)..." -ForegroundColor Yellow
Write-Host ""

Push-Location $spd
try {
    & .\spd_dump.exe fdl nor_fdl1.bin 0x40004000 fdl $osbin ram
    $code = $LASTEXITCODE
} finally {
    Pop-Location
}

Write-Host ""
if ($code -ne 0) {
    Write-Host "FAIL: spd_dump exited with code $code (see output above)." -ForegroundColor Red
    Write-Host "  - No 'CHIP ID' reply -> boot key wrong / USB driver (Zadig/WinUSB for 1782:4d00)."
    Write-Host "  - 'timeout reached'   -> code crashed before the fdl_ack (report the console)."
    exit $code
}

Write-Host "PASS: spd_dump completed - our firmware is running in RAM." -ForegroundColor Green
Write-Host ""
Write-Host "Next, check the PHONE SCREEN:" -ForegroundColor Cyan
Write-Host "  Expected: blue background, white 'B310E-OS v0.4', green 'kernel: alive',"
Write-Host "            blinking pixel bottom-right. (No colorful noise.)"
Write-Host ""
Write-Host "Then start the USB log console (new terminal, from <repo>\tools\libc_server):"
Write-Host "  .\libc_server.exe" -ForegroundColor Green
Write-Host "  Expected: !!! banner: up, !!! keypad: up, !!! banner: tick 32/64...,"
Write-Host "            !!! key: <NAME> when you press keys."
Write-Host ""
Write-Host "AUDIO TEST (first integration):" -ForegroundColor Cyan
Write-Host "  Press LSOFT once -> expect a short 440 Hz square-wave beep from the"
Write-Host "  speaker (drivers/audio.c: powers on-die codec + PA, toggles DAC gain)."
Write-Host "  If silent: report whether the phone hangs (bad register), or hear only a"
Write-Host "  click/pop (codec powered, amp/GPIO wrong -> run the AMP_EN GPIO probe)."
Write-Host ""
Write-Host "Reboot (remove battery) to return to the stock Samsung OS - nothing was flashed." -ForegroundColor DarkGray
exit 0
