#!/usr/bin/env bash
# extract-audio-trace.sh - turn a -D trace log into the audio-observatory CSV.
# Bash port of extract-audio-trace.ps1 (todo-28 tool). Input: any QEMU `-D`
# log captured with --trace "sc6530_ana_*" --trace "sc6530_aux_*"
# --trace "sc6530_dsp_cmd". Output: CSV with the shape (ts, addr, rw, value,
# pc). A round-trip check counts the audio-family raw lines in the window and
# compares with the CSV row count (MATCH/MISMATCH); exit code reflects it.
#
# Usage:
#   extract-audio-trace.sh -Log <file> [-Csv out.csv] [-FromLine N] [-ToLine N]
#   # window (1-based inclusive lines, default = whole file)
set -euo pipefail

Log=""
Csv=""
FromLine=0
ToLine=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    -Log)      Log="$2";     shift 2 ;;
    -Csv)      Csv="$2";     shift 2 ;;
    -FromLine) FromLine="$2"; shift 2 ;;
    -ToLine)   ToLine="$2";  shift 2 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done

if [[ -z "$Log" ]]; then
  echo "usage: extract-audio-trace.sh -Log <file> [-Csv out.csv] [-FromLine N] [-ToLine N]" >&2
  exit 2
fi

logFull="$Log"
if [[ ! -f "$logFull" ]]; then
  echo "NO LOG: $logFull"
  exit 1
fi

if [[ -z "$Csv" ]]; then
  Csv="${logFull%.*}.csv"
fi
csvFull="$Csv"

total="$(grep -c '' "$logFull" || true)"
from="$FromLine"
to="$ToLine"
if [[ "$from" -le 0 ]]; then from=1; fi
if [[ "$to" -le 0 || "$to" -gt "$total" ]]; then to="$total"; fi
if [[ "$from" -gt "$to" ]]; then
  echo "FAIL: FromLine $from > ToLine $to"
  exit 1
fi

tmp_csv="$(mktemp)"
tmp_meta="$(mktemp)"
trap 'rm -f "$tmp_csv" "$tmp_meta"' EXIT

sed -n "${from},${to}p" "$logFull" | awk '
BEGIN {
  ordinal = 0; parsed = 0; skipped = 0
}
{
  line = $0
  if (line ~ /^sc6530_ana_(read|write) ANA (read|write)[ \t]+addr=(0x[0-9a-f]+) val=(0x[0-9a-f]+) pc=(0x[0-9a-f]+)/) {
    rw = $1; sub(/^sc6530_ana_/, "", rw)
    addr = $4; sub(/^addr=/, "", addr)
    val  = $5; sub(/^val=/,  "", val)
    pc   = $6; sub(/^pc=/,   "", pc)
    printf "%d,%s,%s,%s,%s\n", ++ordinal, addr, rw, val, pc
    parsed++
    next
  }
  if (line ~ /^sc6530_dsp_cmd DSP cmd id=[0-9]+ arg=0x[0-9a-f]+ pc=0x[0-9a-f]+/) {
    id = $4;  sub(/^id=/,  "", id)
    arg = $5; sub(/^arg=/, "", arg)
    pc  = $6; sub(/^pc=/,  "", pc)
    printf "%d,dsp:id=%s,cmd,%s,%s\n", ++ordinal, id, arg, pc
    parsed++
    next
  }
  if (line ~ /^sc6530_aux/) {
    rw = "r"
    if (line ~ /\<write[ \t]+addr=/) rw = "w"
    else if (match(line, /[ \t]([wr])[ \t]+addr=/, m2)) rw = m2[1]
    if (match(line, /addr=(0x[0-9a-f]+) val=(0x[0-9a-f]+).*[ \t]pc=(0x[0-9a-f]+)/, m)) {
      printf "%d,%s,%s,%s,%s\n", ++ordinal, m[1], rw, m[2], m[3]
      parsed++
    } else {
      skipped++
    }
  }
}
END {
  printf "%d %d", parsed, skipped > "/dev/stderr"
}
' > "$tmp_csv" 2> "$tmp_meta"

parsed="$(awk '{ print $1 }' "$tmp_meta" 2>/dev/null || echo 0)"
skipped="$(awk '{ print $2 }' "$tmp_meta" 2>/dev/null || echo 0)"

raw="$(sed -n "${from},${to}p" "$logFull" | grep -cE '^sc6530_(ana_(read|write)|aux|dsp_cmd)' || true)"

mv "$tmp_csv" "$csvFull"
echo "extracted $parsed rows (window lines $from..$to, $total total) -> $csvFull"

if [[ "$parsed" -eq "$raw" ]]; then status="MATCH"; else status="MISMATCH"; fi
echo "round-trip: csv=$parsed raw=$raw $status (skipped unmatched audio-family lines: $skipped)"
if [[ "$status" == "MISMATCH" ]]; then
  exit 1
fi
exit 0
