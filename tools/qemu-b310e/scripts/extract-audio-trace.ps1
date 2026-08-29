# extract-audio-trace.ps1 - turn a -D trace log into the audio-observatory CSV.
# Automates the todo-28 hand-built extraction (logs/w6/extract-w6-sound-csv.ps1)
# into a generic, reusable tool. Input: any QEMU `-D` log captured with
# --trace "sc6530_ana_*" --trace "sc6530_aux_*" --trace "sc6530_dsp_cmd"
# (see run-b310e.ps1). Output: CSV with the shape (ts, addr, rw, value, pc):
#
#   ts    ordinal of the line within the extracted window (no timestamps in
#         the QEMU log backend)
#   addr  guest address (for ADI mailbox READ-DATA accesses the RESOLVED ANA
#         register, e.g. 0x820010xx - never the 0x8200001c MMIO offset;
#         for sc6530_dsp_cmd rows: "dsp:id=<N>")
#   rw    "read" | "write" | "w" (aux single-char form) | "cmd" (dsp rows)
#   value value read/written (dsp rows: the packed arg)
#   pc    guest PC that performed the access
#
# Handled line shapes (all qemu_log/trace-backend formats from the Wave 3-6
# devices, see machine/hw/misc/trace-events):
#   sc6530_ana_read ANA read  addr=0x.. val=0x.. pc=0x..         -> rw=read
#   sc6530_ana_write ANA write addr=0x.. val=0x.. pc=0x..        -> rw=write
#   sc6530_aux_read APB read  addr=0x.. val=0x.. pc=0x..         -> rw=read
#   sc6530_aux_write APB write addr=0x.. val=0x.. pc=0x..        -> rw=w
#   sc6530_aux: <tag> write addr=0x.. val=0x.. pc=0x..           -> rw=w
#     (AHB/GPIO/pinmux/SMC writes + the store+echo spellings)
#   sc6530_aux: <tag> [wr] addr=0x.. val=0x.. [capped=0x..] pc=0x..
#     (scirpos w, UNMODELED w, bootready r, ...)                -> rw=<char>
#   sc6530_dsp_cmd DSP cmd id=<N> arg=0x.. pc=0x..              -> dsp:id=<N>
#
# A round-trip check counts the audio-family raw lines in the window and
# compares with the CSV row count (MATCH/MISMATCH); exit code reflects it.
#
# Usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\qemu-b310e\scripts\extract-audio-trace.ps1 `
#     -Log tools\qemu-b310e\logs\w6\stock-sound.log -Csv out.csv
#   # window (1-based inclusive lines, default = whole file):
#   ... -Log stock-sound.log -FromLine 180 -ToLine 410          # reproduces
#       the todo-28 stock-sound.csv (231 rows)
param(
  [Parameter(Mandatory = $true)]
  [string]$Log,
  [string]$Csv,                 # default: <log>.csv (extension swap)
  [int]$FromLine = 0,           # 1-based inclusive; 0 = whole file
  [int]$ToLine = 0
)
$ErrorActionPreference = "Stop"

$logFull = [IO.Path]::GetFullPath($Log)
if (-not (Test-Path -LiteralPath $logFull)) { Write-Host "NO LOG: $logFull"; exit 1 }
if (-not $Csv) { $Csv = [IO.Path]::ChangeExtension($logFull, ".csv") }
$csvFull = [IO.Path]::GetFullPath($Csv)

$all = [System.IO.File]::ReadAllLines($logFull)
$total = $all.Count
if ($FromLine -le 0) { $FromLine = 1 }
if ($ToLine -le 0 -or $ToLine -gt $total) { $ToLine = $total }
if ($FromLine -gt $ToLine) { Write-Host "FAIL: FromLine $FromLine > ToLine $ToLine"; exit 1 }

$anaRe  = '^sc6530_ana_(read|write) ANA (?:read|write)\s+addr=(0x[0-9a-f]+) val=(0x[0-9a-f]+) pc=(0x[0-9a-f]+)'
$dspRe  = '^sc6530_dsp_cmd DSP cmd id=(\d+) arg=(0x[0-9a-f]+) pc=(0x[0-9a-f]+)'

$rows = [System.Collections.Generic.List[string]]::new()
$parsed = 0
$skipped = 0
for ($ln = $FromLine; $ln -le $ToLine; $ln++) {
  $l = $all[$ln - 1]   # 0-based
  $row = $null
  if ($l -match $anaRe) {
    $row = '{0},{1},{2},{3},{4}' -f ($rows.Count + 1), $Matches[2], $Matches[1], $Matches[3], $Matches[4]
  } elseif ($l -match $dspRe) {
    $row = '{0},dsp:id={1},cmd,{2},{3}' -f ($rows.Count + 1), $Matches[1], $Matches[2], $Matches[3]
  } elseif ($l -match '^sc6530_aux') {
    $rw = 'r'
    if ($l -match '\bwrite\s+addr=') { $rw = 'w' }
    elseif ($l -match '\s([wr])\s+addr=') { $rw = $Matches[1] }
    if ($l -match 'addr=(0x[0-9a-f]+) val=(0x[0-9a-f]+).*? pc=(0x[0-9a-f]+)') {
      $row = '{0},{1},{2},{3},{4}' -f ($rows.Count + 1), $Matches[1], $rw, $Matches[2], $Matches[3]
    } else { $skipped++ }
  }
  if ($row) { $rows.Add($row); $parsed++ }
}

$rows | Set-Content -LiteralPath $csvFull -Encoding ASCII
Write-Host "extracted $parsed rows (window lines $FromLine..$ToLine, $total total) -> $csvFull"

# ---- round-trip verification: count audio-family lines in the raw window ----
$raw = 0
for ($ln = $FromLine; $ln -le $ToLine; $ln++) {
  if ($all[$ln - 1] -match '^sc6530_(ana_(read|write)|aux|dsp_cmd)') { $raw++ }
}
$status = if ($parsed -eq $raw) { "MATCH" } else { "MISMATCH" }
Write-Host "round-trip: csv=$parsed raw=$raw $status (skipped unmatched audio-family lines: $skipped)"
if ($status -ne "MATCH") { exit 1 }
exit 0
