#!/usr/bin/env python3
"""Analyze dsp_code_full.bin to determine DSP instruction width and code-vs-data regions.

For each 64KB window, test two hypotheses:
  H16LE: 16-bit little-endian words, opcode field = high byte (bits 15:8)
  H24BE: 24-bit big-endian words, opcode field = top byte (bits 23:16)

A code region should show a CONCENTRATED set of opcode values (a handful of
distinct opcodes cover most words). A data region shows near-uniform
distribution.

Also dump the distinct-word histogram for the most code-like window to sanity
check (a real instruction stream has a small alphabet of full encodings).
"""
import sys, struct, collections, os

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
PATH = os.path.join(REPO, ".omo", "dumpmine", "dsp", "dsp_code_full.bin")

def hist16le(data, off, size=0x10000):
    words = struct.unpack_from("<%dH" % (size//2), data, off)
    hi = collections.Counter(w >> 8 for w in words)
    return hi, words

def hist24be(data, off, size=0x10000):
    top = collections.Counter()
    n = size // 3
    for i in range(n):
        b = data[off + i*3]
        top[b] += 1
    return top, n

def hist16be(data, off, size=0x10000):
    words = struct.unpack_from(">%dH" % (size//2), data, off)
    hi = collections.Counter(w >> 8 for w in words)
    return hi, words

def hist24le(data, off, size=0x10000):
    top = collections.Counter()
    n = size // 3
    for i in range(n):
        b = data[off + i*3 + 2]  # LE: msb is 3rd byte
    # full word for opcode-in-low-bits check
    words = []
    for i in range(n):
        w = data[off+i*3] | (data[off+i*3+1] << 8) | (data[off+i*3+2] << 16)
        words.append(w)
    top = collections.Counter((w >> 16) & 0xFF for w in words)
    return top, words

def conc(hist, total):
    """Concentration: fraction of samples covered by the 8 most common values."""
    top = sum(v for _, v in hist.most_common(8))
    return top / max(total, 1)

def main():
    data = open(PATH, "rb").read()
    n = len(data)
    print(f"file size: 0x{n:X} ({n})")
    print(f"{'off':>8}  {'H16LE conc':>10} {'H16BE conc':>10} {'H24LE conc':>10} {'H24BE conc':>10}  verdict")
    W = 0x10000
    for off in range(0, n, W):
        if off + W > n:
            break
        h16l, w16l = hist16le(data, off)
        h16b, w16b = hist16be(data, off)
        h24l, _ = hist24le(data, off)
        h24b, _ = hist24be(data, off)
        c16l = conc(h16l, len(w16l)); c16b = conc(h16b, len(w16b))
        c24l = conc(h24l, len(w16l)//2); c24b = conc(h24b, len(w16l)//2)
        # winner
        best = max((c16l,"16LE"),(c16b,"16BE"),(c24l,"24LE"),(c24b,"24BE"))
        flag = "  <<<" if best[0] > 0.6 else ""
        print(f"0x{off:06X}  {c16l:10.3f} {c16b:10.3f} {c24l:10.3f} {c24b:10.3f}  {best[1]} ({best[0]:.3f}){flag}")
    # For the strongest 16LE code window, print the distinct full-word histogram
    print("\n=== distinct 16-bit LE words in 0x40000..0x44000 (top 24) ===")
    w16 = collections.Counter(struct.unpack_from("<0x4000H", data, 0x40000))
    for w, c in w16.most_common(24):
        print(f"0x{w:04X} : {c}")
    print(f"distinct words: {len(w16)} / 16384")

if __name__ == "__main__":
    main()
