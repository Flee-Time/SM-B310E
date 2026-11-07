#!/usr/bin/env python3
"""How fast Doom, Heretic or Hexen (os/ports/chocolate) starts and draws,
in QEMU at a fixed instruction rate (-icount: one instruction every
2^SHIFT ns, so the OS's clock is the instructions run; shift 2 is 250
million a second, about the phone's 208 MHz ARM926). Not the phone's
times (no cache, no PSRAM waits), but the same for the same code: for
comparing builds.

The card has the app, the WAD and args.txt with "-timedemo demo1": the
game starts (first_frame_ms: from Apps -> the game to its first frame),
plays demo1 as fast as it can and stops
with "timed N gametics in M realtics" (35 tics a second).

  python os/tests/bench-chocolate.py --game doom --wad path/to/doom1.wad

SPDX-License-Identifier: GPL-2.0-or-later
"""
import argparse
import json
import os
from pathlib import Path
import re
import runpy
import subprocess
import sys
import tempfile
import time

REPO = Path(__file__).resolve().parents[2]
boot = runpy.run_path(str(Path(__file__).with_name('test-boot.py')))
QT, capture = boot['QT'], boot['capture']
symbol, status, string, wait_for, free_port = (
    boot[n] for n in ('symbol', 'status', 'string', 'wait_for', 'free_port'))
