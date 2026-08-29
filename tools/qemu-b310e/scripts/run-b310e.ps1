# run-b310e.ps1 - self-contained B310E QEMU machine run script (todo 34).
# Generalizes the Wave-6/7 capture scripts (logs/w6/run-w6-stock-sound.ps1,
# logs/w7/run-w7-ours.ps1) into one parameterized runner for all three boot
# modes. Runs from plain PowerShell (5.1+); UNC-safe (every path is derived
# from $PSScriptRoot or the -D log location, no Get-Location reliance).
#
#   qemu-system-arm.exe -M b310e,boot-mode=<Boot> -display none -serial none
#     -d int -D <log> [--trace <pattern> ...]
#     -monitor telnet:127.0.0.1:<MonitorPort>,server,nowait
#     -drive file=<Nor>,format=raw,if=none,id=nor
#     [-drive file=<Os>,format=raw,if=none,id=os]        # boot=ours only
#
# Sequence (FIXED SLEEPS - no log polling; Get-Content -Tail on a growing
# multi-GB in_asm log is unusably slow in PS 5.1, the W4 QA-lesson #2 trap):
#   warm|stock: keys every -KeyDelay s from t+5 s until t=TimeSec-8 s, then
#               screendump final-<boot>.png + info registers + quit.
#   ours:       -BannerSec wait -> screendump banner-{1,2}.png (3 s apart) ->
#               the key list (KeyDelay apart) -> banner-3.png -> cooldown to
#               -TimeSec -> final state + quit.
#
# The W4-20b trace lesson applies: use -d int (NOT -d in_asm - the os.bin
# USB_CONNECT_BUDGET poll burns ~4 min wall / ~11 GB under in_asm); and ONE
# --trace per pattern (a comma list "--trace a,b" kills qemu at startup).
#
# Usage examples:
#   # stock OS + audio observatory (the Wave-6 capture shape), 90 s:
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\qemu-b310e\scripts\run-b310e.ps1 `
#     -Boot warm -TimeSec 90 -D tools\qemu-b310e\logs\w7\final-sweep.log
#   # our os.bin banner + keys + tick evidence:
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\qemu-b310e\scripts\run-b310e.ps1 `
#     -Boot ours -D tools\qemu-b310e\logs\w7\ours-sweep.log
#   # bare stock boot with no keys, 30 s (-Keys "" = none; with -File invocation
#   # "-Keys ret,up" arrives as ONE literal string, so comma-separated values are
#   # normalized back into a list inside the script):
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\qemu-b310e\scripts\run-b310e.ps1 `
#     -Boot stock -TimeSec 30 -Keys "" -D tools\qemu-b310e\logs\w7\stock-sweep.log
#
# Evidence (all in the -D log's directory): the raw -D log, keys.txt (the
# sendkey injection journal), final-<boot>.png / banner-{1,2,3}.png
# screendumps, and final-regs.txt (info registers + sys-ms + LCDC img base).
param(
  [ValidateSet("warm", "stock", "ours")]
  [string]$Boot = "warm",
  [string]$Nor,                 # default: repo\tools\spd_dump\full-backup.bin
  [string]$Os,                  # default: repo\tools\qemu-b310e\logs\os-sdboot-era.bin (boot=ours)
  [string[]]$Trace,             # default per boot mode; ONE --trace per pattern
  [Parameter(Mandatory = $true)]
  [string]$D,                   # REQUIRED: the -D trace/log file (qemu_log evidence)
  [int]$MonitorPort = 4550,
  [int]$TimeSec = 60,           # total run budget (warm/stock key loop + final cooldown)
  [int]$KeyDelay = 5,           # seconds between sendkey injections
  [int]$BannerSec = 25,         # ours-only: wait for the banner before screendump #1
  [string[]]$Keys,              # default per boot mode (see below)
  [string]$Qemu = (Join-Path $env:USERPROFILE 'qemu-b310e\qemu-src\build\qemu-system-arm.exe')
)
$ErrorActionPreference = "Stop"

# ---- resolve paths (UNC-safe: everything derives from $PSScriptRoot) --------
$repo = Split-Path (Split-Path (Split-Path $PSScriptRoot -Parent) -Parent) -Parent
if (-not $Nor) { $Nor = Join-Path $repo "tools\spd_dump\full-backup.bin" }
if (-not $Os)  { $Os  = Join-Path $repo "tools\qemu-b310e\logs\os-sdboot-era.bin" }

# ---- defaults per boot mode --------------------------------------------------
if (-not $Trace) {
  if ($Boot -eq "ours") { $Trace = @("sc6530_lcdc_refresh", "sc6530_ana_*", "sc6530_aux_*") }
  else                  { $Trace = @("sc6530_ana_*", "sc6530_aux_*", "sc6530_dsp_cmd") }
}
if (-not $Keys) {
  if ($Boot -eq "ours") { $Keys = @('ret','up','down','left','right','1','5','f1','kp_enter','esc') }
  else                  { $Keys = @('ret','1','2','3','4','f1','f2','kp_enter','up','down','left','right','5','6','7') }
}
# Normalize: with -File invocation PowerShell passes "-Keys ret,up" as ONE literal
# string ("ret,up") instead of two elements - split on commas so both forms work.
$Keys = @($Keys | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
foreach ($t in $Trace) {
  if ($t -match ',') { Write-Host "WARN: trace pattern '$t' contains a comma - qemu would parse it as options (W4-20b lesson); one --trace per pattern is the rule." }
}

# ---- sanity checks -----------------------------------------------------------
if (-not (Test-Path -LiteralPath $Qemu)) {
  Write-Host "FAIL: qemu binary not found at $Qemu"; Write-Host "      run tools\qemu-b310e\scripts\build-qemu.ps1 first (or pass -Qemu)"; exit 1
}
if (-not (Test-Path -LiteralPath $Nor)) { Write-Host "FAIL: NOR image not found at $Nor"; exit 1 }
if ($Boot -eq "ours" -and -not (Test-Path -LiteralPath $Os)) {
  Write-Host "FAIL: os image not found at $Os (boot=ours needs the committed sdboot-era os.bin, or pass -Os)"; exit 1
}
$logFull = [IO.Path]::GetFullPath($D)
$logDir  = Split-Path $logFull -Parent
if (-not $logDir) { $logDir = $PWD.Path }
if (-not (Test-Path -LiteralPath $logDir)) { New-Item -ItemType Directory -Path $logDir -Force | Out-Null }
$logFull = Join-Path $logDir (Split-Path $logFull -Leaf)   # normalize: log lives in $logDir
$keysFile = Join-Path $logDir "keys.txt"
$regsFile = Join-Path $logDir "final-regs.txt"
$shot1 = "banner-1.png"; $shot2 = "banner-2.png"; $shot3 = "banner-3.png"
$shotF = "final-$Boot.png"

Write-Host "=== B310E QEMU run: boot=$Boot TimeSec=${TimeSec}s Trace=$($Trace -join ',') ==="
Write-Host "    log:      $logFull"
Write-Host "    nor:      $Nor"
if ($Boot -eq "ours") { Write-Host "    os:       $Os" }
Write-Host "    monitor:  telnet:127.0.0.1:$MonitorPort"
$expect = if ($Boot -eq "ours") { "LCDC refresh lines + screendump banner + key DOWN/UP edges + ~1k/s IRQ tick exceptions, ZERO Taking-exception aborts" } else { "sc6530_ana_* codec/ANA writes with guest PCs (the audio bring-up), sc6530_aux_* APB/PA-ladder + UNMODELED lines, sc6530_dsp_cmd lines" }
Write-Host "    expected: $expect"

# Preserve any prior run's log (evidence discipline).
if (Test-Path -LiteralPath $logFull) {
  $stamp = Get-Date -Format yyyyMMdd-HHmmss
  Copy-Item -LiteralPath $logFull -Destination "$logFull.prior-$stamp" -Force
  Write-Host "preserved prior log -> $logFull.prior-$stamp"
}

# Kill zombie qemu (todo-17 lesson: stale qemu holds the chardev port).
Get-Process qemu-system-arm -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 800

# ---- build the qemu command line --------------------------------------------
$qargs = @(
  "-M", "b310e,boot-mode=$Boot",
  "-display", "none", "-serial", "none",
  "-d", "int", "-D", $logFull,
  "-monitor", "telnet:127.0.0.1:$MonitorPort,server,nowait",
  "-drive", "file=$Nor,format=raw,if=none,id=nor"
)
foreach ($t in $Trace) { $qargs += @("--trace", $t) }
if ($Boot -eq "ours") { $qargs += @("-drive", "file=$Os,format=raw,if=none,id=os") }

$p = Start-Process -FilePath $Qemu -ArgumentList $qargs -WorkingDirectory $logDir -PassThru -WindowStyle Hidden
Write-Host "qemu pid=$($p.Id) workdir=$logDir"

function Wait-Port($port, $ms) {
  $t0 = [DateTime]::UtcNow
  while (([DateTime]::UtcNow - $t0).TotalMilliseconds -lt $ms) {
    try { $c = New-Object Net.Sockets.TcpClient("127.0.0.1", $port); $c.Close(); return $true }
    catch { Start-Sleep -Milliseconds 100 }
  }
  return $false
}

function HMP($cmd) {
  # HMP monitor protocol: LF-terminated ASCII lines; close after each command.
  try {
    $c = New-Object Net.Sockets.TcpClient("127.0.0.1", $MonitorPort)
    $s = $c.GetStream()
    $bytes = [Text.Encoding]::ASCII.GetBytes($cmd + "`n")
    $s.Write($bytes, 0, $bytes.Length); $s.Flush()
    Start-Sleep -Milliseconds 300
    $buf = New-Object byte[] 8192
    $n = $s.Read($buf, 0, 8192)
    $c.Close()
    return [Text.Encoding]::ASCII.GetString($buf, 0, $n)
  } catch { return "" }
}

