#!/usr/bin/env bash
# read-spy.sh - bash port of tools/stock-spy/read-spy.ps1
#
# Decode the stock-spy ring dump (spy-rings.bin) into a readable register
# trace.
#
# Usage:  tools/stock-spy/read-spy.sh [spy-rings.bin]
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
#
# Output: same path as the input with a .txt extension.

set -euo pipefail

IN="${1:-spy-rings.bin}"

# normalize a Windows-style drive path (C:\... or C:/...) to the MSYS
# /c/... form that native tools (dd/od/wc) can open
if [[ "$IN" =~ ^([A-Za-z]):([\\/])(.*)$ ]]; then
    IN="/${BASH_REMATCH[1],,}/${BASH_REMATCH[3]//\\//}"
fi

if [[ "$IN" == *.* ]]; then
    OUT="${IN%.*}.txt"
else
    OUT="$IN.txt"
fi

if [ ! -f "$IN" ]; then
    echo "dump not found: $IN" >&2
    exit 1
fi
len=$(wc -c < "$IN")
if [ "$len" -lt $((0x2000)) ]; then
    echo "dump too small ($len B); expected >= 0x2000" >&2
    exit 1
fi

# read a little-endian u32 at byte offset $1 (od -tu1 + manual LE assembly;
# endian-independent, no dependence on the host byte order)
u32() {
    local off=$(( $1 )) b0 b1 b2 b3
    read -r b0 b1 b2 b3 <<< "$(dd if="$IN" bs=1 skip="$off" count=4 2>/dev/null | od -An -tu1 -v)"
    echo $((b0 | b1 << 8 | b2 << 16 | b3 << 24))
}

buf=""
W() { buf+="$1"$'\r\n'; }

# ---- header ----
magic=$(u32 0x1F18)
rebooted=$(u32 0x1F1C)
tick=$(u32 0x1F00)
r1idx=$(u32 0x1F04)
r2idx=$(u32 0x1F08)
tbase=$(u32 0x1F0C)
kidle=$(u32 0x1F10)
khold=$(u32 0x1F14)

if [ "$magic" -eq $((0x59505331)) ]; then magic_ok='(OK)'; else magic_ok='(BAD - not a spy dump!)'; fi

W "stock-spy dump: $IN"
W "$(printf '  magic    = 0x%08X %s' "$magic" "$magic_ok")"
W "  rebooted = $rebooted (1 = the spy fired the watchdog)"
W "  tick     = $tick ADI-write hook invocations"
W "  ring1    = $r1idx/512 ADI writes logged"
W "  ring2    = $r2idx/128 snapshots logged"
W "$(printf '  keypad idle = 0x%08X, hold_start = 0x%08X' "$kidle" "$khold")"
if [ "$r1idx" -eq 0 ] && [ "$tick" -gt 0 ]; then
    W '  WARNING: ring1 never filled - the patch may not be live (stock OS fell back to NOR?)'
fi
W ""

# ---- register name table ----
declare -A names
names[$((0x820010e0))]='ANA_ARM_CLK_EN0 (bits 1-8)'
names[$((0x820010e4))]='ANA_SOFT_RST0'
names[$((0x82001040))]='ANA_MODULE'
names[$((0x82001014))]='codec LDO VOL'
names[$((0x8200116c))]='IF2 b10'
names[$((0x82001180))]='IF0'
names[$((0x820011a0))]='IF1'
names[$((0x820011c0))]='IF1+0x20 (keepalive 0x19)'
names[$((0x82001164))]='LDO bank'
names[$((0x82001220))]='ANA_LED_CTRL (keylight)'
names[$((0x82001224))]='keylight level'
names[$((0x82001240))]='vibrator'
names[$((0x82001244))]='vibrator intensity'
names[$((0x82001154))]='vibrator pwr gate'
names[$((0x82001288))]='DAC1 mux'
names[$((0x82001290))]='PA mute'
names[$((0x820012a0))]='GRP0 (DAC path)'
names[$((0x820012a4))]='GRP1 (DAC path)'
names[$((0x820012c0))]='codec power'
names[$((0x820012f4))]='dacr (DAC cores)'
names[$((0x82001304))]='dcr1 (routing)'
names[$((0x82001314))]='dcgr1 HP gain'
names[$((0x82001318))]='dcgr2 EAR gain'
names[$((0x8200131c))]='dcgr3 SPK gain'
names[$((0x8200132c))]='ccr (DAC clocks)'
names[$((0x82001450))]='PA EN'
names[$((0x82001454))]='PA EN2'
names[$((0x82001440))]='PA type'
names[$((0x82001444))]='PA type2'
names[$((0x82001480))]='WDG LOAD'
names[$((0x820016d4))]='ADC start'
names[$((0x82001a44))]='codec keepalive'
names[$((0x82000d00))]='(non-audio)'

