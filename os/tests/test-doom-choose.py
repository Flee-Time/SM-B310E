#!/usr/bin/env python3
"""Doom's list of games (os/ports/chocolate/chocolate_os.c) in QEMU.

The card has Doom.app in /b310e-os/apps/Games/Doom with DOOM.WAD, DOOM2.WAD and a
PWAD with a DeHackEd file of its name (Sandy's Cities: sandy.wad, sandy.deh), and a
Doom 1 save (doomsav0.dsg) beside them. Apps -> Games -> Doom opens on a list of
three: Doom, Doom II, Sandy. Each one picked loads the app again and its title
screen runs (frames counted, Doom's demo state). Doom II starts a new game and
saves in slot 0: the save goes to doom2/ (with doom2/default.cfg), not over Doom 1's.
Opened again, the list has the last one picked selected (the store's
"app/Doom/last"). Sandy runs with -merge sandy.wad -deh sandy.deh on DOOM2.WAD.
Doom (DOOM.WAD) runs as before: its save, in the folder itself, unchanged.

The WADs are the user's (not in the repository):

  python os/tests/test-doom-choose.py --wads E:/b310e-os/apps/Games/Doom \
      --doom2 E:/games/doom2 [--save path/to/doomsav0.dsg]

SPDX-License-Identifier: GPL-2.0-or-later
"""
import argparse
import hashlib
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
symbol, status, string, wait_for, free_port, card_file, store_keys = (
    boot[n] for n in ('symbol', 'status', 'string', 'wait_for', 'free_port', 'card_file',
                      'store_keys'))
