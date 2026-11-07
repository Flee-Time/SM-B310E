#!/usr/bin/env python3
"""Heretic or Hexen (os/ports/chocolate: upstream Chocolate Doom's, on the
game kit) in QEMU, from Apps; test-doom.py goes further with Doom.

The card has <game>.app and the WAD in /b310e-os/apps/<Name>. Apps ->
<Name>: the title and its demo show (frames counted by the kit, the
game's colours in a screenshot). RSoft opens the menu; Center (a
gamepad's fire there) three times starts a game (New game, the episode
or the class, the skill; once more if it asks one more thing); Right
held walks: the view moves. END asks to quit, LSoft says yes: back in
Apps, the app unloaded, the heap back where it was, "<game>: ended (0)"
in the log, the game's settings beside the WAD.

The WADs are the user's (not in the repository):

  python os/tests/test-chocolate.py --game heretic --wad path/to/heretic1.wad
  python os/tests/test-chocolate.py --game hexen --wad path/to/hexen.wad

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
GS_LEVEL, GS_DEMOSCREEN = 0, 3
GAMES = {'heretic': ('Heretic', 'heretic.cfg'), 'hexen': ('Hexen', 'hexen.cfg')}


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--game', choices=sorted(GAMES), required=True)
    parser.add_argument('--wad', type=Path, required=True)
    parser.add_argument('--qemu', type=Path, default=REPO / 'build/qemu/build/qemu-system-arm.exe')
    parser.add_argument('--build', type=Path, default=REPO / 'build/os')
    args = parser.parse_args()
    name, cfg = GAMES[args.game]
    folder = f'/b310e-os/apps/{name}'
    if not args.wad.is_file():
        sys.exit(f'test-chocolate: no WAD at {args.wad}')
    out = (args.build / f'test-{args.game}').resolve()
    out.mkdir(parents=True, exist_ok=True)
    elf, image = args.build / 'b310e-os.elf', args.build / 'b310e-os.bin'
    game_elf, game_app = args.build / f'ports/{args.game}.elf', args.build / f'ports/{args.game}.app'
    base = symbol(elf, 'os_status')
    apps_list = symbol(elf, 'apps')
    offs = {n: symbol(game_elf, n) for n in
            ('kit', 'gamestate', 'usergame', 'demoplayback', 'viewx', 'viewy')}
    card = out / 'card.img'
    subprocess.run([sys.executable, str(REPO / 'ports/rockbox/tests/make-sd-image.py'),
                    '--fat32', '--size-mib', '64', '--seconds', '1', '--force',
                    '--runtime', str(REPO / 'sdcard/.rockbox'),
                    '--rockbox', str(REPO / 'sdcard/payloads/rockbox.bin'),
                    '--add', f'{folder[1:]}/{args.game}.app={game_app.resolve()}',
                    '--add', f'{folder[1:]}/{args.wad.name.lower()}={args.wad.resolve()}',
                    '--output', str(card)], check=True, capture_output=True)
    qmp_port, qtest_port = free_port(), free_port()
    cmd = [str(args.qemu.resolve()), '-M', 'b310e,boot-mode=rockbox',
           '-display', 'none', '-serial', 'none', '-monitor', 'none',
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

        def key(k, want=None, wait=0.35, hold=100):
            qmp.hmp(f'sendkey {k} {200 if k == "esc" else hold}')
            if want:
                wait_for(qt, base, lambda s: string(qt, s['ui_title']) == want, 8, f'not {want}')
            time.sleep(wait)

        def shot(label):
            path = out / f'{label}.png'
            qmp.command('screendump', {'filename': str(path), 'format': 'png'})
            time.sleep(0.3)
            return path

        def var(n, at=0, size=4):
            app = qt.read(apps_list)
            assert app, f'{name} is not loaded'
            return qt.read(qt.read(app + 8) + offs[n] + at) & ((1 << 8 * size) - 1)

        def flag(n):
            return var(n, size=1)

        def until(test, seconds, what):
            end = time.monotonic() + seconds
            while time.monotonic() < end:
                if test():
                    return
                time.sleep(0.2)
            raise AssertionError(f'{what}: {json.dumps(result)}')

        def colours(path):
            from PIL import Image
            return len(Image.open(path).convert('RGB').getcolors(1 << 16) or [])

        wait_for(qt, base, lambda s: s['magic'] == MAGIC, 10, 'OS never started')
        wait_for(qt, base, lambda s: s['ui_depth'] == 1, 60, 'no UI')
        time.sleep(1)
        result['heap_before'] = status(qt, base)['heap_free']

        key('esc', wait=0.5)
        key('ret', 'Menu')
        key('ret', 'Apps')
        key('ret', name, wait=0.5)
        wait_for(qt, base, lambda s: s['apps_loaded'] == 1, 10, f'{name} not loaded')
        t0 = time.monotonic()
        until(lambda: var('kit') > 0, 180, 'no first frame')
        result['first_frame_s'] = round(time.monotonic() - t0, 1)
        until(lambda: var('kit') > 60, 180, 'the frames stopped')
        time.sleep(3)
        result['title_colours'] = colours(shot(f'{args.game}-title'))
        result['gamestate_title'] = flag('gamestate')
        assert result['title_colours'] > 16, result
        assert result['gamestate_title'] in (GS_LEVEL, GS_DEMOSCREEN), result

        key('f2', wait=1.0)
        shot(f'{args.game}-menu')
        for _ in range(3):
            key('ret', wait=1.2)
        if not (flag('gamestate') == GS_LEVEL and flag('usergame')):
            key('ret', wait=1.5)                              # one more question
        until(lambda: flag('gamestate') == GS_LEVEL and flag('usergame')
              and not flag('demoplayback'), 30, 'no new game')
        time.sleep(2)
        x0, y0 = var('viewx'), var('viewy')
        key('right', wait=1.0, hold=1500)
        x1, y1 = var('viewx'), var('viewy')
        result['walked'] = [x0, y0, x1, y1]
        assert (x0, y0) != (x1, y1), result
        result['game_colours'] = colours(shot(f'{args.game}-level'))

        key('esc', wait=1.0)                                  # END: quit?
        shot(f'{args.game}-quit')
        key('f1')                                             # LSoft: yes
        wait_for(qt, base, lambda s: string(qt, s['ui_title']) == 'Apps', 20, 'not back in Apps')
        wait_for(qt, base, lambda s: s['apps_loaded'] == 0, 10, f'{name} still loaded')
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
    log = (card_file(img, '/b310e-os/log.txt') or b'').decode(errors='replace')
    result['log'] = [l.strip() for l in log.splitlines() if f'{args.game}:' in l][:40]
    result['config'] = bool(card_file(img, f'{folder}/{cfg}'))
    print(json.dumps(result, indent=2))
    assert any(f'{args.game}: ended (0)' in l for l in result['log']), result
    assert result['config'], result
    print(f'test-chocolate {args.game}: PASS')


if __name__ == '__main__':
    main()
