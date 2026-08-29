#!/usr/bin/env python3
"""Rigorous Teak-family code scan: branch-target self-consistency.

Decodes 16-bit words with teakra and checks whether conditional-branch
targets (brr/callr) land exactly on instruction boundaries inside a local
window. Genuine straight-line code has most branches hitting instruction
starts; data/pointer tables/UTF-16 text hit arbitrary word positions.

For each 4KB block:
  - decode with teak_disasm, record every instruction's word offset + size
    (1 word, or 2 when an expansion word follows)
  - for each brr/callr, compute the target word address; count it "consistent"
    if it equals an instruction-start offset within the block
  - report blocks with many branches AND high consistency + diverse mnemonics
"""
import os, subprocess, re, collections, sys

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
TEAK = os.path.join(REPO, "build", "teakra", "build", "teak_disasm.exe")
if not os.path.exists(TEAK):
    sys.exit("teak_disasm.exe not found at %s - clone + build teakra into build/teakra (tools/dsp/TOOLING.md)" % TEAK)

def scan_block(img, off, n=1024):
    out = subprocess.run([TEAK, img, "%X" % off, str(n), "0"],
                         capture_output=True, text=True, timeout=60).stdout
    # parse lines: "ADDR  WORD   <body>" with "^^ expansion XXXX" continuation
    starts = []       # (word_offset, size) where size = 1 or 2
    branches = []     # (word_offset, target)
    mnems = collections.Counter()
    err = 0
    pos = 0           # word offset within block
    lines = out.splitlines()
    i = 0
    while i < len(lines):
        line = lines[i].strip()
        if not line or line.startswith("^^"):
            i += 1
            continue
        m = re.match(r"^[0-9A-F]{8}\s+[0-9A-F]{4}\s+(.*)$", line)
        if not m:
            i += 1
            continue
        body = m.group(1)
        if "[ERROR]" in body:
            err += 1
            size = 1
        else:
            toks = body.split()
            if toks:
                mnems[toks[0]] += 1
            # expansion consumed?
            nxt = lines[i + 1].strip() if i + 1 < len(lines) else ""
            size = 2 if nxt.startswith("^^") else 1
        starts.append((pos, size))
        if not err and pos < n:
            bm = re.search(r"\b(?:brr|callr)\s+0x([0-9a-f]+)", body, re.I)
            if bm:
                branches.append((pos, int(bm.group(1), 16)))
        pos += size
        i += 1
    return starts, branches, mnems, err

def main():
    img = sys.argv[1]
    lo = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0
    hi = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0x800000
    n = int(sys.argv[4]) if len(sys.argv) > 4 else 1024
    rows = []
    for off in range(lo, hi, 0x1000):
        try:
            starts, branches, mnems, err = scan_block(img, off, n)
        except subprocess.TimeoutExpired:
            continue
        starts_set = {s for s, _ in starts}
        nb = len(branches)
        if nb < 4:
            continue
        cons = sum(1 for _, t in branches if t in starts_set)
        cf = cons / nb
        # score: consistency * branchcount * mnemonic diversity
        score = cf * (0.3 + min(nb, 40) / 40.0) * (0.4 + min(len(mnems), 60) / 60.0)
        rows.append((off, nb, cf, cons, len(mnems), err, score,
                     ", ".join("%s x%d" % (k, v) for k, v in
                               sorted(mnems.items(), key=lambda x: -x[1])[:4])))
    print("blocks with >=4 branches: %d  (window n=%d words)" % (len(rows), n))
    for off, nb, cf, cons, mnem, err, score, top in sorted(rows, key=lambda r: -r[6])[:30]:
        print("0x%06X  br=%3d cons=%d (%.2f) mnem=%2d err=%d  [%s]" %
              (off, nb, cons, cf, mnem, err, top))

if __name__ == "__main__":
    main()
