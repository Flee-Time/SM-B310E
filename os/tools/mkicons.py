#!/usr/bin/env python3
"""Draw the OS's 12 x 12 list icons into os/ui/icons.c (4-bit alpha), and
the same as initializers for apps into os/sdk/icons.h (an app's own icon
in the Apps list: `static const icon_t icon = APP_ICON_MUSIC;`).

Simple geometric icons of our own, drawn at 8x and scaled down so the
edges are anti-aliased; the UI tints them (ui/gfx.c, gfx_icon).
Run when an icon changes; the output is committed.

SPDX-License-Identifier: GPL-2.0-or-later
"""
from pathlib import Path

from PIL import Image, ImageDraw

REPO = Path(__file__).resolve().parents[2]
N = 14                                  # design units
OUT = 12                                # pixels
S = 8                                   # supersampling
W = N * S


def canvas():
    img = Image.new('L', (W, W), 0)
    return img, ImageDraw.Draw(img)


def P(*v):                              # icon units (0..14) to canvas
    return [round(x * S) for x in v]


def rrect(d, x0, y0, x1, y1, r, fill=255, width=0):
    if width:
        d.rounded_rectangle(P(x0, y0, x1, y1), radius=r * S, outline=fill, width=round(width * S))
    else:
        d.rounded_rectangle(P(x0, y0, x1, y1), radius=r * S, fill=fill)


def line(d, pts, w):
    d.line([tuple(P(x, y)) for x, y in pts], fill=255, width=round(w * S), joint='curve')
    for x, y in (pts[0], pts[-1]):     # round caps
        d.ellipse(P(x - w / 2, y - w / 2, x + w / 2, y + w / 2), fill=255)


def apps(d):
    for x in (1.5, 8):
        for y in (1.5, 8):
            rrect(d, x, y, x + 4.5, y + 4.5, 1.2)


def folder(d):
    d.polygon([tuple(P(*p)) for p in ((1, 3), (5.5, 3), (7, 4.5), (13, 4.5), (13, 12), (1, 12))], fill=255)
    d.rectangle(P(1, 6, 13, 6.6), fill=0)


def file(d):
    d.polygon([tuple(P(*p)) for p in ((3, 1), (8.5, 1), (11.5, 4), (11.5, 13), (3, 13))], fill=255)
    d.polygon([tuple(P(*p)) for p in ((8.5, 1), (8.5, 4), (11.5, 4))], fill=0)


def music(d):
    d.ellipse(P(1.5, 9, 6, 13), fill=255)
    d.ellipse(P(8, 7.5, 12.5, 11.5), fill=255)
    d.rectangle(P(4.8, 2.5, 6, 11), fill=255)
    d.rectangle(P(11.3, 1, 12.5, 9.5), fill=255)
    d.polygon([tuple(P(*p)) for p in ((4.8, 2.5), (12.5, 1), (12.5, 3.5), (4.8, 5))], fill=255)


def radio(d):
    rrect(d, 1, 5, 13, 13, 1.5)
    d.ellipse(P(7.5, 7, 11.5, 11), fill=0)
    d.ellipse(P(8.7, 8.2, 10.3, 9.8), fill=255)
    d.rectangle(P(2.5, 7.5, 6, 8.3), fill=0)
    d.rectangle(P(2.5, 9.7, 6, 10.5), fill=0)
    line(d, [(3, 4.5), (10, 1)], 1.1)


def tools(d):
    line(d, [(3, 11), (8.5, 5.5)], 2.4)
    d.ellipse(P(7, 1, 13, 7), fill=255)
    d.polygon([tuple(P(*p)) for p in ((10, 0.5), (13.5, 0.5), (13.5, 4), (10, 4))], fill=0)
    d.ellipse(P(9.3, 2.2, 11.3, 4.2), fill=0)


def chart(d):
    for x, h in ((1.5, 5), (5.5, 9), (9.5, 7)):
        rrect(d, x, 13 - h, x + 3, 13, 0.8)


def gear(d):
    import math
    pts = []
    for i in range(16):
        a = i * math.pi / 8
        r = 6.4 if i % 2 == 0 else 4.9
        a2 = a + (math.pi / 16 if i % 2 == 0 else -math.pi / 16)
        pts.append((7 + r * math.cos(a - math.pi / 32), 7 + r * math.sin(a - math.pi / 32)))
        pts.append((7 + r * math.cos(a + math.pi / 32), 7 + r * math.sin(a + math.pi / 32)))
    d.polygon([tuple(P(*p)) for p in pts], fill=255)
    d.ellipse(P(4.8, 4.8, 9.2, 9.2), fill=0)


def power(d):
    d.arc(P(1.5, 2, 12.5, 13), start=-60, end=240, fill=255, width=round(1.6 * S))
    line(d, [(7, 1), (7, 7)], 1.6)


