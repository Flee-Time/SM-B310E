#!/usr/bin/env python3
"""Wolfenstein 3D (os/ports/wolf4sdl: upstream Wolf4SDL on the game kit, its
SDL ours) in QEMU, from Apps.

The card has wolf3d.app and the data (*.WL6) in /b310e-os/apps/Wolf3D.
Apps -> Wolf3D: the sign-on and title screens show (frames counted by
the kit, a screenshot with their colours). Keys get through them to the
menu; Center (Enter) four times starts a new game (New Game, the
episode, the difficulty); Right held turns: the view changes. END asks
to quit (F10), LSoft says yes (Y): back in Apps, the app unloaded, the
heap back where it was, "wolf3d: ended (0)" in the log, its config
(config.wl6) beside the data.

The data is the user's (not in the repository):

  python os/tests/test-wolf.py --data path/to/wolf3d

SPDX-License-Identifier: GPL-2.0-or-later
"""
import argparse
import json
import os
from pathlib import Path
import runpy
import subprocess
import sys
import time

REPO = Path(__file__).resolve().parents[2]
boot = runpy.run_path(str(Path(__file__).with_name('test-boot.py')))
QT, capture = boot['QT'], boot['capture']
symbol, status, string, wait_for, free_port, card_file = (
    boot[n] for n in ('symbol', 'status', 'string', 'wait_for', 'free_port', 'card_file'))
