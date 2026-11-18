#!/usr/bin/env python3
"""Render Montserrat into the OS's anti-aliased bitmap fonts (os/ui/font_montserrat.c).

Each glyph is stored as 4-bit alpha (two pixels a byte, high nibble first),
cropped to its ink, with its offset from the pen position on the baseline
and its advance. The OS blends them into the frame (ui/gfx.c) and finds a
glyph by its code point: ASCII by index, the rest by binary search in the
table, which is sorted by code.

Run when the sizes or the character set change; the output is committed.
Montserrat is under the SIL Open Font License 1.1 (os/ui/fonts/OFL.txt);
so is the generated file. Get the TTFs with:

  https://github.com/JulietaUla/Montserrat/raw/master/fonts/ttf/Montserrat-<Weight>.ttf

  python os/tools/mkfont.py --ttf-dir build/fonts/montserrat

A character the font does not have is left out (and listed): the OS draws
'?' for it rather than a made-up glyph.

SPDX-License-Identifier: GPL-2.0-or-later (this script)
"""
import argparse
import hashlib
from pathlib import Path
import struct

from PIL import Image, ImageDraw, ImageFont

REPO = Path(__file__).resolve().parents[2]


def span(a, b):
    return [chr(c) for c in range(a, b + 1)]


ASCII = span(0x20, 0x7e)
TEXT = (ASCII
        + [c for c in span(0xa0, 0xff) if c != '­']    # Latin-1; no soft hyphen
        + span(0x100, 0x17f)                    # Latin Extended-A: Turkish, Central Europe
        + span(0x218, 0x21b)                    # Romanian comma-below Ș ș Ț ț
        + span(0x400, 0x45f) + ['Ґ', 'ґ']   # Cyrillic, Ukrainian Ґ ґ
        + ['π']                            # π
        # General Punctuation: dashes, quotes, daggers, bullet, ellipsis, per mille,
        # primes, angle quotes, fraction slash
        + list('‐‒–—―‘’‚‛“”„‟'
               '†‡•…‰′″‹›‼⁄')
        + span(0x20a0, 0x20bf)                  # currency: € ₺ ₽ ₴ ₹ ...
        + list('ℓ№™Ω℮')    # ℓ № ™ Ω ℮
        + span(0x2190, 0x2193)                  # arrows
        + list('∂∆∏∑−∙√∞∫≈≠≤≥'))
DIGITS = list(' 0123456789:.-+%')

# name, weight file, pixel size, characters
FONTS = [
    ('small', 'Medium', 9, TEXT),
    ('body', 'Medium', 10, TEXT),
    ('title', 'SemiBold', 12, TEXT),
    ('huge', 'Medium', 24, DIGITS),
]

SHA256 = {
    'Montserrat-Medium.ttf': 'dae47428bb041f9716604e0e07b5b0c8585b3bdd8183362f75c69fe7bb3cfaf4',
    'Montserrat-SemiBold.ttf': 'b4e1563393d73fdff491a869441245aef31add2ec03d9c97a6dae4de07c52fd0',
}


def cmap(path):
    """The code points a TrueType font maps (cmap formats 4 and 12)."""
    d = path.read_bytes()
    tables = {}
    for i in range(struct.unpack('>H', d[4:6])[0]):
        tag, _, off, _ = struct.unpack('>4sIII', d[12 + 16 * i:28 + 16 * i])
        tables[tag] = off
    c, codes = tables[b'cmap'], set()
    for i in range(struct.unpack('>H', d[c + 2:c + 4])[0]):
        _, _, off = struct.unpack('>HHI', d[c + 4 + 8 * i:c + 12 + 8 * i])
        t = c + off
        fmt = struct.unpack('>H', d[t:t + 2])[0]
        if fmt == 4:
            seg2 = struct.unpack('>H', d[t + 6:t + 8])[0]
            ends = struct.unpack(f'>{seg2 // 2}H', d[t + 14:t + 14 + seg2])
            starts = struct.unpack(f'>{seg2 // 2}H', d[t + 16 + seg2:t + 16 + 2 * seg2])
            for s, e in zip(starts, ends):
                if s != 0xffff:
                    codes.update(range(s, e + 1))
        elif fmt == 12:
            for g in range(struct.unpack('>I', d[t + 12:t + 16])[0]):
                s, e, _ = struct.unpack('>III', d[t + 16 + 12 * g:t + 28 + 12 * g])
                codes.update(range(s, e + 1))
    return codes


