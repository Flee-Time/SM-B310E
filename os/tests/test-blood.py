#!/usr/bin/env python3
"""Blood (os/ports/blood: NBlood's game code on the shared Build engine, os/ports/build)
in QEMU, from Apps.

The card has the app and Blood's v1.21 files (its .RFF, .ART, .DAT and .INI files) in
/b310e-os/apps/Blood. Apps -> Blood: the logos (frames counted by the kit, a screenshot
with their colours). Center (Enter) goes through them and the menus to a new game: the
first map is in (E1M1's 155 sectors); Up held walks: the view changes. END held closes
it: back in Apps, the app unloaded, the heap back where it was, "blood: ended (0)" in
the log.

The data is the user's (not in the repository): the folder of One Unit Whole Blood
(v1.21, GOG's has it):

  python os/tests/test-blood.py --data path/to/blood

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
FOLDER = '/b310e-os/apps/Blood'
E1M1_SECTORS = 155                                    # the first map's
KINDS = ('.RFF', '.ART', '.DAT', '.INI', '.MAP', '.AR_')



def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--data', type=Path, required=True, help="Blood's folder (v1.21)")
    parser.add_argument('--qemu', type=Path, default=REPO / 'build/qemu/build/qemu-system-arm.exe')
    parser.add_argument('--build', type=Path, default=REPO / 'build/os')
    args = parser.parse_args()
    out = (args.build / 'test-blood').resolve()
    out.mkdir(parents=True, exist_ok=True)
    elf, image = args.build / 'b310e-os.elf', args.build / 'b310e-os.bin'
    base = symbol(elf, 'os_status')
    apps_list = symbol(elf, 'apps')
    kit = symbol(args.build / 'ports/blood.elf', 'kit')
    numsectors = symbol(args.build / 'ports/blood.elf', 'numsectors')
    card = out / 'card.img'
    files = [f for f in sorted(args.data.iterdir()) if f.is_file() and f.suffix.upper() in KINDS]
    adds = [a for f in files for a in ('--add', f'{FOLDER[1:]}/{f.name}={f.resolve()}')]
    subprocess.run([sys.executable, str(REPO / 'ports/rockbox/tests/make-sd-image.py'),
                    '--fat32', '--size-mib', '128', '--seconds', '1', '--force',
                    '--runtime', str(REPO / 'sdcard/.rockbox'),
                    '--rockbox', str(REPO / 'sdcard/payloads/rockbox.bin'),
                    '--add', f'{FOLDER[1:]}/Blood.app={(args.build / "ports/blood.app").resolve()}',
                    *adds,
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
        key('ret', 'Blood', wait=0.5)
        wait_for(qt, base, lambda s: s['apps_loaded'] == 1 and frames() > 0, 90, 'no frames')
        result['first_frames_at'] = time.time()
        time.sleep(3)
        result['logo_colours'] = colours(shot('blood-logo'))
        assert result['logo_colours'] > 8, result
        def in_level():                                       # the first map (E1M1)
            app = qt.read(apps_list)
            return app and qt.read(qt.read(app + 8) + numsectors) & 0xffff == E1M1_SECTORS

        for _ in range(40):                                   # the logos, New Game, episode, skill
            if in_level():
                break
            key('ret', wait=1.5, hold=300)
        shot('blood-menu')
        result['sectors'] = in_level()
        assert result['sectors'], result
        def playing():                                        # loaded: frames come again
            f0 = frames()
            time.sleep(1)
            return frames() - f0 > 10
        for _ in range(60):
            if playing():
                break
        assert playing(), ('the level never came', frames())
        time.sleep(2)
        before = shot('blood-game')
        key('up', wait=1.0, hold=1500)
        after = shot('blood-walked')
        from PIL import ImageChops
        result['view_changed'] = ImageChops.difference(picture(before), picture(after)).getbbox() is not None
        result['game_colours'] = colours(after)
        result['frames'] = frames()
        assert result['view_changed'] and result['game_colours'] > 8, result

        qmp.hmp('sendkey esc 3500')                           # END held: it closes
        wait_for(qt, base, lambda s: s['apps_loaded'] == 0, 15, 'still loaded')
        time.sleep(3)
        s = status(qt, base)
        assert string(qt, s['ui_title']) == 'Apps', string(qt, s['ui_title'])
        result['heap_after'] = s['heap_free']
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

    text = (card_file(card.read_bytes(), '/b310e-os/log.txt') or b'').decode(errors='replace')
    result['log'] = [l.strip() for l in text.splitlines() if 'blood:' in l][-30:]
    print(json.dumps(result, indent=2))
    assert 'blood: ended (0)' in text, result
    print('test-blood: PASS')


if __name__ == '__main__':
    main()
