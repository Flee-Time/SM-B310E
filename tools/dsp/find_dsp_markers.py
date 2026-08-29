#!/usr/bin/env python3
"""Locate genuine Teak-family DSP code by DSP-marker mnemonic density.

ARM Thumb code and MIDI data decode "plausibly" with a 16-bit Teak decoder by
coincidence. A genuine DSP program contains a distinctive set of DSP-specific
opcodes that Thumb/MIDI essentially never produce: mpy/mac/maa/msu/sqr/sqra/
divs/exp/norm/pacr/shfi/shfc/lim/bkrep/movd/movp/banke/mods/etc.

This scanner runs teak_disasm over each 4KB block and counts:
  - markers : occurrences of the DSP-marker mnemonics
  - mnem    : distinct mnemonics
  - err     : [ERROR] instructions
Blocks with high marker density + high mnemonic count are genuine DSP code.

Usage: python find_dsp_markers.py <lo_hex> <hi_hex>
"""
import os, subprocess, re, collections, sys

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
TEAK = os.path.join(REPO, "build", "teakra", "build", "teak_disasm.exe")
IMG = os.path.join(REPO, "dump_firmware.bin")
if not os.path.exists(TEAK):
    sys.exit("teak_disasm.exe not found at %s - clone + build teakra into build/teakra (tools/dsp/TOOLING.md)" % TEAK)

MARKERS = {
    "mpy", "mpyi", "mpysu", "mpyus", "macuu", "macsu", "macus", "mac",
    "maa", "maasu", "msu", "msusu", "sqr", "sqra", "divs", "exp", "norm",
    "pacr", "pacr1", "shfi", "shfc", "lim", "bkrep", "bkreprst", "bkrepsto",
    "movd", "movp", "movpdw", "banke", "bankr", "modr", "mods", "movs",
    "movsi", "tstb", "tst0", "tst1", "tst4b", "vtrclr", "vtrmov", "vtrshr",
    "swap", "cntx", "callr", "rets", "rep", "repd", "dint", "eint",
}

def scan_block(off):
    out = subprocess.run([TEAK, IMG, "%X" % off, "2048", "0"],
                         capture_output=True, text=True, timeout=60).stdout
    markers = collections.Counter()
    mnems = collections.Counter()
    err = dec = 0
    for line in out.splitlines():
        line = line.strip()
        if not line or line.startswith("^^"):
            continue
        m = re.match(r"^[0-9A-F]{8}\s+[0-9A-F]{4}\s+(.*)$", line)
        if not m:
            continue
        body = m.group(1)
        dec += 1
        if "[ERROR]" in body:
            err += 1
        toks = body.split()
        if not toks:
            continue
        mn = toks[0]
        mnems[mn] += 1
        if mn in MARKERS:
            markers[mn] += 1
    return dec, err, len(mnems), sum(markers.values()), dict(markers)

def main():
    lo = int(sys.argv[1], 16) if len(sys.argv) > 1 else 0
    hi = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0x800000
    hits = []
    for off in range(lo, hi, 0x1000):
        try:
            dec, err, mnem, nmark, markerd = scan_block(off)
        except subprocess.TimeoutExpired:
            continue
        if nmark >= 3:
            top = ", ".join("%s x%d" % (k, v) for k, v in
                            sorted(markerd.items(), key=lambda x: -x[1])[:6])
            hits.append((off, nmark, mnem, err, dec, top))
    print("blocks with >=3 DSP-marker ops: %d" % len(hits))
    for off, nmark, mnem, err, dec, top in hits:
        print("0x%06X  markers=%3d mnem=%2d err=%d/%d  [%s]" %
              (off, nmark, mnem, err, dec, top))

if __name__ == "__main__":
    main()