def torch(d):
    d.polygon([tuple(P(*p)) for p in ((2, 1), (12, 1), (9.5, 6), (4.5, 6))], fill=255)
    rrect(d, 4.5, 6.5, 9.5, 13, 1)
    d.ellipse(P(6.3, 8.3, 7.7, 9.7), fill=0)


def vibrate(d):
    rrect(d, 4, 1.5, 10, 12.5, 1.5)
    rrect(d, 5.3, 3, 8.7, 10, 0.5, fill=0)
    for x in (1.5, 12.5):
        line(d, [(x, 4.5), (x, 9.5)], 1.1)


def sim(d):
    d.polygon([tuple(P(*p)) for p in ((2.5, 1), (8.5, 1), (11.5, 4), (11.5, 13), (2.5, 13))], fill=255)
    rrect(d, 4.5, 6, 9.5, 11, 0.8, fill=0)
    d.rectangle(P(6.5, 6, 7.5, 11), fill=255)


def usb(d):
    line(d, [(7, 1.5), (7, 12)], 1.3)
    d.polygon([tuple(P(*p)) for p in ((7, 0.5), (9, 3), (5, 3))], fill=255)
    line(d, [(7, 9), (3.5, 6.5), (3.5, 5)], 1.2)
    line(d, [(7, 7.5), (10.5, 5.5), (10.5, 4.5)], 1.2)
    d.ellipse(P(2.2, 3.8, 4.8, 6.4), fill=255)
    d.rectangle(P(9.4, 3.4, 11.6, 5.6), fill=255)
    d.ellipse(P(5.5, 11, 8.5, 14), fill=255)


def clock(d):
    d.ellipse(P(1, 1, 13, 13), outline=255, width=round(1.5 * S))
    line(d, [(7, 3.8), (7, 7), (9.5, 8.6)], 1.3)


def sun(d):
    import math
    d.ellipse(P(4.3, 4.3, 9.7, 9.7), fill=255)
    for i in range(8):
        a = i * math.pi / 4
        line(d, [(7 + 4.6 * math.cos(a), 7 + 4.6 * math.sin(a)),
                 (7 + 6 * math.cos(a), 7 + 6 * math.sin(a))], 1.2)


def screen(d):
    rrect(d, 1, 2, 13, 10.5, 1.2, width=1.4)
    line(d, [(5, 12.5), (9, 12.5)], 1.3)


def cpu(d):
    rrect(d, 3.5, 3.5, 10.5, 10.5, 1)
    d.rectangle(P(5.3, 5.3, 8.7, 8.7), fill=0)
    for v in (5, 7, 9):
        line(d, [(v, 1), (v, 3)], 0.9)
        line(d, [(v, 11), (v, 13)], 0.9)
        line(d, [(1, v), (3, v)], 0.9)
        line(d, [(11, v), (13, v)], 0.9)


def info(d):
    d.ellipse(P(1, 1, 13, 13), fill=255)
    d.ellipse(P(6.1, 3, 7.9, 4.8), fill=0)
    rrect(d, 6.2, 6, 7.8, 11, 0.6, fill=0)


def play(d):
    d.polygon([tuple(P(*p)) for p in ((3.5, 2), (12, 7), (3.5, 12))], fill=255)


def record(d):
    d.ellipse(P(2.5, 2.5, 11.5, 11.5), fill=255)


def mic(d):
    rrect(d, 4.8, 1, 9.2, 8.5, 2.2)
    d.arc(P(3, 4, 11, 11), start=0, end=180, fill=255, width=round(1.2 * S))
    line(d, [(7, 11), (7, 13)], 1.2)


def restart(d):
    d.arc(P(1.5, 1.5, 12.5, 12.5), start=-30, end=270, fill=255, width=round(1.6 * S))
    d.polygon([tuple(P(*p)) for p in ((8.5, 0), (13, 2.5), (8.5, 5))], fill=255)


def calc(d):
    rrect(d, 2.5, 1, 11.5, 13, 1.5)
    d.rectangle(P(4, 2.5, 10, 5), fill=0)            # the display
    for y in (7, 9.5):
        for x in (4, 6.6, 9.2):
            d.rectangle(P(x, y, x + 1.6, y + 1.5), fill=0)
    d.rectangle(P(4, 11.5 - 0.4, 10.8, 12), fill=0)


def notes(d):
    rrect(d, 2, 1, 12, 13, 1.2)
    for y in (4, 6.8, 9.6):
        d.rectangle(P(4, y, 10 if y < 9 else 7.5, y + 1.1), fill=0)


def alarm(d):
    d.chord(P(2.5, 1.5, 11.5, 13), start=180, end=360, fill=255)    # the bell
    d.rectangle(P(2.5, 7.2, 11.5, 10.5), fill=255)
    rrect(d, 1, 10, 13, 11.6, 0.6)
    d.ellipse(P(5.6, 11.4, 8.4, 14), fill=255)                      # the clapper


