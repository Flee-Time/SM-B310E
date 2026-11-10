#!/usr/bin/env python3
"""What the shell's `prof` caught: where the phone's CPU was, by function.

  python os/tools/prof-report.py prof.bin [APP.elf ...] [--os build/os/b310e-os.elf]
         [--top 40] [--callers] [--lines]

prof.bin (shell `prof SECONDS`, then `get /prof.bin`): "PROF", the samples, the ms,
the apps then loaded (name, image, size), then each tick's pc and lr. An app's pcs
are offsets into its .elf (apps are linked at 0): give the .elf of each app named
in the file, matched by its file name or in the given order. --callers splits the
busiest leaf functions by where they were called from (lr); --lines by source line.

SPDX-License-Identifier: GPL-2.0-or-later
"""
import argparse
import bisect
import collections
import os
import struct
import subprocess
import sys

NM = os.environ.get('NM', 'arm-none-eabi-nm')
ADDR2LINE = os.environ.get('ADDR2LINE', 'arm-none-eabi-addr2line')


class Syms:
    def __init__(self, elf):
        self.elf = elf
        out = subprocess.run([NM, '-n', '-C', '--defined-only', elf], capture_output=True,
                             text=True, check=True).stdout
        self.addr, self.name = [], []
        for line in out.splitlines():
            parts = line.split(None, 2)
            if len(parts) < 3 or parts[1] not in 'tTwW':
                continue
            if parts[2].startswith('$'):
                continue
            self.addr.append(int(parts[0], 16) & ~1)
            self.name.append(parts[2])

    def __call__(self, a):
        i = bisect.bisect_right(self.addr, a) - 1
        return self.name[i] if i >= 0 else f'?{a:#x}'


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('prof')
    ap.add_argument('elfs', nargs='*')
    ap.add_argument('--os', default='build/os/b310e-os.elf')
    ap.add_argument('--top', type=int, default=40)
    ap.add_argument('--callers', action='store_true')
    ap.add_argument('--lines', action='store_true')
    a = ap.parse_args()

    data = open(a.prof, 'rb').read()
    magic, n, ms, napps = struct.unpack_from('<4I', data)
    if magic != 0x464f5250:
        sys.exit('not a prof file')
    off = 16
    apps = []
    for i in range(napps):
        name = data[off:off + 20].split(b'\0')[0].decode('latin-1')
        base, size = struct.unpack_from('<2I', data, off + 20)
        off += 28
        apps.append((name, base, size))
    samples = [struct.unpack_from('<2I', data, off + 8 * i) for i in range(n)]

    oss = Syms(a.os)
    elfs = {}
    for i, (name, base, size) in enumerate(apps):
        match = [e for e in a.elfs if os.path.splitext(os.path.basename(e))[0].lower() in name.lower()
                 or name.lower().replace(' ', '') in os.path.basename(e).lower()]
        if match:
            elfs[name] = Syms(match[0])
        elif i < len(a.elfs):
            elfs[name] = Syms(a.elfs[i])

    def where(pc):
        for name, base, size in apps:
            if base <= pc < base + size:
                s = elfs.get(name)
                return name, pc - base, (s(pc - base) if s else f'+{pc - base:#x}')
        return 'OS', pc, oss(pc)

    funcs = collections.Counter()
    owners = collections.Counter()
    callers = collections.defaultdict(collections.Counter)
    pcs = collections.defaultdict(collections.Counter)
    for pc, lr in samples:
        owner, rel, fn = where(pc)
        owners[owner] += 1
        funcs[(owner, fn)] += 1
        callers[(owner, fn)][where(lr & ~1)[2]] += 1
        pcs[(owner, fn)][rel] += 1

    print(f'{n} samples in {ms} ms ({n * 1000 // max(ms, 1)} a second)')
    for name, base, size in apps:
        print(f'  app {name}: {base:#010x}, {size // 1024} KB')
    print()
    for owner, c in owners.most_common():
        print(f'{100 * c / n:6.1f}%  {owner}')
    print()
    for (owner, fn), c in funcs.most_common(a.top):
        print(f'{100 * c / n:6.1f}%  {c:6d}  {owner:12.12s}  {fn}')
        if a.callers:
            for who, k in callers[(owner, fn)].most_common(4):
                print(f'{"":26s}{100 * k / c:5.0f}% from {who}')
        if a.lines and owner in elfs:
            top = pcs[(owner, fn)].most_common(6)
            out = subprocess.run([ADDR2LINE, '-e', elfs[owner].elf] + [hex(r) for r, _ in top],
                                 capture_output=True, text=True).stdout.split()
            for (r, k), line in zip(top, out):
                print(f'{"":26s}{100 * k / c:5.0f}% {r:#x} {os.path.basename(line)}')


if __name__ == '__main__':
    main()
