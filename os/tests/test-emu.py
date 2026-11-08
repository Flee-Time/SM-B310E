#!/usr/bin/env python3
"""An emulator app (os/ports: on the game kit's front end) in QEMU.

The card has the app and a ROM in the app's own folder. Apps -> the app:
the list of ROMs. LSoft opens the ROM's options: Right changes the first
(the screen's turn), Back keeps them (<rom>.opt beside the ROM). Center
plays: the app loads again with the ROM (app_reload) and the game runs
(frames counted by the kit, the game's colours in a screenshot, the
keys pressed reach it). END quits: the app loads again with the list,
the cursor on the ROM. RSoft leaves: the app is unloaded, the heap is
back where it was. The log says how it went.

The ROMs are the user's (not in the repository):

  python os/tests/test-emu.py --app gnuboy --rom "path/to/game.gbc"

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
APPS = {'gnuboy': ('Game Boy', 'gnuboy'), 'infones': ('NES', 'infones')}


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--app', choices=sorted(APPS), required=True)
    parser.add_argument('--rom', type=Path, required=True)
    parser.add_argument('--seconds', type=float, default=8, help='how long the game runs')
    parser.add_argument('--flash-shots', type=int, default=150, help='screens looked at for a stale one')
    parser.add_argument('--qemu', type=Path, default=REPO / 'build/qemu/build/qemu-system-arm.exe')
    parser.add_argument('--build', type=Path, default=REPO / 'build/os')
    args = parser.parse_args()
    name, log = APPS[args.app]
    folder = f'/b310e-os/apps/Emulators/{name}'         # a category, with its own icon
    rom_name = args.rom.stem
    out = (args.build / f'test-{args.app}').resolve()
    out.mkdir(parents=True, exist_ok=True)
    info = out / 'category.info'                          # its name, and a square for an icon
    info.write_text('name Retro games\n' + 'icon ffffffffffff\n' + 'icon f0000000000f\n' * 10 +
                    'icon ffffffffffff\n', newline='\n')
    elf, image = args.build / 'b310e-os.elf', args.build / 'b310e-os.bin'
    app_elf, app_file = args.build / f'ports/{args.app}.elf', args.build / f'ports/{args.app}.app'
    base = symbol(elf, 'os_status')
    apps_list = symbol(elf, 'apps')
    kit = symbol(app_elf, 'kit')
    card = out / 'card.img'
    subprocess.run([sys.executable, str(REPO / 'ports/rockbox/tests/make-sd-image.py'),
                    '--fat32', '--size-mib', '64', '--seconds', '1', '--force',
                    '--runtime', str(REPO / 'sdcard/.rockbox'),
                    '--rockbox', str(REPO / 'sdcard/payloads/rockbox.bin'),
                    '--add', f'{folder[1:]}/{name}.app={app_file.resolve()}',
                    '--add', f'b310e-os/apps/Emulators/.info={info.as_posix()}',
                    '--add', f'{folder[1:]}/{args.rom.name}={args.rom.resolve()}',
                    '--output', str(card)], check=True, capture_output=True)
    qmp_port, qtest_port = free_port(), free_port()
    cmd = [str(args.qemu.resolve()), '-M', 'b310e,boot-mode=rockbox',
           '-display', os.environ.get('B310E_QEMU_DISPLAY', 'none'), *os.environ.get('B310E_QEMU_AUDIO', '').split(), '-serial', 'none', '-monitor', 'none',
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

        def title():
            return string(qt, status(qt, base)['ui_title'])

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
        shot('apps')
        key('ret', 'Retro games')                             # the category
        key('ret', name, wait=0.5)                            # the list of ROMs
        shot(f'{args.app}-list')
        key('f1', rom_name)                                   # its options
        key('right', wait=0.3)                                # Screen: the next turn
        shot(f'{args.app}-options')
        key('f2', name)                                       # back: kept
        key('ret', wait=0.5)                                  # play: a fresh app with it
        wait_for(qt, base, lambda s: s['apps_loaded'] == 1 and frames() > 0, 60, 'no frames')
        time.sleep(args.seconds / 2)
        for k in ('kp_enter', 'ret', 'ret', 'ret'):           # Start, then A a few times
            key(k, wait=0.8)
        time.sleep(args.seconds / 2)
        result['frames'] = frames()
        result['colours'] = colours(shot(f'{args.app}-game'))
        # the game's frames only: never, for a moment, the menu the game replaced
        from PIL import Image, ImageChops
        menu = Image.open(out / f'{args.app}-list.png').convert('RGB')
        flash = out / 'flash.png'
        result['menu_flashes'] = 0
        for _ in range(args.flash_shots):
            qmp.command('screendump', {'filename': str(flash), 'format': 'png'})
            now = Image.open(flash).convert('RGB')
            same = ImageChops.difference(menu, now).convert('L').point(lambda v: 255 if v == 0 else 0).histogram()[255]
            if same > menu.width * menu.height // 3:
                result['menu_flashes'] += 1
                now.save(out / 'menu-flash.png')
        assert not result['menu_flashes'], result
        assert result['frames'] > 60 and result['colours'] >= 2, result     # not blank

        key('esc', 'Leave?', wait=0.5)                        # END asks, the game waits
        shot(f'{args.app}-leave')
        paused = frames()
        time.sleep(1.0)
        result['paused'] = frames() == paused
        key('f2', wait=1.0)                                   # RSoft: play on
        result['played_on'] = frames() > paused
        assert result['paused'] and result['played_on'], result
        key('esc', 'Leave?', wait=0.5)
        seen = set()
        qmp.hmp('sendkey esc 100')                            # END again: leave
        deadline = time.time() + 5                            # no other screen on the way
        while time.time() < deadline:
            seen.add(title())
            time.sleep(0.01)
        result['on_the_way'] = sorted(seen)
        result['back_in_list'] = title()
        assert result['back_in_list'] == name and 'Apps' not in seen, result
        wait_for(qt, base, lambda s: s['apps_loaded'] == 1, 5, 'no list app')
        key('f2', 'Retro games', wait=0.5)                    # Back: out of the app
        key('f2', 'Apps', wait=0.5)                           # and of the category
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
    result['log'] = [l.strip() for l in text.splitlines() if f'{log}:' in l or 'app:' in l][-30:]
    opt = card_file(img, f'{folder}/{rom_name}.opt')
    result['options'] = opt.decode(errors='replace').split('\n')[0] if opt else None
    print(json.dumps(result, indent=2))
    assert result['options'] == 'Screen 2', result            # sideways (1) + Right
    assert any(f'{log}: ended (0)' in l for l in result['log']), result
    print(f'test-emu {args.app}: PASS')


if __name__ == '__main__':
    main()
