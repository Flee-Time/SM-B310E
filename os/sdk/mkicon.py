#!/usr/bin/env python3
"""An Apps menu folder's icon, from a picture: 12 x 12, 16 levels, the menu tints
it. Transparent or dark is off, opaque and light is on. The icon lines go into
the folder's .info, whose other lines stay (os/apps/apps.c says what they are):

  python os/sdk/mkicon.py picture.png /path/to/card/b310e-os/apps/Emulators/.info

SPDX-License-Identifier: GPL-2.0-or-later
"""
import os
import sys

from PIL import Image


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    img = Image.open(sys.argv[1]).convert('RGBA').resize((12, 12), Image.LANCZOS)
    rows = []
    for y in range(12):
        row = ''
        for x in range(12):
            r, g, b, a = img.getpixel((x, y))
            row += '%x' % ((r * 299 + g * 587 + b * 114) // 1000 * a // 255 // 17)
        rows.append('icon ' + row)
    kept = []
    if os.path.exists(sys.argv[2]):
        with open(sys.argv[2], encoding='utf-8') as f:
            kept = [l.rstrip() for l in f if l.strip() and not l.startswith('icon ')]
    with open(sys.argv[2], 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(kept + rows) + '\n')


if __name__ == '__main__':
    main()