if (-not (Wait-Port $MonitorPort 15000)) { Write-Host "FAIL: monitor port never opened"; exit 1 }
Write-Host "monitor port up ($MonitorPort)"
$t0 = [DateTime]::UtcNow
$keyOut = @()

function Send-One-Key($k, $idx) {
  $ts = ([DateTime]::UtcNow - $t0).TotalSeconds
  if ($p.HasExited) { Write-Host "qemu exited early at t=$([math]::Round($ts,1))s"; return $false }
  $resp = HMP "sendkey $k"
  $line = "t=$([math]::Round($ts,1))s sendkey $k -> $($resp -replace "`n", ' | ')"
  $script:keyOut += $line
  Write-Host $line
  return $true
}

if ($Boot -eq "ours") {
  # ---- boot=ours: fixed banner evidence sequence (w7-32 shape) --------------
  Write-Host "waiting ${BannerSec}s for the banner (boot -> lcd init ~10-12s)..."
  Start-Sleep -Seconds $BannerSec
  $r1 = HMP "screendump $shot1 -f png"
  Write-Host "screendump#1 -> $($r1 -replace "`n", ' | ')"
  Start-Sleep -Seconds 3
  $r2 = HMP "screendump $shot2 -f png"
  Write-Host "screendump#2 -> $($r2 -replace "`n", ' | ')"
  Start-Sleep -Seconds 1
  foreach ($k in $Keys) { if (-not (Send-One-Key $k 0)) { break } ; Start-Sleep -Seconds $KeyDelay }
  $r3 = HMP "screendump $shot3 -f png"
  Write-Host "screendump#3 -> $($r3 -replace "`n", ' | ')"
} else {
  # ---- warm|stock: periodic key injections until the time budget ------------
  Write-Host "warming up 5s, then keys every ${KeyDelay}s until t=$(($TimeSec - 8))s..."
  Start-Sleep -Seconds 5
  $i = 0
  while ($Keys.Count -gt 0 -and ([DateTime]::UtcNow - $t0).TotalSeconds -lt ($TimeSec - 8)) {
    if (-not (Send-One-Key $Keys[$i % $Keys.Count] $i)) { break }
    $i++
    Start-Sleep -Seconds $KeyDelay
  }
  $r = HMP "screendump $shotF -f png"
  Write-Host "final screendump -> $($r -replace "`n", ' | ')"
}

# ---- final state + clean quit ------------------------------------------------
$tEnd = ([DateTime]::UtcNow - $t0).TotalSeconds
$remaining = $TimeSec - $tEnd
if ($remaining -gt 0) {
  Write-Host "cooldown $([math]::Round($remaining,1))s (total budget ${TimeSec}s)"
  Start-Sleep -Seconds $remaining
}
$regsOut = @("=== info registers ===")
$regsOut += (HMP "info registers")
$regsOut += "=== xp 0x8100300c (sys ms counter) ==="
$regsOut += (HMP "xp /4wx 0x8100300c")
$regsOut += "=== xp 0x20d00024 (lcdc img base) ==="
$regsOut += (HMP "xp /4wx 0x20d00024")
$regsOut | Set-Content -LiteralPath $regsFile -Encoding ASCII

HMP "quit" | Out-Null
Start-Sleep -Seconds 1
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
$elapsed = ([DateTime]::UtcNow - $t0).TotalSeconds
$keyOut | Set-Content -LiteralPath $keysFile -Encoding ASCII
Write-Host "qemu exited; run elapsed $([math]::Round($elapsed,1))s; log -> $logFull"

# ---- expected-evidence print --------------------------------------------------
Write-Host ""
Write-Host "=== EXPECTED EVIDENCE (check these) ==="
Write-Host "  raw log:   $logFull"
Write-Host "  keys:      $keysFile"
if ($Boot -eq "ours") {
  Write-Host "  screens:   $(Join-Path $logDir $shot1), $(Join-Path $logDir $shot2), $(Join-Path $logDir $shot3)  (non-black banner, text bands)"
  Write-Host "  log:       sc6530_lcdc_refresh lines (fb=0x340054c8), ~1k/s 'Taking exception 5 [IRQ]' ticks, 20 keypad DOWN/UP edges, ZERO non-IRQ exceptions"
} else {
  Write-Host "  screen:    $(Join-Path $logDir $shotF)"
  Write-Host "  log:       sc6530_ana_read/write codec lines (0x820010xx/0x820011xx/0x820018xx family, guest PCs), sc6530_aux_* APB/PA-ladder + UNMODELED lines, sc6530_dsp_cmd lines"
  Write-Host "  extract:   powershell -NoProfile -ExecutionPolicy Bypass -File tools\qemu-b310e\scripts\extract-audio-trace.ps1 -Log $logFull"
}
Write-Host "  regs:      $regsFile (info registers + sys-ms + LCDC img base)"
exit 0