regname() {
    local a=$1
    local n="${names[$a]:-}"
    if [ -n "$n" ]; then
        echo "$n"
    else
        printf 'ANA 0x%06X' "$a"
    fi
}

vbcbit() {
    local v=$1 s=""
    if (( v & 0x8000 )); then s+=' VBENABLE'; fi
    if (( v & 0x6000 )); then s+=' DMA-EN(0x6000)'; fi
    if (( v & 0x400 )); then s+=' VBRAMSW_EN'; fi
    if (( v & 0x200 )); then s+=' VBRAMSW_NUM'; fi
    echo "$s"
}

# ---- ring1: every ADI write ----
W '==== ring1: ADI writes (addr = value) in order ===='
n1=$r1idx
if [ "$n1" -gt 512 ]; then n1=512; fi
for (( i = 0; i < n1; i++ )); do
    off=$((0x400 + i * 8))
    a=$(u32 "$off")
    v=$(u32 $((off + 4)))
    W "$(printf '  [%3d] 0x%08X = 0x%08X   %s' "$i" "$a" "$v" "$(regname "$a")")"
done
W ""

# ---- ring2: the GLB snapshots (ownership/power/DSP-IRQ) ----
W '==== ring2: GLB audio-register snapshots (audio-ANA writes only) ===='
W '  [idx] 0x8b0001c4 ownership | 0x8b000060 power | 0x8b0000a0 power2 | 0x8b000160 DSP-IRQ'
n2=$r2idx
if [ "$n2" -gt 128 ]; then n2=128; fi
for (( i = 0; i < n2; i++ )); do
    base=$((0x1400 + i * 16))
    c4=$(u32 "$base")
    p60=$(u32 $((base + 4)))
    pa0=$(u32 $((base + 8)))
    r160=$(u32 $((base + 12)))
    if [ $((c4 & 0x604)) -eq $((0x604)) ]; then
        own='ARM-owns(0x604)'
    elif [ $((c4 & 0x600)) -eq $((0x600)) ]; then
        own='ARM codec(0x600)'
    elif [ $((c4 & 0x4)) -eq $((0x4)) ]; then
        own='ARM VB b2'
    elif [ "$c4" -eq 0 ]; then
        own='DSP?'
    else
        own=''
    fi
    W "$(printf '  [%3d] 0x%08X %-16s | 0x%08X | 0x%08X | 0x%08X' "$i" "$c4" "$own" "$p60" "$pa0" "$r160")"
done

# ---- ring3: the UART debug text (the putc hook) ----
r3idx=$(u32 0x1F20)
if [ "$r3idx" -gt 255 ]; then r3idx=255; fi
if [ "$r3idx" -gt 0 ]; then
    W ""
    W "==== ring3: captured UART debug text (the stock OS') ===="
    text=""
    r3bytes=$(dd if="$IN" bs=1 skip=$((0x1D00)) count="$r3idx" 2>/dev/null | od -An -tu1 -v)
    for ch in $r3bytes; do
        if [ "$ch" -ge 32 ] && [ "$ch" -le 126 ]; then
            printf -v c "\\$(printf '%03o' "$ch")"
            text+="$c"
        elif [ "$ch" -eq 10 ] || [ "$ch" -eq 13 ]; then
            printf -v c "\\$(printf '%03o' "$ch")"
            text+="$c"
        else
            text+='?'
        fi
    done
    W "  \"$text\""
    W '  (the AST_BLUESCREEN / assert text names the hang location)'
fi

W ""
W 'Decode hints:'
W '  - the LAST 0x820012xx/0x820014xx writes before any 0x82003018-ish activity = the playback-time codec/PA config'
W '  - ownership 0x604 = ARM owns codec+VBC; 0x600 = ARM codec; b2 alone = ARM VBC; 0 = DSP owns'
W '  - the keepalive cycle (0x820011c0=0x19, 0x820011a0, 0x82001180) repeats every few seconds'

printf '%s' "$buf" > "$OUT"
echo "==> decoded $n1 ring1 + $n2 ring2 entries -> $OUT"