MAGIC = boot['MAGIC']
GS_LEVEL, GS_DEMOSCREEN = 0, 3
DIR = '/b310e-os/apps/Games/Doom'


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--qemu', type=Path, default=REPO / 'build/qemu/build/qemu-system-arm.exe')
    parser.add_argument('--build', type=Path, default=REPO / 'build/os')
    parser.add_argument('--wads', type=Path, required=True, help='the folder with DOOM.WAD')
    parser.add_argument('--doom2', type=Path, required=True,
                        help='the folder with DOOM2.WAD, sandy.wad, sandy.deh')
    parser.add_argument('--save', type=Path, help="a Doom 1 save (doomsav0.dsg) to put beside")
    args = parser.parse_args()
    files = {'DOOM.WAD': args.wads / 'DOOM.WAD', 'DOOM2.WAD': args.doom2 / 'DOOM2.WAD',
             'sandy.wad': args.doom2 / 'sandy.wad', 'sandy.deh': args.doom2 / 'sandy.deh'}
    if args.save:
        files['doomsav0.dsg'] = args.save
    for f in files.values():
        if not f.is_file():
            sys.exit(f'test-doom-choose: no {f}')
    out = (args.build / 'test-doom-choose').resolve()
    out.mkdir(parents=True, exist_ok=True)
    elf, image = args.build / 'b310e-os.elf', args.build / 'b310e-os.bin'
    doom_elf, doom_app = args.build / 'ports/doom.elf', args.build / 'ports/doom.app'
    base = symbol(elf, 'os_status')
    apps_list = symbol(elf, 'apps')
    offs = {n: symbol(doom_elf, n) for n in
            ('kit', 'gamestate', 'usergame', 'demoplayback', 'game_list')}
    card = out / 'card.img'
    adds = ['--add', f'{DIR[1:]}/Doom.app={doom_app.resolve()}']
    for name, f in files.items():
        adds += ['--add', f'{DIR[1:]}/{name}={f.resolve()}']
    subprocess.run([sys.executable, str(REPO / 'ports/rockbox/tests/make-sd-image.py'),
                    '--fat32', '--size-mib', '128', '--seconds', '1', '--force',
                    '--runtime', str(REPO / 'sdcard/.rockbox'),
                    '--rockbox', str(REPO / 'sdcard/payloads/rockbox.bin'),
                    *adds, '--output', str(card)], check=True, capture_output=True)
    qmp_port, qtest_port = free_port(), free_port()
    cmd = [str(args.qemu.resolve()), '-M', 'b310e,boot-mode=rockbox',
           '-display', os.environ.get('B310E_QEMU_DISPLAY', 'none'), '-serial', 'none',
           '-monitor', 'none',
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
                wait_for(qt, base, lambda s: string(qt, s['ui_title']) == want, 10, f'not {want}')
            time.sleep(wait)

        def shot(label):
            path = out / f'{label}.png'
            qmp.command('screendump', {'filename': str(path), 'format': 'png'})
            time.sleep(0.3)
            return path

        def var(n, at=0, size=4):
            app = qt.read(apps_list)
            assert app, 'Doom is not loaded'
            return qt.read(qt.read(app + 8) + offs[n] + at) & ((1 << 8 * size) - 1)

        def flag(n):
            return var(n, size=1)

        def until(test, seconds, what):
            end = time.monotonic() + seconds
            while time.monotonic() < end:
                try:
                    if test():
                        return
                except AssertionError:
                    pass                # (between the app and its reload)
                time.sleep(0.2)
            raise AssertionError(f'{what}: {json.dumps(result)}')

        def open_doom():
            """Games -> Doom: the list (count, the one selected)"""
            key('ret', 'Doom', wait=0.8)
            wait_for(qt, base, lambda s: s['apps_loaded'] == 1, 10, 'Doom not loaded')
            until(lambda: var('game_list') == 3, 10, 'no list of three')
            return var('game_list'), var('game_list', 4)

        def runs(label):
            """the game picked: the app again, its title screen and demo"""
            until(lambda: var('game_list') == 0 and var('kit') > 60, 240, f'{label}: no frames')
            time.sleep(2)
            shot(f'{label}-title')
            result[f'{label}_state'] = flag('gamestate')
            assert result[f'{label}_state'] in (GS_LEVEL, GS_DEMOSCREEN), result

        def quit_game():
            key('esc', wait=1.0)        # END: quit?
            key('f1')                   # LSoft: yes
            wait_for(qt, base, lambda s: string(qt, s['ui_title']) == 'Games', 30,
                     'not back in Games')
            wait_for(qt, base, lambda s: s['apps_loaded'] == 0, 15, 'Doom still loaded')
            time.sleep(1)

        wait_for(qt, base, lambda s: s['magic'] == MAGIC, 10, 'OS never started')
        wait_for(qt, base, lambda s: s['ui_depth'] == 1, 60, 'no UI')
        time.sleep(1)
        key('esc', wait=0.5)
        key('ret', 'Menu')
        key('ret', 'Apps')
        key('ret', 'Games', wait=0.5)

        # ---- the list: Doom selected (nothing picked yet); Doom II
        result['list_1'] = open_doom()
        shot('list-1')
        assert result['list_1'] == (3, 0), result
        key('down')
        key('ret', wait=1.0)
        runs('doom2')
        # a new game (New game, the skill), saved in slot 0
        key('f2', wait=1.0)
        for _ in range(2):
            key('ret', wait=1.2)
        if not (flag('gamestate') == GS_LEVEL and flag('usergame')):
            key('ret', wait=1.5)
        until(lambda: flag('gamestate') == GS_LEVEL and flag('usergame')
              and not flag('demoplayback'), 30, 'doom2: no new game')
        time.sleep(2)
        shot('doom2-map01')
        key('f2', wait=0.8)
        for _ in range(3):
            key('left', wait=0.3)
        key('ret', wait=0.8)            # Save game
        key('ret', wait=0.8)            # slot 0
        key('ret', wait=2.0)            # saved
        quit_game()

        # ---- again: Doom II selected; Sandy
        result['list_2'] = open_doom()
        assert result['list_2'] == (3, 1), result
        key('down')
        key('ret', wait=1.0)
        runs('sandy')
        quit_game()

        # ---- again: Sandy selected; Doom
        result['list_3'] = open_doom()
        assert result['list_3'] == (3, 2), result
        key('up')
        key('up')
        key('ret', wait=1.0)
        runs('doom')
        quit_game()

        key('f2', 'Apps')
        key('f2', 'Menu')
        for _ in range(5):
            key('down')
        key('ret', 'Power')
        key('ret', 'Power off')
        qmp.hmp('sendkey f1 100')
        p.wait(timeout=20)
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
    log = (card_file(img, '/b310e-os/log') or b'').decode(errors='replace')
    result['log'] = [l.strip() for l in log.splitlines()
                     if 'doom:' in l and ('adding' in l or 'merging' in l or 'Games/Doom' in l
                                          or 'ended' in l or '.deh' in l)][-40:]
    result['doom2_save'] = len(card_file(img, f'{DIR}/doom2/doomsav0.dsg') or b'')
    result['doom2_cfg'] = card_file(img, f'{DIR}/doom2/default.cfg') is not None
    result['sandy_cfg'] = card_file(img, f'{DIR}/sandy/default.cfg') is not None
    result['root_cfg'] = card_file(img, f'{DIR}/default.cfg') is not None
    root_save = card_file(img, f'{DIR}/doomsav0.dsg')
    if args.save:
        result['root_save_kept'] = (root_save is not None and hashlib.sha256(root_save).digest()
                                    == hashlib.sha256(args.save.read_bytes()).digest())
    else:
        result['root_save_absent'] = root_save is None
    result['last'] = store_keys(img, 'app/Doom/')
    print(json.dumps(result, indent=2))
    assert sum('doom: ended (0)' in l for l in result['log']) == 3, result
    assert any('merging sandy.wad' in l for l in result['log']), result
    assert any('loading sandy.deh' in l for l in result['log']), result
    assert result['doom2_save'] > 1000 and result['doom2_cfg'], result
    assert result['sandy_cfg'] and result['root_cfg'], result
    assert result.get('root_save_kept', result.get('root_save_absent')), result
    print('test-doom-choose: PASS')


if __name__ == '__main__':
    main()
