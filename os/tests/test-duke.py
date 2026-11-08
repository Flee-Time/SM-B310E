#!/usr/bin/env python3
"""Duke Nukem 3D (os/ports/duke: upstream's jfduke3d and jfbuild on the game kit) in
QEMU, from Apps.

The card has the app and DUKE3D.GRP in /b310e-os/apps/Duke 3D. Apps -> Duke 3D: the
logos (frames counted by the kit, a screenshot with their colours). Center (Enter)
goes through them and the menus to a new game: a map is in (numsectors); Up held walks:
the view changes. The player dies (health 0): the frames go on through the tilted view,
and LSoft restarts the level. END asks to quit (F10), LSoft says yes (Y): back in Apps, the app unloaded,
the heap back where it was, "duke: ended (0)" in the log. Then END held: Duke again, it
closes as asked; and again with its thread hung (a loop poked into its code): it is
killed, the phone stays on, the heap comes back and Duke can start once more.

The data is the user's (not in the repository):

  python os/tests/test-duke.py --grp path/to/DUKE3D.GRP

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
FOLDER = '/b310e-os/apps/Duke 3D'


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--grp', type=Path, required=True, help='DUKE3D.GRP')
    parser.add_argument('--qemu', type=Path, default=REPO / 'build/qemu/build/qemu-system-arm.exe')
    parser.add_argument('--build', type=Path, default=REPO / 'build/os')
    args = parser.parse_args()
    out = (args.build / 'test-duke').resolve()
    out.mkdir(parents=True, exist_ok=True)
    elf, image = args.build / 'b310e-os.elf', args.build / 'b310e-os.bin'
    base = symbol(elf, 'os_status')
    apps_list = symbol(elf, 'apps')
    kit = symbol(args.build / 'ports/duke3d.elf', 'kit')
    numsectors = symbol(args.build / 'ports/duke3d.elf', 'numsectors')
    headspritestat = symbol(args.build / 'ports/duke3d.elf', 'headspritestat')
    sprite = symbol(args.build / 'ports/duke3d.elf', 'sprite')
    card = out / 'card.img'
    subprocess.run([sys.executable, str(REPO / 'ports/rockbox/tests/make-sd-image.py'),
                    '--fat32', '--size-mib', '64', '--seconds', '1', '--force',
                    '--runtime', str(REPO / 'sdcard/.rockbox'),
                    '--rockbox', str(REPO / 'sdcard/payloads/rockbox.bin'),
                    '--add', f'{FOLDER[1:]}/Duke 3D.app={(args.build / "ports/duke3d.app").resolve()}',
                    '--add', f'{FOLDER[1:]}/duke3d.grp={args.grp.resolve()}',
                    '--output', str(card)], check=True, capture_output=True)
    qmp_port, qtest_port = free_port(), free_port()
    cmd = [str(args.qemu.resolve()), '-M', 'b310e,boot-mode=rockbox',
           '-display', os.environ.get('B310E_QEMU_DISPLAY', 'none'),
           *os.environ.get('B310E_QEMU_AUDIO', '').split(),
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
        key('ret', 'Duke 3D', wait=0.5)
        wait_for(qt, base, lambda s: s['apps_loaded'] == 1 and frames() > 0, 90, 'no frames')
        result['first_frames_at'] = time.time()
        time.sleep(3)
        result['logo_colours'] = colours(shot('duke-logo'))
        assert result['logo_colours'] > 8, result
        def in_level():                                       # a map is in: its sectors
            app = qt.read(apps_list)
            return app and qt.read(qt.read(app + 8) + numsectors) & 0xffff

        for _ in range(40):                                   # the logos, New Game, episode, skill
            if in_level():
                break
            key('ret', wait=1.5)
        shot('duke-menu')
        result['sectors'] = in_level()
        assert result['sectors'], result
        time.sleep(8)
        for _ in range(3):                                    # "press a key" at a level's start
            key('ret', wait=1.0)
        time.sleep(4)
        before = shot('duke-game')
        key('up', wait=1.0, hold=1500)
        after = shot('duke-walked')
        from PIL import ImageChops
        result['view_changed'] = ImageChops.difference(picture(before), picture(after)).getbbox() is not None
        result['game_colours'] = colours(after)
        result['frames'] = frames()
        assert result['view_changed'] and result['game_colours'] > 8, result

        app = qt.read(qt.read(apps_list) + 8)                 # dying: sprite[player].extra = 0
        player = qt.read(app + headspritestat + 2 * 10) & 0xffff
        extra = app + sprite + player * 44 + 40
        qt.write(extra, qt.read(extra) & 0xffff)
        died_at = frames()
        for n in range(8):
            time.sleep(1)
            assert frames() > died_at + 10, ('no frames while dying', n, died_at, frames())
            died_at = frames()
        shot('duke-dead')
        key('f1', wait=5)                                     # LSoft: open, the level again
        died_at = frames()
        for n in range(4):
            time.sleep(1)
            assert frames() > died_at + 10, ('no frames after dying', n, died_at, frames())
            died_at = frames()
        shot('duke-died')
        result['frames_after_dying'] = frames()

        key('esc', wait=1.5)                                  # END: F10, quit?
        shot('duke-quit')
        key('f1')                                             # LSoft: Y
        wait_for(qt, base, lambda s: string(qt, s['ui_title']) == 'Apps', 30, 'not back in Apps')
        wait_for(qt, base, lambda s: s['apps_loaded'] == 0, 10, 'still loaded')
        time.sleep(1.5)
        result['heap_after'] = status(qt, base)['heap_free']
        assert abs(result['heap_after'] - result['heap_before']) < 16 * 1024, result

        def start_duke():
            key('ret', 'Duke 3D', wait=0.5)
            wait_for(qt, base, lambda s: s['apps_loaded'] == 1 and frames() > 0, 90, 'no frames')
            time.sleep(3)

        def end_held(why):
            qmp.hmp('sendkey esc 3500')                       # past the 2 s, not the 8 s
            wait_for(qt, base, lambda s: s['apps_loaded'] == 0, 10, f'{why}: still loaded')
            time.sleep(3)
            s = status(qt, base)
            assert string(qt, s['ui_title']) == 'Apps', (why, string(qt, s['ui_title']))
            result[why] = s['heap_free']
            assert abs(s['heap_free'] - result['heap_before']) < 16 * 1024, result

        start_duke()
        end_held('heap_after_asked')
        start_duke()
        app = qt.read(qt.read(apps_list) + 8)                 # its thread never comes back
        hang = symbol(args.build / 'ports/duke3d.elf', 'handleevents')
        qt.write(app + hang, 0xe7fee7fe)                      # (Thumb: b .)
        time.sleep(0.5)
        hung_at = frames()
        time.sleep(1)
        assert frames() == hung_at, ('still drawing', hung_at, frames())
        end_held('heap_after_killed')
        start_duke()
        end_held('heap_after_again')

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

    text = (card_file(card.read_bytes(), '/b310e-os/log.txt') or b'').decode(errors='replace')
    result['log'] = [l.strip() for l in text.splitlines() if 'duke:' in l][-30:]
    print(json.dumps(result, indent=2))
    assert text.count('duke: ended (0)') == 3, (text.count('duke: ended (0)'), result)
    assert text.count('app: Duke 3D killed') == 1, (text.count('app: Duke 3D killed'), result)
    assert sum('Loaded map hack' in l for l in text.splitlines()) >= 2, 'the level did not restart'
    print('test-duke: PASS')


if __name__ == '__main__':
    main()
