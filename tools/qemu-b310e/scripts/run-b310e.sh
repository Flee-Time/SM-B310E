#!/usr/bin/env bash
# run-b310e.sh - self-contained B310E QEMU machine run script (todo 34).
# Bash port of run-b310e.ps1. Same params and behavior: fixed sleeps, no log
# polling; HMP monitor via /dev/tcp; evidence in the -D log's directory.
#
#   qemu-system-arm -M b310e,boot-mode=<Boot> ${display_arg} -serial none
#     -d int -D <log> [--trace <pattern> ...]
#     -monitor telnet:127.0.0.1:<MonitorPort>,server,nowait
#     -drive file=<Nor>,format=raw,if=none,id=nor
#     [-drive file=<Os>,format=raw,if=none,id=os]        # boot=ours only
#
# KEYBOARD MAP: ret->CENTER, kp_enter->DIAL, 0-9->0-9, minus/*->STAR, slash->HASH, f1->LSOFT, f2->RSOFT, esc/end->END, arrows->D-pad
#
# Usage examples:
#   run-b310e.sh -Boot warm -TimeSec 90 -D tools/qemu-b310e/logs/w7/final-sweep.log
#   run-b310e.sh -Boot ours -D tools/qemu-b310e/logs/w7/ours-sweep.log
#   run-b310e.sh -Boot stock -TimeSec 30 -Keys "" -D tools/qemu-b310e/logs/w7/stock-sweep.log
set -euo pipefail

Boot="warm"
Nor=""
Os=""
Trace=()
D=""
MonitorPort=4550
TimeSec=60
KeyDelay=5
BannerSec=25
Keys=()
Qemu=""
Display="gtk"

while [[ $# -gt 0 ]]; do
  case "$1" in
    -Boot)        Boot="$2";      shift 2 ;;
    -Nor)         Nor="$2";       shift 2 ;;
    -Os)          Os="$2";        shift 2 ;;
    -Trace)       Trace+=("$2");  shift 2 ;;
    -D)           D="$2";         shift 2 ;;
    -MonitorPort) MonitorPort="$2"; shift 2 ;;
    -TimeSec)     TimeSec="$2";   shift 2 ;;
    -KeyDelay)    KeyDelay="$2";  shift 2 ;;
    -BannerSec)   BannerSec="$2"; shift 2 ;;
    -Keys)        Keys+=("$2");   shift 2 ;;
    -Qemu)        Qemu="$2";      shift 2 ;;
    -Display)     Display="$2";   shift 2 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done

# ---- resolve paths (everything derives from the script location) ----------
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$SCRIPT_DIR/../../.." && pwd)"
if [[ -z "$Nor" ]]; then Nor="$repo/tools/spd_dump/full-backup.bin"; fi
if [[ -z "$Os" ]]; then Os="$repo/tools/qemu-b310e/logs/os-sdboot-era.bin"; fi
if [[ -z "$Qemu" ]]; then Qemu="$HOME/qemu-b310e/qemu-src/build/qemu-system-arm"; fi

# ---- param validation ------------------------------------------------------
case "$Boot" in
  warm|stock|ours) ;;
  *) echo "FAIL: unknown boot mode '$Boot' (expected warm|stock|ours)"; exit 2 ;;
esac
if [[ -z "$D" ]]; then
  echo "FAIL: -D <logfile> is required (the -D trace/log evidence file)" >&2
  exit 2
fi

