#!/usr/bin/env python3
"""ui/gfx.c against a reference gfx.c, pixel for pixel, on the host (no QEMU).

os/tests/gfx-host.c links both: the reference with its public names prefixed
ref_ (a header of #defines, force-included), the working tree's os/ui/gfx.c as
it is, the fonts (ui/font_montserrat.c), the icons (ui/icons.c) and lib/utf8.c.
Each seed calls every public gfx_* function with random arguments (off-screen
and negative coordinates, zero and negative sizes, every font and scale,
UTF-8 and broken text, max_w, alpha, radius, icons, blits, bars, graphs) over
solid theme colours and noise; the two frames, the return values and the
missing-glyph counters must stay byte for byte the same, and every row a call
changed must be inside the rows gfx.c marked for lcd_update_drawn()
(lcd_dirty_y0..y1, drivers/lcd.c).

The reference is the committed gfx.c by default (git:HEAD): a change to
gfx.c that must not change pixels is checked against the last commit. Any
other revision or a file can be given:

  python os/tests/test-gfx.py                         against git:HEAD
  python os/tests/test-gfx.py --ref git:HEAD~3
  python os/tests/test-gfx.py --ref path/to/old-gfx.c --seeds 8 --calls 500000

Needs a host gcc (PATH, $CC, or msys64's mingw64 as for QEMU). Built with
-fsanitize=undefined in trap mode when the compiler takes it.

SPDX-License-Identifier: GPL-2.0-or-later
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

REPO = Path(__file__).resolve().parents[2]
OS = REPO / 'os'
PUBLIC = ('gfx_fill gfx_blit gfx_shade gfx_hline gfx_rect gfx_round gfx_round_alpha '
          'gfx_icon gfx_text_height gfx_text_width gfx_text gfx_text_centered gfx_bar '
          'gfx_graph gfx_switch').split()
RENAMES = {n: 'ref_' + n for n in PUBLIC}
RENAMES.update(lcd_back='ref_back', os_status='ref_status',
               lcd_dirty_y0='ref_dirty_y0', lcd_dirty_y1='ref_dirty_y1')


def find_gcc():
    for cc in (os.environ.get('CC'), shutil.which('gcc'),
               r'D:\Toolchains\msys64\mingw64\bin\gcc.exe', r'C:\msys64\mingw64\bin\gcc.exe'):
        if cc and (Path(cc).exists() or shutil.which(cc)):
            return cc
    sys.exit('test-gfx: no host gcc (set CC)')


def reference(spec, tmp):
    if spec.startswith('git:'):
        src = subprocess.run(['git', '-C', str(REPO), 'show', spec[4:] + ':os/ui/gfx.c'],
                             check=True, capture_output=True).stdout
        path = tmp / 'gfx-ref.c'
        path.write_bytes(src)
        return path
    return Path(spec).resolve()


def build(cc, ref, tmp, sanitize):
    env = dict(os.environ)
    bindir = str(Path(cc).parent) if Path(cc).is_absolute() else None
    if bindir:                          # mingw's gcc finds its DLLs on PATH
        env['PATH'] = bindir + os.pathsep + env.get('PATH', '')
    rename = tmp / 'ref-rename.h'
    rename.write_text(''.join(f'#define {a} {b}\n' for a, b in RENAMES.items()))
    flags = ['-std=gnu11', '-O2', '-g', '-Wall', '-Wextra', '-Wno-unused-parameter',
             f'-I{OS}', f'-I{OS / "include"}'] + sanitize
    units = [(ref, ['-include', str(rename)]), (OS / 'ui/gfx.c', []),
             (OS / 'ui/font_montserrat.c', []), (OS / 'ui/icons.c', []),
             (OS / 'lib/utf8.c', []), (OS / 'tests/gfx-host.c', [])]
    objs = []
    for i, (src, extra) in enumerate(units):
        obj = tmp / f'{i}.o'
        subprocess.run([cc] + flags + extra + ['-c', str(src), '-o', str(obj)],
                       check=True, env=env)
        objs.append(str(obj))
    exe = tmp / 'gfx-host.exe'
    subprocess.run([cc] + sanitize + objs + ['-o', str(exe)], check=True, env=env)
    return exe, env


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--ref', default='git:HEAD', help='git:<rev> or a gfx.c file (git:HEAD)')
    ap.add_argument('--seeds', type=int, default=4)
    ap.add_argument('--calls', type=int, default=200000, help='calls a seed')
    ap.add_argument('--first-seed', type=int, default=1)
    ap.add_argument('--no-dirty', action='store_true',
                    help='do not check the rows marked for lcd_update_drawn')
    args = ap.parse_args()
    cc = find_gcc()
    with tempfile.TemporaryDirectory(prefix='test-gfx-') as t:
        tmp = Path(t)
        ref = reference(args.ref, tmp)
        san = ['-fsanitize=undefined', '-fsanitize-trap=undefined']
        probe = subprocess.run([cc] + san + ['-x', 'c', '-c', '-', '-o', str(tmp / 'p.o')],
                               input=b'int x;', capture_output=True)
        try:
            exe, env = build(cc, ref, tmp, san if probe.returncode == 0 else [])
        except subprocess.CalledProcessError as e:
            sys.exit(f'test-gfx: build failed ({e})')
        failed = 0
        for seed in range(args.first_seed, args.first_seed + args.seeds):
            r = subprocess.run([str(exe), str(seed), str(args.calls), '0' if args.no_dirty else '1'],
                               capture_output=True, text=True, env=env)
            out = (r.stdout + r.stderr).strip() or f'exit {r.returncode} (a trap: undefined behaviour?)'
            print(out)
            failed += r.returncode != 0
        print(f'test-gfx: {args.seeds - failed} of {args.seeds} seeds the same as {args.ref}')
        return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