MAGIC = boot['MAGIC']
FOLDER = '/b310e-os/apps/Wolf3D'


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--data', type=Path, required=True, help='the folder with the *.WL6')
    parser.add_argument('--qemu', type=Path, default=REPO / 'build/qemu/build/qemu-system-arm.exe')
    parser.add_argument('--build', type=Path, default=REPO / 'build/os')
    args = parser.parse_args()
    files = sorted(p for p in args.data.iterdir() if p.suffix.lower() == '.wl6')
    if not files:
        sys.exit(f'test-wolf: no *.WL6 in {args.data}')
    out = (args.build / 'test-wolf').resolve()
    out.mkdir(parents=True, exist_ok=True)
    elf, image = args.build / 'b310e-os.elf', args.build / 'b310e-os.bin'
    base = symbol(elf, 'os_status')
    apps_list = symbol(elf, 'apps')
    kit = symbol(args.build / 'ports/wolf3d.elf', 'kit')
    card = out / 'card.img'
    adds = ['--add', f'{FOLDER[1:]}/wolf3d.app={(args.build / "ports/wolf3d.app").resolve()}']
    for f in files:
        adds += ['--add', f'{FOLDER[1:]}/{f.name.lower()}={f.resolve()}']
    subprocess.run([sys.executable, str(REPO / 'ports/rockbox/tests/make-sd-image.py'),
                    '--fat32', '--size-mib', '64', '--seconds', '1', '--force',
                    '--runtime', str(REPO / 'sdcard/.rockbox'),
                    '--rockbox', str(REPO / 'sdcard/payloads/rockbox.bin'),
                    *adds, '--output', str(card)], check=True, capture_output=True)
    qmp_port, qtest_port = free_port(), free_port()
    cmd = [str(args.qemu.resolve()), '-M', 'b310e,boot-mode=rockbox',
           '-display', os.environ.get('B310E_QEMU_DISPLAY', 'none'), *os.environ.get('B310E_QEMU_AUDIO', '').split(),
           '-serial', 'none', '-monitor', 'none',
           '-drive', f'file={image.resolve().as_posix()},format=raw,if=none,id=os,readonly=on',
           '-drive', f'file={card.as_posix()},format=raw,if=none,id=sdcard',
           '-qmp', f'tcp:127.0.0.1:{qmp_port},server=on,wait=off',
           '-qtest', f'tcp:127.0.0.1:{qtest_port},server=on,wait=off']
    env = os.environ.copy()
    if os.name == 'nt':
        env['PATH'] = r'D:\Toolchains\msys64\mingw64\bin;' + env.get('PATH', '')
    result = {}
    p = subprocess.Popen(cmd, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    qmp = qt = None
    try:
        for _ in range(100):
            try:
                qmp, qt = capture.QMP(qmp_port), QT(qtest_port)
                break
            except OSError:
                time.sleep(.1)
        assert qmp and qt, 'QEMU did not start'

        def key(k, want=None, wait=0.4, hold=100):
            qmp.hmp(f'sendkey {k} {200 if k == "esc" else hold}')
            if want:
                wait_for(qt, base, lambda s: string(qt, s['ui_title']) == want, 15, f'not {want}')
            time.sleep(wait)

        def shot(label):
            path = out / f'{label}.png'
            qmp.command('screendump', {'filename': str(path), 'format': 'png'})
            time.sleep(0.3)
            return path

        def frames():
            app = qt.read(apps_list)
            return qt.read(qt.read(app + 8) + kit) if app else 0

        def picture(path):
            from PIL import Image
            return Image.open(path).convert('RGB')

        def colours(path):
            return len(picture(path).getcolors(1 << 16) or [])

        wait_for(qt, base, lambda s: s['magic'] == MAGIC, 10, 'OS never started')
        wait_for(qt, base, lambda s: s['ui_depth'] == 1, 60, 'no UI')
        time.sleep(1)
        result['heap_before'] = status(qt, base)['heap_free']

        key('esc', wait=0.5)
        key('ret', 'Menu')
        key('ret', 'Apps')
        key('ret', 'Wolf3D', wait=0.5)
        wait_for(qt, base, lambda s: s['apps_loaded'] == 1 and frames() > 0, 60, 'no frames')
        time.sleep(4)
        result['title_colours'] = colours(shot('wolf-title'))
        assert result['title_colours'] > 8, result
        for i in range(4):                                    # through the title screens
            key('ret', wait=1.5)
        shot('wolf-menu')
        for i in range(4):                                    # New Game, episode, difficulty
            key('ret', wait=1.5)
        time.sleep(3)
        before = shot('wolf-game')
        key('right', wait=1.0, hold=1200)
        after = shot('wolf-turned')
        from PIL import ImageChops
        result['view_changed'] = ImageChops.difference(picture(before), picture(after)).getbbox() is not None
        result['game_colours'] = colours(after)
        result['frames'] = frames()
        assert result['view_changed'] and result['game_colours'] > 8, result

        key('esc', wait=1.5)                                  # END: F10, quit?
        shot('wolf-quit')
        key('f1')                                             # LSoft: Y
        wait_for(qt, base, lambda s: string(qt, s['ui_title']) == 'Apps', 20, 'not back in Apps')
        wait_for(qt, base, lambda s: s['apps_loaded'] == 0, 10, 'still loaded')
        time.sleep(1.5)
        result['heap_after'] = status(qt, base)['heap_free']
        assert abs(result['heap_after'] - result['heap_before']) < 16 * 1024, result

        key('f2', 'Menu')
        for _ in range(5):
            key('down')
        key('ret', 'Power')
        key('ret', 'Power off')
        qmp.hmp('sendkey f1 100')
        p.wait(timeout=15)
    except Exception:
        print(json.dumps(result, indent=2))
        if qmp:
            try:
                qmp.command('screendump', {'filename': str(out / 'failed.png'), 'format': 'png'})
            except Exception:
                pass
        raise
    finally:
        if qt:
            qt.close()
        if qmp:
            qmp.close()
        if p.poll() is None:
            p.terminate()
            p.wait()

    img = card.read_bytes()
    text = (card_file(img, '/b310e-os/log.txt') or b'').decode(errors='replace')
    result['log'] = [l.strip() for l in text.splitlines() if 'wolf3d:' in l][-30:]
    result['config'] = bool(card_file(img, f'{FOLDER}/config.wl6'))
    print(json.dumps(result, indent=2))
    assert any('wolf3d: ended (0)' in l for l in result['log']), result
    print('test-wolf: PASS')


if __name__ == '__main__':
    main()
