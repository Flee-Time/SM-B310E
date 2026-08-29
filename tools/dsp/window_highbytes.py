#!/usr/bin/env python3
"""Print per-64K-window dominant high bytes for 16-bit LE words."""
import struct, collections, os

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
PATH = os.path.join(REPO, ".omo", "dumpmine", "dsp", "dsp_code_full.bin")
data = open(PATH, "rb").read()
n = len(data)
print("off      top high-bytes (16-bit LE words)  top1/top3 coverage   distinct words")
for off in range(0, n, 0x10000):
    words = struct.unpack_from("<16384H", data, off)
    hi = collections.Counter(w >> 8 for w in words)
    total = 16384
    top = hi.most_common(5)
    c1 = top[0][1] / total if top else 0
    c3 = sum(v for _, v in top[:3]) / total if top else 0
    t = ", ".join("%02X x%d" % (k, v) for k, v in top)
    distinct = len(collections.Counter(words))
    print("0x%06X  %s  top1=%.3f top3=%.3f  %d" % (off, t, c1, c3, distinct))
