#!/usr/bin/env python3
"""Per-4KB teak_disasm decode-quality scan of dsp_code_full.bin.

For each 4KB block, run teak_disasm and score:
  err   : [ERROR] instructions
  dec   : decoded (non-error) instructions
  mnem  : distinct mnemonics
  bt    : fraction of branch targets (brr/callr/brs) landing inside 0x0..0x110000
          (the whole image) -- self-consistency proxy
Prints the most code-like blocks (low error, high mnemonic count, high
in-image branch fraction). A real TeakLite stream should score well;
pointer tables / UTF-16 text / data should score badly.
"""
import os, subprocess, re, collections, sys

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
TEAK = os.path.join(REPO, "build", "teakra", "build", "teak_disasm.exe")
IMG = os.path.join(REPO, ".omo", "dumpmine", "dsp", "dsp_code_full.bin")
if not os.path.exists(TEAK):
    sys.exit("teak_disasm.exe not found at %s - clone + build teakra into build/teakra (tools/dsp/TOOLING.md)" % TEAK)
IMG_WORDS = 0x11C000 // 2   # whole image in 16-bit words

def scan_block(off):
    # teak_disasm <file> <start_hex> <count> <base_hex>; base=0 -> addresses are word idx
    out = subprocess.run([TEAK, IMG, "%X" % off, "2048", "0"],
                         capture_output=True, text=True, timeout=60).stdout
    err = dec = 0
    mnems = collections.Counter()
    bt_targets = []
    for line in out.splitlines():
        line = line.strip()
        if not line:
            continue
        if line.startswith("^^"):   # expansion echo
            continue
        m = re.match(r"^[0-9A-F]{8}\s+[0-9A-F]{4}\s+(.*)$", line)
        if not m:
            continue
        body = m.group(1)
        dec += 1
        if "[ERROR]" in body:
            err += 1
        toks = body.split()
        if toks:
            mnems[toks[0]] += 1
        bm = re.search(r"\b(?:brr|callr|brs|callra|reti|br)\s+0x([0-9a-f]+)", body, re.I)
        if bm:
            bt_targets.append(int(bm.group(1), 16))
    # branch self-consistency: targets inside image word range
    bt_in = sum(1 for t in bt_targets if 0 <= t < IMG_WORDS) if bt_targets else 0
    btf = bt_in / len(bt_targets) if bt_targets else -1.0
    return dec, err, len(mnems), btf, len(bt_targets), mnems

def main():
    lo, hi = 0, 0x11C000
    if len(sys.argv) > 2:
        lo = int(sys.argv[1], 16); hi = int(sys.argv[2], 16)
    rows = []
    for off in range(lo, hi, 0x1000):
        try:
            dec, err, mnem, btf, nbt, mnems = scan_block(off)
        except subprocess.TimeoutExpired:
            print("0x%06X  TIMEOUT" % off)
            continue
        # code-likeness score
        score = 0.0
        if dec > 0:
            err_frac = err / dec
            score = (len(mnems) / 40.0) * 0.4 + (1 - err_frac) * 0.4 + (max(btf, 0) * 0.2)
        rows.append((off, dec, err, mnem, btf, nbt, score))
        print("0x%06X  dec=%4d err=%3d(%4.0f%%) mnem=%2d bt=%2d inimg=%s score=%.3f" %
              (off, dec, err, 100.0 * err / max(dec, 1), mnem, nbt,
               ("%.2f" % btf if btf >= 0 else "  - "), score))
    print("\n=== top-12 code-like blocks ===")
    for off, dec, err, mnem, btf, nbt, score in sorted(rows, key=lambda r: -r[6])[:12]:
        print("0x%06X  dec=%d err=%d mnem=%d bt=%d inimg=%s score=%.3f" %
              (off, dec, err, mnem, nbt, ("%.2f" % btf if btf >= 0 else "-"), score))

if __name__ == "__main__":
    main()