MAGIC = boot['MAGIC']
NAMES = {'doom': 'Doom', 'heretic': 'Heretic', 'hexen': 'Hexen'}
CONFIG = {'doom': 'default.cfg', 'heretic': 'heretic.cfg', 'hexen': 'hexen.cfg'}


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--game', choices=sorted(NAMES), default='doom')
    parser.add_argument('--wad', type=Path, required=True)
    parser.add_argument('--demo', default='demo1')
    parser.add_argument('--icount', type=int, default=2, metavar='SHIFT')
    parser.add_argument('--config', action='append', default=[], metavar='LINE',
                        help='a line of the configuration, as "detaillevel 1"')
    parser.add_argument('--cluster-kb', type=int, default=0, metavar='KB',
                        help='FAT32 clusters of the card, in KB (default: the test '
                             'image\'s 0.5; a 16 GB card has 8 or 16)')
    parser.add_argument('--qemu', type=Path, default=REPO / 'build/qemu/build/qemu-system-arm.exe')
    parser.add_argument('--build', type=Path, default=REPO / 'build/os')
    args = parser.parse_args()
    name = NAMES[args.game]
    folder = f'b310e-os/apps/{name}'
    out = (args.build / f'bench-{args.game}').resolve()
    out.mkdir(parents=True, exist_ok=True)
    elf, image = args.build / 'b310e-os.elf', args.build / 'b310e-os.bin'
    base = symbol(elf, 'os_status')
    apps_list = symbol(elf, 'apps')
    kit = symbol(args.build / f'ports/{args.game}.elf', 'kit')
    argfile = out / 'args.txt'
    argfile.write_text(f'-timedemo {args.demo}\n')
    extra = []
    if args.config:
        cfg = out / 'game.cfg'
        cfg.write_text(''.join(line + '\n' for line in args.config))
        extra = ['--add', f'{folder}/{CONFIG[args.game]}={cfg}']
    card = out / 'card.img'
    maker, size_mib = REPO / 'ports/rockbox/tests/make-sd-image.py', 64
    if args.cluster_kb:                 # a copy of the image tool, larger clusters
        spc = args.cluster_kb * 2
        text = maker.read_text(encoding='utf-8').replace(
            'spc = 1 if fat32 else SPC', f'spc = {spc} if fat32 else SPC')
        maker = out / 'make-sd-image.py'
        maker.write_text(text, encoding='utf-8')
        size_mib = 1 << ((65536 * spc // 2048) + 64 - 1).bit_length()   # QEMU: a power of 2
    subprocess.run([sys.executable, str(maker),
                    '--fat32', '--size-mib', str(size_mib), '--seconds', '1', '--force',
                    '--runtime', str(REPO / 'sdcard/.rockbox'),
                    '--rockbox', str(REPO / 'sdcard/payloads/rockbox.bin'),
                    '--add', f'{folder}/{args.game}.app={(args.build / f"ports/{args.game}.app").resolve()}',
                    '--add', f'{folder}/{args.wad.name.lower()}={args.wad.resolve()}',
                    '--add', f'{folder}/args.txt={argfile}', *extra,
                    '--output', str(card)], check=True, capture_output=True)
    qmp_port, qtest_port = free_port(), free_port()
    cmd = [str(args.qemu.resolve()), '-M', 'b310e,boot-mode=rockbox',
           '-display', 'none', '-serial', 'none', '-monitor', 'none',
           '-icount', f'shift={args.icount},sleep=on',
           '-drive', f'file={image.resolve().as_posix()},format=raw,if=none,id=os,readonly=on',
           '-drive', f'file={card.as_posix()},format=raw,if=none,id=sdcard',
           '-qmp', f'tcp:127.0.0.1:{qmp_port},server=on,wait=off',
           '-qtest', f'tcp:127.0.0.1:{qtest_port},server=on,wait=off']
    env = os.environ.copy()
    if os.name == 'nt':
        env['PATH'] = r'D:\Toolchains\msys64\mingw64\bin;' + env.get('PATH', '')
    p = subprocess.Popen(cmd, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    qmp = qt = None
    result = {}
    try:
        for _ in range(100):
            try:
                qmp, qt = capture.QMP(qmp_port), QT(qtest_port)
                break
            except OSError:
                time.sleep(.1)

        def key(k, want=None, wait=0.5):
            qmp.hmp(f'sendkey {k} {200 if k == "esc" else 100}')
            if want:
                wait_for(qt, base, lambda s: string(qt, s['ui_title']) == want, 30, f'not {want}')
            time.sleep(wait)

        wait_for(qt, base, lambda s: s['magic'] == MAGIC, 30, 'OS never started')
        wait_for(qt, base, lambda s: s['ui_depth'] == 1, 120, 'no UI')
        time.sleep(1)
        key('esc', wait=1)
        key('ret', 'Menu')
        key('ret', 'Apps')
        key('ret', name, wait=1)
        t0 = status(qt, base)['ticks']
        end = time.monotonic() + 1800
        error = ''
        while time.monotonic() < end:
            app = qt.read(apps_list)
            if app:
                if 'first_frame_ms' not in result and qt.read(qt.read(app + 8) + kit):
                    result['first_frame_ms'] = status(qt, base)['ticks'] - t0   # the start-up
                error = string(qt, qt.read(app + 8) + kit + 4)
                if error:
                    break
            time.sleep(0.1)
        result['ticks_ms'] = status(qt, base)['ticks'] - t0
        result['error'] = error
        m = re.search(r'timed (\d+) gametics in (\d+) realtics', error)
        assert m, result
        tics, real = int(m[1]), int(m[2])
        result['gametics'], result['realtics'] = tics, real
        result['fps'] = round(35 * tics / real, 1) if real else None
        # Back (the error screen, once the game has ended), then power off:
        # the log reaches the card
        time.sleep(2)
        key('f2', 'Apps', wait=1)
        key('f2', wait=0.5)
        key('esc', wait=0.5)
        key('ret', 'Menu')
        for _ in range(5):
            key('down', wait=0.3)
        key('ret', 'Power')
        key('ret', 'Power off')
        qmp.hmp('sendkey f1 100')
        p.wait(timeout=60)
        log = (boot['card_file'](card.read_bytes(), '/b310e-os/log.txt') or b'').decode(errors='replace')
        m = re.search(rf'{args.game}: first frame after (\d+) ms', log)
        result['start_ms'] = int(m[1]) if m else None      # the kit's: launch to first frame
        result['log'] = [l.strip() for l in log.splitlines() if f'{args.game}:' in l][-40:]
    finally:
        if qt:
            qt.close()
        if qmp:
            qmp.close()
        if p.poll() is None:
            p.terminate()
            p.wait()
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
