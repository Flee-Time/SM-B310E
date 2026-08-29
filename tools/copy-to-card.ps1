# Copy the staged fpdoom card build to the SD card and hash-verify.
# Usage:  powershell -ExecutionPolicy Bypass -File tools\copy-to-card.ps1
# Requires: card reader mounted as E: (change $Card below if different).
$ErrorActionPreference = 'Stop'

$Repo   = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$Stage  = Join-Path $Repo 'sdcard\fpbin'
$Card   = 'E:\fpbin'

if (-not (Test-Path $Stage)) { Write-Error "staging not found: $Stage"; exit 1 }
if (-not (Test-Path $Card))  { Write-Error "card not mounted at $Card - plug in the card reader"; exit 1 }

$files = @('infones.bin','chocolate-doom.bin','chocolate-heretic.bin','chocolate-hexen.bin','gnuboy.bin','config.json')
$bad = 0
foreach ($f in $files) {
    $src = Join-Path $Stage $f
    $dst = Join-Path $Card $f
    if (-not (Test-Path $src)) { Write-Error "missing in staging: $f"; $bad++; continue }
    Copy-Item -Force $src $dst
    $h1 = (Get-FileHash $src -Algorithm SHA256).Hash
    $h2 = (Get-FileHash $dst -Algorithm SHA256).Hash
    if ($h1 -eq $h2) { Write-Output "OK   $f" } else { Write-Output "DIFF $f"; $bad++ }
}
Write-Output ''
if ($bad -eq 0) { Write-Output 'ALL COPIED AND VERIFIED' } else { Write-Output "$bad file(s) FAILED" }
exit $bad