def dot_on(d):
    d.ellipse(P(1.5, 1.5, 12.5, 12.5), outline=255, width=round(1.4 * S))
    d.ellipse(P(4.5, 4.5, 9.5, 9.5), fill=255)


def dot_off(d):
    d.ellipse(P(1.5, 1.5, 12.5, 12.5), outline=255, width=round(1.4 * S))


def speaker(d):
    d.polygon([tuple(P(*p)) for p in ((1.5, 5), (4.5, 5), (8, 2), (8, 12), (4.5, 9), (1.5, 9))], fill=255)
    d.arc(P(6, 3.5, 12, 10.5), start=-60, end=60, fill=255, width=round(1.2 * S))


def speaker_off(d):
    d.polygon([tuple(P(*p)) for p in ((1, 5), (4, 5), (7.5, 2), (7.5, 12), (4, 9), (1, 9))], fill=255)
    line(d, [(9.2, 5.2), (13, 9)], 1.3)
    line(d, [(13, 5.2), (9.2, 9)], 1.3)


def headphones(d):
    d.arc(P(2, 1.5, 12, 11.5), start=180, end=360, fill=255, width=round(1.4 * S))
    rrect(d, 1.2, 6.5, 3.9, 12, 1)
    rrect(d, 10.1, 6.5, 12.8, 12, 1)


def headset(d):
    d.arc(P(2, 0.5, 12, 10.5), start=180, end=360, fill=255, width=round(1.4 * S))
    rrect(d, 1, 4.5, 3.9, 9, 1)
    rrect(d, 10.1, 4.5, 13, 9, 1)
    line(d, [(2.4, 9), (2.4, 11), (6, 11.6)], 1.2)
    d.ellipse(P(5.8, 10.2, 8.8, 13), fill=255)


def card(d):
    d.polygon([tuple(P(*p)) for p in ((3, 1), (9.5, 1), (12, 3.5), (12, 13), (3, 13))], fill=255)
    for x in (5, 7, 9):
        d.rectangle(P(x, 2.5, x + 1, 5), fill=0)


ICONS = [apps, folder, file, music, radio, tools, chart, gear, power, torch, vibrate,
         sim, usb, clock, sun, screen, cpu, info, play, record, mic, restart,
         calc, notes, alarm, dot_on, dot_off, speaker, card, speaker_off, headphones, headset]


def main():
    out = ['/* B310E OS — list icons, 12 x 12, 4-bit alpha (os/tools/mkicons.py).',
           ' * SPDX-License-Identifier: GPL-2.0-or-later',
           ' */',
           '#include "os/ui.h"',
           '']
    sdk = ["/* B310E OS app SDK — the OS's icons as initializers (os/tools/mkicons.py).",
           ' *',
           ' *     static const icon_t icon = APP_ICON_MUSIC;',
           ' *     APP("Music", start, NULL, &icon);',
           ' *',
           ' * An icon is 12 x 12 pixels, 4-bit alpha, row by row, two pixels a byte',
           ' * (the left one in the high nibble); the UI tints it. Your own: 72 bytes',
           ' * in the same order.',
           ' *',
           ' * SPDX-License-Identifier: GPL-2.0-or-later',
           ' */',
           '#ifndef OS_SDK_ICONS_H',
           '#define OS_SDK_ICONS_H',
           '']
    for fn in ICONS:
        img, d = canvas()
        fn(d)
        small = img.resize((OUT, OUT), Image.LANCZOS)
        data = bytearray()
        for y in range(OUT):
            row = [(small.getpixel((x, y)) * 15 + 127) // 255 for x in range(OUT)]
            for x in range(0, OUT, 2):
                data.append(row[x] << 4 | row[x + 1])
        out.append(f'const icon_t icon_{fn.__name__} = {{ {{')
        for i in range(0, len(data), 12):
            out.append('    ' + ', '.join(f'0x{b:02x}' for b in data[i:i + 12]) + ',')
        out.append('} };')
        sdk.append(f'#define APP_ICON_{fn.__name__.upper()} {{ {{ \\')
        for i in range(0, len(data), 12):
            sdk.append('    ' + ', '.join(f'0x{b:02x}' for b in data[i:i + 12]) + ', \\')
        sdk.append('} }')
    (REPO / 'os/ui/icons.c').write_text('\n'.join(out) + '\n', encoding='utf-8', newline='\n')
    sdk += ['', '#endif']
    (REPO / 'os/sdk/icons.h').write_text('\n'.join(sdk) + '\n', encoding='utf-8', newline='\n')
    print(f'os/ui/icons.c, os/sdk/icons.h: {len(ICONS)} icons')


if __name__ == '__main__':
    main()
