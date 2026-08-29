#!/usr/bin/env python3
"""Fine-grained 4KB-block scan: find 16-bit LE code-like regions.

For each 4KB block, report:
  distinct: number of distinct 16-bit LE words
  top1/top3: coverage of the most common high-byte values
  looks like: CODE (concentrated-but-not-degenerate), TABLE (very few
  distinct), DATA (uniform), ASCII
"""
import struct, collections, os

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
PATH = os.path.join(REPO, ".omo", "dumpmine", "dsp", "dsp_code_full.bin")
data = open(PATH, "rb").read()
n = len(data)

def classify(off):
    blk = data[off:off+0x1000]
    # ASCII check
    printable = sum(1 for b in blk if 0x20 <= b <= 0x7e)
    words = struct.unpack_from("<2048H", blk, 0)
    wc = collections.Counter(words)
    distinct = len(wc)
    top1 = wc.most_common(1)[0][1] / 2048
    hi = collections.Counter(w >> 8 for w in words)
    h3 = sum(v for _, v in hi.most_common(3)) / 2048
    tag = ""
    if printable / 4096 > 0.8:
        tag = "ASCII"
    elif distinct < 400:
        tag = "TABLE(hi=0x%02X)" % hi.most_common(1)[0][0] if hi else "TABLE"
    elif h3 > 0.85:
        tag = "CODE16LE(hi=0x%02X,0x%02X,0x%02X)" % (hi.most_common(3)[0][0], hi.most_common(3)[1][0], hi.most_common(3)[2][0])
    elif h3 > 0.55:
        tag = "code-ish"
    else:
        tag = "DATA"
    return f"{off:06X}  d={distinct:5d} top1={top1:.2f} h3={h3:.2f}  {tag}"

# Scan 0x0 .. 0x20000 and 0x80000-0xE0000 densely; print all blocks 0..0x20000
print("=== 0x0 - 0x20000 (4KB blocks) ===")
for off in range(0, 0x20000, 0x1000):
    print(classify(off))
print("\n=== 0x80000 - 0xE0000 (4KB blocks, code candidate range) ===")
for off in range(0x80000, 0xE0000, 0x1000):
    print(classify(off))