def render(font, ch):
    """(width, height, x offset, y offset from the baseline, advance, alpha rows)."""
    advance = round(font.getlength(ch))
    if ch == ' ':
        return 0, 0, 0, 0, advance, []
    size = font.size * 3
    img = Image.new('L', (size, size), 0)
    origin = (font.size, font.size * 2)          # pen position on the baseline
    ImageDraw.Draw(img).text(origin, ch, font=font, fill=255, anchor='ls')
    box = img.getbbox()
    if not box:
        return 0, 0, 0, 0, advance, []
    x0, y0, x1, y1 = box
    rows = [[img.getpixel((x, y)) for x in range(x0, x1)] for y in range(y0, y1)]
    return x1 - x0, y1 - y0, x0 - origin[0], y0 - origin[1], advance, rows


def pack(rows, w):
    out = bytearray()
    for row in rows:
        q = [(v * 15 + 127) // 255 for v in row]
        if w & 1:
            q.append(0)
        for i in range(0, len(q), 2):
            out.append(q[i] << 4 | q[i + 1])
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ttf-dir', type=Path, default=REPO / 'build/fonts/montserrat')
    parser.add_argument('--output', type=Path, default=REPO / 'os/ui/font_montserrat.c')
    args = parser.parse_args()
    for name, digest in SHA256.items():
        got = hashlib.sha256((args.ttf_dir / name).read_bytes()).hexdigest()
        if got != digest:
            raise SystemExit(f'{name}: SHA-256 {got}, expected {digest}')

    lines = [
        '/* Montserrat, rendered by os/tools/mkfont.py: 4-bit alpha glyphs.',
        ' *',
        ' * Copyright 2024 The Montserrat.Git Project Authors',
        ' * (https://github.com/JulietaUla/Montserrat.git)',
        ' * Licensed under the SIL Open Font License, Version 1.1 (os/ui/fonts/OFL.txt).',
        ' * This file is a Modified Version of the Font Software and is under the',
        ' * same license, not the GPL of the rest of the OS.',
        ' *',
        ' * SPDX-License-Identifier: OFL-1.1',
        ' */',
        '#include "ui/font.h"',
        '',
    ]
    total = 0
    for name, weight, px, chars in FONTS:
        ttf = args.ttf_dir / f'Montserrat-{weight}.ttf'
        have = cmap(ttf)
        missing = [c for c in chars if ord(c) not in have]
        if missing:
            print(f'{name}: not in {ttf.name}, left out: ' +
                  ' '.join(f'U+{ord(c):04X}' for c in missing))
        chars = sorted({c for c in chars if ord(c) in have}, key=ord)
        font = ImageFont.truetype(str(ttf), px)
        ascent, descent = font.getmetrics()
        glyphs, bitmap = [], bytearray()
        for ch in chars:
            w, h, x, y, adv, rows = render(font, ch)
            glyphs.append((ord(ch), len(bitmap), w, h, x, y, adv))
            bitmap += pack(rows, w)
        assert len(bitmap) < 65536, f'{name}: {len(bitmap)} bytes, past a 16-bit offset'
        assert [g[0] for g in glyphs[:95]] == list(range(0x20, 0x7f)) or name == 'huge'
        table = len(glyphs) * 10                 # sizeof(font_glyph_t)
        total += len(bitmap) + table
        print(f'{name}: {len(glyphs)} glyphs, {len(bitmap)} bytes of bitmaps + {table} of table')
        lines.append(f'static const uint8_t {name}_bits[{len(bitmap)}] = {{')
        for i in range(0, len(bitmap), 16):
            lines.append('    ' + ', '.join(f'0x{b:02x}' for b in bitmap[i:i + 16]) + ',')
        lines.append('};')
        lines.append(f'static const font_glyph_t {name}_glyphs[{len(glyphs)}] = {{')
        for code, off, w, h, x, y, adv in glyphs:
            lines.append(f'    {{ 0x{code:04x}, {off:5d}, {w:2d}, {h:2d}, {x:3d}, {y:4d}, {adv:2d} }},')
        lines.append('};')
        lines.append(f'const font_t font_{name} = {{ {ascent}, {descent}, {len(glyphs)}, '
                     f'{name}_glyphs, {name}_bits }};')
        lines.append('')
    args.output.write_text('\n'.join(lines), newline='\n')
    print(f'{args.output}: {len(FONTS)} fonts, {total} bytes')


if __name__ == '__main__':
    main()
