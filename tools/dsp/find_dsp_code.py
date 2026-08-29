#!/usr/bin/env python3
"""Scan the firmware dump for genuine TeakLite-family 16-bit code regions.

The DSP_CODE partition (0x570000-0x68C000) was assumed to be dense TeakLite
code, but per-region analysis shows it is mostly MIDI ringtones, UTF-16
language strings, NV data and fill. This scanner locates blocks that really
decode as Teak-family code by combining:

  - tl_match : fraction of 16-bit LE words matching a GBATEK TL or TL2 row
               (family-classifier patterns; a real code stream scores high,
               data/pointers/strings score low)
  - distinct : number of distinct 16-bit words per block (code is diverse,
               tables are repetitive)

Blocks with tl_match >= 0.6 and distinct >= 500 are reported as code hits.
Usage:
  python find_dsp_code.py <gbatek_html> <file> [lo_hex] [hi_hex]
"""
import re, sys, struct, collections, importlib.util, os

spec = importlib.util.spec_from_file_location("tlfc", os.path.join(os.path.dirname(os.path.abspath(__file__)), "tl_family_classify.py"))
tlfc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tlfc)

def main():
    html_path, bin_path = sys.argv[1], sys.argv[2]
    lo = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0
    hi = int(sys.argv[4], 16) if len(sys.argv) > 4 else 0xFFFFFF
    html = open(html_path, encoding='latin-1').read()
    rows = tlfc.parse_rows(html)
    tl, tl2 = tlfc.build_patterns(rows)
    data = open(bin_path, 'rb').read()
    n = min(len(data), hi)
    print("scanning %s 0x%06X-0x%06X (%d bytes) with TL=%d/TL2=%d patterns" %
          (bin_path, lo, n, n - lo, len(tl), len(tl2)))
    hits = []
    for off in range(lo, n - 0x1000, 0x1000):
        words = struct.unpack_from('<2048H', data, off)
        wc = collections.Counter(words)
        distinct = len(wc)
        tl_c = tl2_c = 0
        for w in words:
            if tlfc.first_match(w, tl) is not None:
                tl_c += 1
            elif tlfc.first_match(w, tl2) is not None:
                tl2_c += 1
        tlm = (tl_c + tl2_c) / 2048
        top = ", ".join("%04X" % w for w, _ in wc.most_common(4))
        if tlm >= 0.55 and distinct >= 500:
            hits.append((off, tlm, tl_c / 2048, distinct, top))
    print("code-like hits: %d" % len(hits))
    for off, tlm, tlf, distinct, top in hits:
        print("0x%06X  tl+tl2=%.3f tl=%.3f distinct=%d  top=[%s]" %
              (off, tlm, tlf, distinct, top))

if __name__ == '__main__':
    main()
