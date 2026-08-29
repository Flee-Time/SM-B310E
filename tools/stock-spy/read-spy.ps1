# read-spy.ps1 — decode the stock-spy ring dump (spy-rings.bin) into a
# readable register trace.
#
# Usage:  powershell -ExecutionPolicy Bypass -File tools\stock-spy\read-spy.ps1
#             [-In spy-rings.bin] [-Out spy-trace.txt]
#
# The dump is the raw 0x2000-byte payload recovered with:
#   .\spd_dump.exe fdl nor_fdl1.bin 0x40004000 read_mem 0x352F0800 0x2000 spy-rings.bin
#
# Payload layout (VA 0x042F0000 = dump offset 0):
#   0x0000  hook code (not decoded)
#   0x0400  ring1: 512 x {u32 addr, u32 value}  (every ADI write, in order)
#   0x1400  ring2: 128 x {u32 0x8b0001c4, 0x8b000060, 0x8b0000a0, 0x8b000160}
#           (snapshots, taken only for writes in the audio ANA block)
#   0x1F00  header: tick, ring1_idx, ring2_idx, timer_base, keypad_idle,
#           hold_start, magic, rebooted

param(
  [string]$In  = (Join-Path (Get-Location) 'spy-rings.bin'),
  [string]$Out = ''
)

$ErrorActionPreference = 'Stop'
if (-not $Out) { $Out = [IO.Path]::ChangeExtension($In, '.txt') }
if (-not (Test-Path -LiteralPath $In)) { throw "dump not found: $In" }
$b = [System.IO.File]::ReadAllBytes($In)
if ($b.Length -lt 0x2000) { throw "dump too small ($($b.Length) B); expected >= 0x2000" }

function U32([int]$o) { return $b[$o] + ($b[$o+1] * 256) + ($b[$o+2] * 65536) + ($b[$o+3] * 16777216) }
function U32B([int]$o) { return [int64]$b[$o] + ([int64]$b[$o+1] * 256) + ([int64]$b[$o+2] * 65536) + ([int64]$b[$o+3] * 16777216) }

$o = [System.Text.StringBuilder]::new()
function W($s) { [void]$o.AppendLine($s) }

# ---- header ----
$magic    = U32 0x1F18
$rebooted = U32 0x1F1C
$tick     = U32 0x1F00
$r1idx    = U32 0x1F04
$r2idx    = U32 0x1F08
$tbase    = U32 0x1F0C
$kidle    = U32 0x1F10
$khold    = U32 0x1F14

W ('stock-spy dump: ' + $In)
W ('  magic    = 0x{0:X8} {1}' -f ([uint32]$magic), $(if ($magic -eq 0x59505331) { '(OK)' } else { '(BAD - not a spy dump!)' }))
W ('  rebooted = {0} (1 = the spy fired the watchdog)' -f $rebooted)
W ('  tick     = {0} ADI-write hook invocations' -f $tick)
W ('  ring1    = {0}/512 ADI writes logged' -f $r1idx)
W ('  ring2    = {0}/128 snapshots logged' -f $r2idx)
W ('  keypad idle = 0x{0:X8}, hold_start = 0x{1:X8}' -f ([uint32]$kidle), ([uint32]$khold))
if ($r1idx -eq 0 -and $tick -gt 0) { W ('  WARNING: ring1 never filled - the patch may not be live (stock OS fell back to NOR?)') }
W ''

