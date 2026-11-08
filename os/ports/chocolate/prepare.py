#!/usr/bin/env python3
"""Heretic's and Hexen's r_plane.c for low detail (low_detail.c says why): the sky
is the one thing they draw without the column drawers, one byte at x. Here, at x
doubled, and twice. Upstream's file stays as it is; each change is checked:

  python os/ports/chocolate/prepare.py <r_plane.c> <out>

SPDX-License-Identifier: GPL-2.0-or-later
"""
from pathlib import Path
import sys

CHANGES = {
    'heretic': [('dest = ylookup[dc_yl] + columnofs[dc_x];', 'dest = ylookup[dc_yl] + columnofs[dc_x << detailshift];'),
                ('*dest = dc_source[frac];', '*dest = dest[detailshift] = dc_source[frac];')],
    'hexen': [('dest = ylookup[dc_yl] + columnofs[x];', 'dest = ylookup[dc_yl] + columnofs[x << detailshift];'),
              ('*dest = *source++;', '*dest = dest[detailshift] = *source++;'),
              ('*dest = *source2++;', '*dest = dest[detailshift] = *source2++;')],     # its double sky
}


def main():
    src, out = Path(sys.argv[1]), Path(sys.argv[2])
    text = src.read_bytes().decode('latin-1')
    for old, new in CHANGES[src.parent.name]:
        if old not in text:
            sys.exit(f'prepare.py: {src} is not what it was (looked for {old!r})')
        text = text.replace(old, new)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(text.encode('latin-1'))


if __name__ == '__main__':
    main()
