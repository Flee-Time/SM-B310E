#!/usr/bin/env python3
"""Full-dump scan for 16-bit LE DSP-code-like regions.

Signature for a real (non-degenerate) 16-bit instruction stream:
  - distinct 16-bit words per 4KB in [700, 4000]  (tables < 400, uniform data > 4000)
  - top-1 word not dominant (< 15% of block)
  - top-3 high-byte coverage in [0.40, 0.95]  (code opcodes concentrated, not degenerate)
  - low printable fraction (not ASCII)
Also test a 24-bit reading (both endians): for 24-bit we check top-3 opcode-byte
coverage per 4KB (1365 instrs).
"""
import struct, collections, os

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
PATH = os.path.join(REPO, "dump_firmware.bin")
data = open(PATH, "rb").read()
n = len(data)

def scan16(off, blk):
    words = struct.unpack_from("<2048H", blk, 0)
    wc = collections.Counter(words)
    distinct = len(wc)
    top1 = wc.most_common(1)[0][1] / 2048
    hi = collections.Counter(w >> 8 for w in words)
    h3 = sum(v for _, v in hi.most_common(3)) / 2048
    return distinct, top1, h3

def scan24(off, blk):
    # top-3 byte-value coverage for each alignment (24-bit BE: msb first)
    best = 0.0
    for align in range(3):
        cnt = collections.Counter()
        total = 0
        for i in range(align, 0x1000 - 3, 3):
            cnt[blk[i]] += 1  # BE: first byte = top
            total += 1
        if total:
            c = sum(v for _, v in cnt.most_common(3)) / total
            best = max(best, c)
    return best

print("scanning 16MB dump for code-like 16-bit LE blocks...")
hits = []
for off in range(0, n - 0x1000, 0x1000):
    blk = data[off:off + 0x1000]
    printable = sum(1 for b in blk if 0x20 <= b <= 0x7e) / 4096
    distinct, top1, h3 = scan16(off, blk)
    if printable < 0.5 and 700 <= distinct <= 4000 and top1 < 0.15 and 0.40 <= h3 <= 0.95:
        hits.append((off, distinct, top1, h3, printable))
print("code-like hits: %d" % len(hits))
for off, distinct, top1, h3, printable in hits:
    blk = data[off:off + 0x1000]
    hi = collections.Counter(w >> 8 for w in struct.unpack_from("<2048H", blk, 0))
    ht = ", ".join("%02X x%d" % (k, v) for k, v in hi.most_common(5))
    c24 = scan24(off, blk)
    print("0x%06X d=%5d top1=%.3f h3=%.3f asc=%.2f c24=%.3f  hi=[%s]" % (off, distinct, top1, h3, printable, c24, ht))
