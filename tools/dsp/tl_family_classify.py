#!/usr/bin/env python3
"""Determine TeakLite-I vs TeakLite-II from the DSP image.

Parses GBATEK's "DSi TeakLite II Instruction Set Encoding" opcode table
(http://problemkaputt.de/gbatek-dsi-teaklite-ii-instruction-set-encoding.htm),
which lists BOTH families: rows marked "TL" = TeakLite I, "TL2" = TeakLite II.
Both are 16-bit encodings (GBATEK: "The opcodes are 16bits wide") - the
plan's assumption that TL-II is 24-bit is factually wrong.

For every 16-bit word in a region we compute whether it matches a TL row, a
TL2 row, both, or neither (first-match in GBATEK table order). Variable bit
positions are the "@N" annotations in the operand text (plus "andN" suffixes
and "UnusedN@M" fields).

A real TeakLite-I (Oak) code stream should decode almost entirely via TL rows
with zero/few TL2-only opcodes; a TeakLite-II stream should contain many
TL2-only opcodes (0x0008/0x0028/0x0023/0x0010/... are TL2-only movs, D2Dx
register ALU forms, etc).

Usage:
  python tl_family_classify.py <html> <bin> <start_hex> <count> [step]
"""
import re, sys, collections, struct

def parse_rows(html):
    """Return list of (base, version, operand_text)."""
    # The page's <PRE> is unclosed (malformed HTML); anchor on the footer.
    m = re.search(r'<PRE>(.*?)(?:extracted from no\$gba|</TABLE>)', html, re.S)
    if not m:
        raise RuntimeError("PRE table not found in GBATEK html")
    pre = m.group(1)
    # strip tags / entities
    pre = re.sub(r'<[^>]+>', '', pre)
    pre = pre.replace('&nbsp;', ' ')
    lines = pre.splitlines()
    rows = []
    cur = None
    for ln in lines:
        ln = ln.strip()
        if not ln:
            continue
        mm = re.match(r'^([0-9A-F]{4})h\s+(TL2?)\s+(\S+)\s*(.*)$', ln, re.I)
        if mm:
            if cur:
                rows.append(cur)
            base = int(mm.group(1), 16)
            ver = mm.group(2).upper()
            cur = [base, ver, mm.group(4)]
        else:
            if cur:
                cur[2] += ' ' + ln
    if cur:
        rows.append(cur)
    return rows

def _field_width(base):
    """Width of the variable bit-field starting at @N for a GBATEK operand.

    Register/memory/immediate field widths per the GBATEK naming:
      ImmNx / MemImmN / AddressN / Imm4bitno   -> N bits (or extension if N>=16)
      Ax/Rx-family 2-bit regs                  -> 2 bits
      Ab/Bx/Axh/Axl/step/offs/ZI/... single     -> 1 bit
      MemRn/Rn                                    -> 3 bits
      R0123457y0/R0123457 (8 regs)                -> 3 bits
      R0123/R45/R4567/R04/R0425/R01 + MemR..      -> 2 bits
      Register (4-bit TL register field)          -> 4 bits
    """
    b = base.strip()
    m = re.match(r'Imm(\d+)', b)
    if m:
        return int(m.group(1))
    if b.startswith('MemImm') or b.startswith('Address'):
        m = re.match(r'MemImm(\d+)|Address(\d+)', b)
        return int(m.group(1) or m.group(2))
    if b.startswith('Imm4bitno'):
        return 4
    if b in ('Ax', 'R0123', 'R45', 'R4567', 'R04', 'R0425', 'R01',
             'MemR01', 'MemR0123', 'MemR45', 'MemR4567', 'MemR04',
             'MemR0425', 'MemR7Imm16', 'MemR7Imm7s', 'MemSp'):
        return 2 if b not in ('MemR7Imm16', 'MemR7Imm7s', 'MemSp') else (16 if b == 'MemR7Imm16' else 7)
    if b in ('MemRn', 'Rn', 'MemR0', 'MemR45', 'MemR4567'):
        return 3
    if b == 'Register' or b.startswith('RegisterP0'):
        return 4 if b == 'Register' else 2
    if b.startswith('R0123457'):
        return 3
    # everything else (Ab, Abh, Abl, Axh, Axl, Bx, Px, p0, p1, sv, mixp, etc) 1 bit
    return 1

def var_bits(operand_text):
    """All variable bit positions < 16 mentioned in the operand text."""
    bits = set()
    for mm in re.finditer(r'([A-Za-z0-9]+)@(\d+)(?:and(\d+))?', operand_text):
        b = int(mm.group(2))
        w = _field_width(mm.group(1))
        if mm.group(3) is not None:
            if int(mm.group(3)) < 16:
                bits.add(int(mm.group(3)))
        if b < 16:
            for k in range(w):
                if b + k < 16:
                    bits.add(b + k)
        # "UnusedN@M": the Unused field width is encoded in its name
        if mm.group(1).startswith('Unused'):
            m2 = re.match(r'Unused(\d+)', mm.group(1))
            if m2:
                w = int(m2.group(1))
                for k in range(w):
                    if b + k < 16:
                        bits.add(b + k)
    return bits

def build_patterns(rows):
    """Return (tl_pats, tl2_pats) where each pat = (base, mask)."""
    tl, tl2 = [], []
    for base, ver, oper in rows:
        # strip ;comments
        oper = re.sub(r';.*$', '', oper)
        bits = var_bits(oper)
        mask = 0xFFFF & ~sum(1 << b for b in bits)
        pat = (base, mask)
        (tl if ver == 'TL' else tl2).append(pat)
    return tl, tl2

def first_match(word, pats):
    for base, mask in pats:
        if (word & mask) == (base & mask):
            return base
    return None

def main():
    html_path, bin_path, start_hex, count = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
    start = int(start_hex, 16)
    html = open(html_path, encoding='latin-1').read()
    rows = parse_rows(html)
    tl, tl2 = build_patterns(rows)
    print("GBATEK rows parsed: %d  (TL=%d, TL2=%d)" % (len(rows), len(tl), len(tl2)))
    data = open(bin_path, 'rb').read()
    tl_c, tl2_c, both_c, none_c = 0, 0, 0, 0
    tl2_only_words = collections.Counter()
    wc = collections.Counter()
    n = min(count, (len(data) - start) // 2)
    words = struct.unpack_from('<%dH' % n, data, start)
    for w in words:
        wc[w] += 1
        in_tl = first_match(w, tl) is not None
        in_tl2 = first_match(w, tl2) is not None
        if in_tl and in_tl2:
            both_c += 1
        elif in_tl:
            tl_c += 1
        elif in_tl2:
            tl2_c += 1
            tl2_only_words[w] += 1
        else:
            none_c += 1
    tot = n
    print("region file 0x%06X count %d words" % (start, n))
    print("  matches TL only : %5d (%5.1f%%)" % (tl_c, 100.0 * tl_c / tot))
    print("  matches TL2 only: %5d (%5.1f%%)" % (tl2_c, 100.0 * tl2_c / tot))
    print("  matches BOTH    : %5d (%5.1f%%)" % (both_c, 100.0 * both_c / tot))
    print("  matches NEITHER : %5d (%5.1f%%)" % (none_c, 100.0 * none_c / tot))
    print("  distinct words  : %d" % len(wc))
    print("  top TL2-only words: %s" % ", ".join("%04X x%d" % (w, c)
          for w, c in tl2_only_words.most_common(12)))

if __name__ == '__main__':
    main()