# ---- register name table ----
$names = @{
  0x820010e0 = 'ANA_ARM_CLK_EN0 (bits 1-8)'
  0x820010e4 = 'ANA_SOFT_RST0'
  0x82001040 = 'ANA_MODULE'
  0x82001014 = 'codec LDO VOL'
  0x8200116c = 'IF2 b10'
  0x82001180 = 'IF0'
  0x820011a0 = 'IF1'
  0x820011c0 = 'IF1+0x20 (keepalive 0x19)'
  0x82001164 = 'LDO bank'
  0x82001220 = 'ANA_LED_CTRL (keylight)'
  0x82001224 = 'keylight level'
  0x82001240 = 'vibrator'
  0x82001244 = 'vibrator intensity'
  0x82001154 = 'vibrator pwr gate'
  0x82001288 = 'DAC1 mux'
  0x82001290 = 'PA mute'
  0x820012a0 = 'GRP0 (DAC path)'
  0x820012a4 = 'GRP1 (DAC path)'
  0x820012c0 = 'codec power'
  0x820012f4 = 'dacr (DAC cores)'
  0x82001304 = 'dcr1 (routing)'
  0x82001314 = 'dcgr1 HP gain'
  0x82001318 = 'dcgr2 EAR gain'
  0x8200131c = 'dcgr3 SPK gain'
  0x8200132c = 'ccr (DAC clocks)'
  0x82001450 = 'PA EN'
  0x82001454 = 'PA EN2'
  0x82001440 = 'PA type'
  0x82001444 = 'PA type2'
  0x82001480 = 'WDG LOAD'
  0x820016d4 = 'ADC start'
  0x82001a44 = 'codec keepalive'
  0x82000d00 = '(non-audio)'
}
function RegName([uint32]$a) {
  $k = if ($a -gt 0x7FFFFFFF) { $a - [long]0x100000000 } else { $a }
  if ($names.ContainsKey([int]$k)) { return $names[[int]$k] }
  return ('ANA 0x{0:X6}' -f $a)
}
function VbcBit([uint32]$v) {
  $s = ''
  if ($v -band 0x8000) { $s += ' VBENABLE' }
  if ($v -band 0x6000) { $s += ' DMA-EN(0x6000)' }
  if ($v -band 0x400)  { $s += ' VBRAMSW_EN' }
  if ($v -band 0x200)  { $s += ' VBRAMSW_NUM' }
  return $s
}

# ---- ring1: every ADI write ----
W '==== ring1: ADI writes (addr = value) in order ===='
$n1 = [Math]::Min($r1idx, 512)
for ($i = 0; $i -lt $n1; $i++) {
  $a = U32B (0x400 + $i * 8)
  $v = U32B (0x404 + $i * 8)
  W ('  [{0,3}] 0x{1:X8} = 0x{2:X8}   {3}' -f $i, ([uint32]$a), ([uint32]$v), (RegName $a))
}
W ''

# ---- ring2: the GLB snapshots (ownership/power/DSP-IRQ) ----
W '==== ring2: GLB audio-register snapshots (audio-ANA writes only) ===='
W '  [idx] 0x8b0001c4 ownership | 0x8b000060 power | 0x8b0000a0 power2 | 0x8b000160 DSP-IRQ'
$n2 = [Math]::Min($r2idx, 128)
for ($i = 0; $i -lt $n2; $i++) {
  $base = 0x1400 + $i * 16
  $c4 = U32B $base
  $p60 = U32B ($base + 4)
  $pa0 = U32B ($base + 8)
  $r160 = U32B ($base + 12)
  $own = ''
  if (($c4 -band 0x604) -eq 0x604) { $own = 'ARM-owns(0x604)' }
  elseif (($c4 -band 0x600) -eq 0x600) { $own = 'ARM codec(0x600)' }
  elseif (($c4 -band 0x4) -eq 0x4) { $own = 'ARM VB b2' }
  elseif ($c4 -eq 0) { $own = 'DSP?' }
  W ('  [{0,3}] 0x{1:X8} {2,-16} | 0x{3:X8} | 0x{4:X8} | 0x{5:X8}' -f $i, ([uint32]$c4), $own, ([uint32]$p60), ([uint32]$pa0), ([uint32]$r160))
}

# ---- ring3: the UART debug text (the putc hook) ----
$r3idx = [Math]::Min((U32 0x1F20), 255)
if ($r3idx -gt 0) {
  W ''
  W '==== ring3: captured UART debug text (the stock OS'') ===='
  $sb = New-Object System.Text.StringBuilder
  for ($i = 0; $i -lt $r3idx; $i++) {
    $ch = $b[0x1D00 + $i]
    if ($ch -ge 0x20 -and $ch -le 0x7e) { [void]$sb.Append([char]$ch) }
    elseif ($ch -eq 10 -or $ch -eq 13) { [void]$sb.Append([char]$ch) }
    else { [void]$sb.Append('?') }
  }
  W ('  "' + $sb.ToString() + '"')
  W '  (the AST_BLUESCREEN / assert text names the hang location)'
}

W ''
W 'Decode hints:'
W '  - the LAST 0x820012xx/0x820014xx writes before any 0x82003018-ish activity = the playback-time codec/PA config'
W '  - ownership 0x604 = ARM owns codec+VBC; 0x600 = ARM codec; b2 alone = ARM VBC; 0 = DSP owns'
W '  - the keepalive cycle (0x820011c0=0x19, 0x820011a0, 0x82001180) repeats every few seconds'
[System.IO.File]::WriteAllText($Out, $o.ToString())
Write-Host "==> decoded $n1 ring1 + $n2 ring2 entries -> $Out"