# ---- defaults per boot mode ------------------------------------------------
if [[ ${#Trace[@]} -eq 0 ]]; then
  if [[ "$Boot" == "ours" ]]; then
    Trace=("sc6530_lcdc_refresh" "sc6530_ana_*" "sc6530_aux_*")
  else
    Trace=("sc6530_ana_*" "sc6530_aux_*" "sc6530_dsp_cmd")
  fi
fi
if [[ ${#Keys[@]} -eq 0 ]]; then
  if [[ "$Boot" == "ours" ]]; then
    Keys=('ret' 'up' 'down' 'left' 'right' '1' '5' 'f1' 'kp_enter' 'esc')
  else
    Keys=('ret' '1' '2' '3' '4' 'f1' 'f2' 'kp_enter' 'up' 'down' 'left' 'right' '5' '6' '7')
  fi
fi
# Normalize: "-Keys ret,up" arrives as ONE literal string - split on commas so
# both forms work (the ps1 -File invocation quirk).
norm_keys=()
for k in "${Keys[@]}"; do
  IFS=',' read -r -a parts <<< "$k"
  for p in "${parts[@]}"; do
    if [[ -n "$p" ]]; then norm_keys+=("$p"); fi
  done
done
Keys=("${norm_keys[@]}")
for t in "${Trace[@]}"; do
  if [[ "$t" == *,* ]]; then
    echo "WARN: trace pattern '$t' contains a comma - qemu would parse it as options (W4-20b lesson); one --trace per pattern is the rule."
  fi
done

# ---- sanity checks -----------------------------------------------------------
qemu_bin=""
if [[ -f "$Qemu" ]]; then
  qemu_bin="$Qemu"
elif [[ -f "$Qemu.exe" ]]; then
  qemu_bin="$Qemu.exe"
elif [[ "${Qemu%.exe}" != "$Qemu" && -f "${Qemu%.exe}" ]]; then
  qemu_bin="${Qemu%.exe}"
fi
if [[ -z "$qemu_bin" ]]; then
  echo "FAIL: qemu binary not found at $Qemu"
  echo "      run tools/qemu-b310e/scripts/build-qemu.sh first (or pass -Qemu)"
  exit 1
fi
Qemu="$qemu_bin"
if [[ ! -f "$Nor" ]]; then
  echo "FAIL: NOR image not found at $Nor"
  exit 1
fi
if [[ "$Boot" == "ours" && ! -f "$Os" ]]; then
  echo "FAIL: os image not found at $Os (boot=ours needs the committed sdboot-era os.bin, or pass -Os)"
  exit 1
fi

# ---- log dir setup -----------------------------------------------------------
case "$D" in
  /*) logFull="$D" ;;
  *)  logFull="$(pwd)/$D" ;;
esac
logDir="$(dirname "$logFull")"
if [[ ! -d "$logDir" ]]; then
  mkdir -p "$logDir"
fi
logFull="$logDir/$(basename "$D")"
keysFile="$logDir/keys.txt"
regsFile="$logDir/final-regs.txt"
shot1="banner-1.png"; shot2="banner-2.png"; shot3="banner-3.png"
shotF="final-$Boot.png"

# ---- header -------------------------------------------------------------------
trace_list=""
for t in "${Trace[@]}"; do
  if [[ -n "$trace_list" ]]; then trace_list="$trace_list,"; fi
  trace_list="$trace_list$t"
done
echo "=== B310E QEMU run: boot=$Boot TimeSec=${TimeSec}s Trace=$trace_list ==="
echo "    log:      $logFull"
echo "    nor:      $Nor"
if [[ "$Boot" == "ours" ]]; then echo "    os:       $Os"; fi
echo "    monitor:  telnet:127.0.0.1:$MonitorPort"
if [[ "$Boot" == "ours" ]]; then
  expect="LCDC refresh lines + screendump banner + key DOWN/UP edges + ~1k/s IRQ tick exceptions, ZERO Taking-exception aborts"
else
  expect="sc6530_ana_* codec/ANA writes with guest PCs (the audio bring-up), sc6530_aux_* APB/PA-ladder + UNMODELED lines, sc6530_dsp_cmd lines"
fi
echo "    expected: $expect"

# Preserve any prior run's log (evidence discipline).
if [[ -f "$logFull" ]]; then
  stamp="$(date +%Y%m%d-%H%M%S)"
  cp -f "$logFull" "$logFull.prior-$stamp"
  echo "preserved prior log -> $logFull.prior-$stamp"
fi

# Kill zombie qemu (todo-17 lesson: stale qemu holds the chardev port).
if command -v pkill >/dev/null 2>&1; then
  pkill -f 'qemu-system-arm' 2>/dev/null || true
  sleep 0.8
else
  echo "    note: pkill not found (Linux procps) - skipping zombie-qemu kill"
fi

# ---- build the qemu command line ---------------------------------------------
if [[ "$Display" == "none" ]]; then
  display_arg="-display none"
elif [[ "$Display" == "gtk" ]]; then
  display_arg="-display gtk,zoom-to-fit=on"
elif [[ "$Display" == "sdl" ]]; then
  display_arg="-display sdl"
else
  display_arg="-display $Display"
fi
qargs=(-M "b310e,boot-mode=$Boot" ${display_arg} -serial none -d int -D "$logFull"
       -monitor "telnet:127.0.0.1:$MonitorPort,server,nowait"
       -drive "file=$Nor,format=raw,if=none,id=nor")
for t in "${Trace[@]}"; do
  qargs+=(--trace "$t")
done
if [[ "$Boot" == "ours" ]]; then
  qargs+=(-drive "file=$Os,format=raw,if=none,id=os")
fi

# Screendumps must land in $logDir - run qemu with that cwd.
cd "$logDir"
"$Qemu" "${qargs[@]}" >/dev/null 2>&1 &
qpid=$!
echo "qemu pid=$qpid workdir=$logDir"

# ---- monitor helpers (HMP via /dev/tcp) --------------------------------------
t0="$(date +%s)"
t0f=""
if [[ -n "${EPOCHREALTIME:-}" ]]; then t0f="$EPOCHREALTIME"; fi

elapsed_int() {
  echo "$(( $(date +%s) - t0 ))"
}
elapsed_sec() {
  if [[ -n "$t0f" && -n "${EPOCHREALTIME:-}" ]]; then
    awk -v b="$t0f" -v e="$EPOCHREALTIME" 'BEGIN { printf "%.1f", e - b }'
  else
    echo "$(( $(date +%s) - t0 ))"
  fi
}

wait_port() {
  local port="$1" i
  for i in $(seq 1 150); do
    if (exec 3<>"/dev/tcp/127.0.0.1/$port") 2>/dev/null; then
      return 0
    fi
    sleep 0.1
  done
  return 1
}

hmp() {
  local cmd="$1" resp=""
  if ! (exec 3<>"/dev/tcp/127.0.0.1/$MonitorPort") 2>/dev/null; then
    printf '%s' ""
    return 0
  fi
  exec 3<>"/dev/tcp/127.0.0.1/$MonitorPort" 2>/dev/null
  printf '%s\n' "$cmd" >&3
  sleep 0.3
  resp="$(timeout 1 dd bs=8192 count=1 <&3 2>/dev/null || true)"
  exec 3>&- 3<&- 2>/dev/null || true
  printf '%s' "$resp"
}

fmt_hmp() {
  # mirror the ps1's `-replace "`n", ' | '`
  printf '%s' "$1" | sed -e ':a;N;$!ba;s/\n/ | /g'
}

keyOut=()

send_one_key() {
  local k="$1" ts resp line
  ts="$(elapsed_sec)"
  if ! kill -0 "$qpid" 2>/dev/null; then
    echo "qemu exited early at t=${ts}s"
    return 1
  fi
  resp="$(hmp "sendkey $k")"
  line="t=${ts}s sendkey $k -> $(fmt_hmp "$resp")"
  keyOut+=("$line")
  echo "$line"
  return 0
}

if ! wait_port "$MonitorPort"; then
  echo "FAIL: monitor port never opened"
  exit 1
fi
echo "monitor port up ($MonitorPort)"

# ---- run sequence (fixed sleeps - no log polling) -----------------------------
if [[ "$Boot" == "ours" ]]; then
  # boot=ours: fixed banner evidence sequence (w7-32 shape)
  echo "waiting ${BannerSec}s for the banner (boot -> lcd init ~10-12s)..."
  sleep "$BannerSec"
  r1="$(hmp "screendump $shot1 -f png")"
  echo "screendump#1 -> $(fmt_hmp "$r1")"
  sleep 3
  r2="$(hmp "screendump $shot2 -f png")"
  echo "screendump#2 -> $(fmt_hmp "$r2")"
  sleep 1
  for k in "${Keys[@]}"; do
    if ! send_one_key "$k"; then break; fi
    sleep "$KeyDelay"
  done
  r3="$(hmp "screendump $shot3 -f png")"
  echo "screendump#3 -> $(fmt_hmp "$r3")"
else
  # warm|stock: periodic key injections until the time budget
  echo "warming up 5s, then keys every ${KeyDelay}s until t=$((TimeSec - 8))s..."
  sleep 5
  i=0
  while [[ ${#Keys[@]} -gt 0 ]] && [[ "$(elapsed_int)" -lt "$((TimeSec - 8))" ]]; do
    k="${Keys[$((i % ${#Keys[@]}))]}"
    if ! send_one_key "$k"; then break; fi
    i=$((i + 1))
    sleep "$KeyDelay"
  done
  r="$(hmp "screendump $shotF -f png")"
  echo "final screendump -> $(fmt_hmp "$r")"
fi

# ---- final state + clean quit -------------------------------------------------
tEnd="$(elapsed_int)"
remaining=$((TimeSec - tEnd))
if [[ "$remaining" -gt 0 ]]; then
  echo "cooldown ${remaining}s (total budget ${TimeSec}s)"
  sleep "$remaining"
fi

{
  echo "=== info registers ==="
  hmp "info registers"
  echo "=== xp 0x8100300c (sys ms counter) ==="
  hmp "xp /4wx 0x8100300c"
  echo "=== xp 0x20d00024 (lcdc img base) ==="
  hmp "xp /4wx 0x20d00024"
} > "$regsFile"

hmp "quit" >/dev/null
sleep 1
if kill -0 "$qpid" 2>/dev/null; then
  kill -9 "$qpid" 2>/dev/null || true
fi
elapsed="$(elapsed_int)"
if [[ ${#keyOut[@]} -gt 0 ]]; then
  printf '%s\n' "${keyOut[@]}" > "$keysFile"
else
  : > "$keysFile"
fi
echo "qemu exited; run elapsed ${elapsed}s; log -> $logFull"

# ---- expected-evidence print --------------------------------------------------
echo ""
echo "=== EXPECTED EVIDENCE (check these) ==="
echo "  raw log:   $logFull"
echo "  keys:      $keysFile"
if [[ "$Boot" == "ours" ]]; then
  echo "  screens:   $logDir/$shot1, $logDir/$shot2, $logDir/$shot3  (non-black banner, text bands)"
  echo "  log:       sc6530_lcdc_refresh lines (fb=0x340054c8), ~1k/s 'Taking exception 5 [IRQ]' ticks, 20 keypad DOWN/UP edges, ZERO non-IRQ exceptions"
else
  echo "  screen:    $logDir/$shotF"
  echo "  log:       sc6530_ana_read/write codec lines (0x820010xx/0x820011xx/0x820018xx family, guest PCs), sc6530_aux_* APB/PA-ladder + UNMODELED lines, sc6530_dsp_cmd lines"
  echo "  extract:   tools/qemu-b310e/scripts/extract-audio-trace.sh -Log $logFull"
fi
echo "  regs:      $regsFile (info registers + sys-ms + LCDC img base)"
exit 0
