#!/usr/bin/env python3
"""Classify every 4KB block of dsp_code_full.bin into content types.

Signature tests per 4KB block:
  ASCII16     : >=80% of 16-bit LE words have hi==0 and lo in 0x20..0x7e  (plain ASCII)
  UTF16_INDIC : >=50% of words have hi in 0x09..0x0D (Devanagari..Malayalam
                Unicode blocks) or hi==0x00 && lo printable (Latin-1-ish)
  ZERO        : >=95% bytes are 0x00
  FILL        : >=95% bytes identical (0x00/0xFF/...)
  CODE16      : 16-bit LE distinct words in [900, 3500] and top-3 high-byte
                coverage in [0.30, 0.80]  (dense code-like stream)
  TABLE       : distinct words in [50, 900] (repeating table / small alphabet)
  UNIFORM     : distinct words > 3500 (uniform data / compressed / random)

Also reports for CODE16 blocks the top high bytes (candidate opcode msbs).
The image layout: file offset == partition 0x570000 + offset.
"""
import struct, collections, sys, os

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
PATH = os.path.join(REPO, ".omo", "dumpmine", "dsp", "dsp_code_full.bin")
BASE = 0x570000

def classify(blk):
    words = struct.unpack_from("<1024H", blk, 0)   # 4KB = 2048 bytes = 1024 words
    n = len(words)
    zero = sum(1 for b in blk if b == 0) / len(blk)
    same = collections.Counter(blk).most_common(1)[0][1] / len(blk)
    # ascii16: hi==0 and printable low
    ascii16 = sum(1 for w in words if (w >> 8) == 0 and 0x20 <= (w & 0xFF) <= 0x7E) / n
    # indic16: hi in 0x09..0x0D
    indic16 = sum(1 for w in words if 0x09 <= (w >> 8) <= 0x0D) / n
    wc = collections.Counter(words)
    distinct = len(wc)
    hi = collections.Counter(w >> 8 for w in words)
    h3 = sum(v for _, v in hi.most_common(3)) / n

    tag = ""
    if zero >= 0.95:
        tag = "ZERO"
    elif same >= 0.95:
        tag = "FILL(0x%02X)" % blk[0]
    elif ascii16 >= 0.80:
        tag = "ASCII16"
    elif indic16 >= 0.50:
        top = ", ".join("U+%04X x%d" % (k, v) for k, v in wc.most_common(3))
        tag = "UTF16_INDIC(%s)" % top
    elif distinct >= 900 and distinct <= 3500 and 0.30 <= h3 <= 0.80:
        toph = ", ".join("%02X" % k for k, _ in hi.most_common(4))
        tag = "CODE16(d=%d,h3=%.2f,hi=[%s])" % (distinct, h3, toph)
    elif distinct < 900:
        top = ", ".join("%04X" % k for k, _ in wc.most_common(3))
        tag = "TABLE(d=%d,top=[%s])" % (distinct, top)
    else:
        tag = "UNIFORM(d=%d)" % distinct
    return tag

def main():
    data = open(PATH, "rb").read()
    n = len(data)
    # coarse 4KB-classification, then merge adjacent same-class runs
    runs = []
    prev = None
    start = 0
    for off in range(0, n - 0x1000, 0x1000):
        tag = classify(data[off:off + 0x1000])
        # simplify tag to a family
        fam = tag.split("(")[0]
        if fam != prev:
            if prev is not None:
                runs.append((start, off, prev))
            start = off
            prev = fam
    runs.append((start, n, prev))
    print("dsp_code_full.bin = %d bytes (partition 0x570000-0x%06X)" % (n, BASE + n))
    print("%-10s %-10s %-8s  %s" % ("file off", "partition", "size", "class"))
    for s, e, fam in runs:
        # get a representative tag detail for CODE16/TABLE blocks
        det = classify(data[s:s + 0x1000])
        print("0x%06X  0x%06X  %6X  %s%s" % (s, BASE + s, e - s, fam,
              ("  (" + det.split("(", 1)[1] if "(" in det else "")))

if __name__ == "__main__":
    main()
