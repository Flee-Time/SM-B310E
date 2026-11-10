#!/usr/bin/env python3
"""Doom (os/ports/chocolate: upstream Chocolate Doom as doom.app, on the
game kit, os/ports/kit) in QEMU, from Apps.

The card has doom.app and a WAD in a folder of the app's own,
/b310e-os/apps/Doom (API 2.6: app_dir; the Apps list looks into the
folders in /b310e-os/apps). Apps -> Doom loads it: a full-screen,
raw-key screen, the game in a thread of the app's. The title screen and
its demo show (frames counted by the kit, a screenshot with Doom's
colours, sound in the captured audio). RSoft opens Doom's menu; Center
three times (New game, the episode, the skill) starts E1M1; Right held
(sideways, it points ahead) walks: the view moves.

Saved in slot 0 from the menu (Center is a gamepad's fire there, so the
empty slot is named after the map), walked on and loaded again: the
view is back where it was saved (doomsav0.dsg beside the WAD, no
temp.dsg left). Then 7 quick-saves (a slot the first time, after that a
question LSoft says yes to), 9 quick-loads (LSoft: yes): the view is
back at the quick save. * + # together opens the quick panel, on Home
and over the game: Right held meanwhile is let go for the game (the view
stops), * + # again closes it. END asks to quit, LSoft says yes: the
game saves its settings, the screen leaves, the app is unloaded once
its thread ends, and the heap is back where it was. The OS log says how
it went.

Memory of an app's threads: the game's thread takes its arena (the
loader lists the block as Doom's, freed when it unloads) and frees the
frame the UI thread took (off Doom's list: not freed twice). Either one
wrong and the heap is not back where it was.

The WAD is the user's (not in the repository): --wad, $B310E_DOOM_WAD or
sdcard/games/doom1/doom1.wad. Shareware doom1.wad 1.666 and 1.9 work.

  python os/tests/test-doom.py --wad path/to/doom1.wad

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
DIR = '/b310e-os/apps/Doom'             # the app's own folder: doom.app, the WAD, saves


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qemu', type=Path, default=REPO / 'build/qemu/build/qemu-system-arm.exe')
    parser.add_argument('--build', type=Path, default=REPO / 'build/os')
    parser.add_argument('--wad', type=Path, default=Path(os.environ.get(
        'B310E_DOOM_WAD', REPO / 'sdcard/games/doom1/doom1.wad')))
    parser.add_argument('--dark', action='store_true',
                        help="left alone, the game's screen goes dark and it stops; a key goes on")
    args = parser.parse_args()
    if not args.wad.is_file():
        sys.exit(f'test-doom: no WAD at {args.wad} (use --wad)')
    out = (args.build / ('test-doom-dark' if args.dark else 'test-doom')).resolve()
    out.mkdir(parents=True, exist_ok=True)
    extra = []
    if args.dark:
        settings = out / 'settings.txt'
        settings.write_text('screen_timeout=5\nawake_limit_s=20\n', newline='\n')
        extra = ['--add', f'b310e-os/settings.txt={settings}']
    elf, image = args.build / 'b310e-os.elf', args.build / 'b310e-os.bin'
    doom_elf, doom_app = args.build / 'ports/doom.elf', args.build / 'ports/doom.app'
    base = symbol(elf, 'os_status')
    apps_list = symbol(elf, 'apps')
    # Doom is linked at 0: its variables are at the loaded image (the
    # loader's first app, app_t.image at +8) plus their offsets
    offs = {n: symbol(doom_elf, n) for n in
            ('kit', 'gamestate', 'gametic', 'menuactive', 'usergame',
             'demoplayback', 'viewx', 'viewy')}
    card = out / 'card.img'
    subprocess.run([sys.executable, str(REPO / 'ports/rockbox/tests/make-sd-image.py'),
                    '--fat32', '--size-mib', '64', '--seconds', '1', '--force',
                    '--runtime', str(REPO / 'sdcard/.rockbox'),
                    '--rockbox', str(REPO / 'sdcard/payloads/rockbox.bin'),
                    '--add', f'{DIR[1:]}/doom.app={doom_app.resolve()}',
                    '--add', f'{DIR[1:]}/{args.wad.name.lower()}={args.wad.resolve()}', *extra,
                    '--output', str(card)], check=True, capture_output=True)
    wav_out = out / 'audio.wav'
    wav_out.unlink(missing_ok=True)
    # heard as well (B310E_QEMU_AUDIO): the same sound still captured, by the monitor
    audio = os.environ.get('B310E_QEMU_AUDIO', '').split()
    qmp_port, qtest_port = free_port(), free_port()
    cmd = [str(args.qemu.resolve()), '-M', 'b310e,boot-mode=rockbox',
           '-display', os.environ.get('B310E_QEMU_DISPLAY', 'none'), *audio, '-serial', 'none', '-monitor', 'none',
           '-drive', f'file={image.resolve().as_posix()},format=raw,if=none,id=os,readonly=on',
           '-drive', f'file={card.as_posix()},format=raw,if=none,id=sdcard',
           '-qmp', f'tcp:127.0.0.1:{qmp_port},server=on,wait=off',
           '-qtest', f'tcp:127.0.0.1:{qtest_port},server=on,wait=off',
           *([] if audio else
             ['-audiodev', f'wav,id=audio0,path={wav_out.as_posix()},out.frequency=44100,'
                           'out.channels=2,out.format=s16',
              '-global', 'sc6530_adi.audiodev=audio0'])]
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
        if audio:
            qmp.hmp(f'wavcapture {wav_out.as_posix()} audio0 44100 16 2')

        def title():
            return string(qt, status(qt, base)['ui_title'])

        def key(k, want=None, wait=0.35, hold=100):
            qmp.hmp(f'sendkey {k} {200 if k == "esc" else hold}')
            if want:
                wait_for(qt, base, lambda s: string(qt, s['ui_title']) == want, 8, f'not {want}')
            time.sleep(wait)

        def shot(name):
            path = out / f'{name}.png'
            qmp.command('screendump', {'filename': str(path), 'format': 'png'})
            time.sleep(0.3)
            return path

        def var(name, at=0, size=4):
            app = qt.read(apps_list)
            assert app, 'Doom is not loaded'
            return qt.read(qt.read(app + 8) + offs[name] + at) & ((1 << 8 * size) - 1)

        def flag(name):                 # enums and booleans: one byte (AAPCS)
            return var(name, size=1)

        def until(test, seconds, what):
            end = time.monotonic() + seconds
            while time.monotonic() < end:
                if test():
                    return
                time.sleep(0.2)
            raise AssertionError(f'{what}: {json.dumps(result)}')

        def settled():
            """The view once the player stops sliding (Doom's momentum
            carries a walk on some 170 units)."""
            last = None
            for _ in range(40):
                now = var('viewx'), var('viewy')
                if now == last:
                    return now
                last = now
                time.sleep(0.5)
            raise AssertionError(f'the view never settled: {json.dumps(result)}')

        def hold(k, down):
            qmp.command('input-send-event', {'events': [
                {'type': 'key', 'data': {'down': down, 'key': {'type': 'qcode', 'data': k}}}]})

        def chord(want):                # * + # (QEMU: minus, slash)
            qmp.hmp('sendkey minus-slash 150')
            wait_for(qt, base, lambda s: string(qt, s['ui_title']) == want, 8, f'* + #: not {want}')
            time.sleep(0.4)

        def colours(path):
            from PIL import Image
            return len(Image.open(path).convert('RGB').getcolors(1 << 16) or [])

        wait_for(qt, base, lambda s: s['magic'] == MAGIC, 10, 'OS never started')
        wait_for(qt, base, lambda s: s['ui_depth'] == 1, 60, 'no UI')
        time.sleep(1)
        result['heap_before'] = status(qt, base)['heap_free']
        home = title()
        chord('Quick panel')                                  # on Home
        chord(home)

        # ---- Apps -> Doom: the title screen and its demo
        key('esc', wait=0.5)
        key('ret', 'Menu')
        key('ret', 'Apps')
        key('ret', 'Doom', wait=0.5)
        wait_for(qt, base, lambda s: s['apps_loaded'] == 1, 10, 'Doom not loaded')
        t0 = time.monotonic()
        until(lambda: var('kit') > 0, 180, 'no first frame')
        result['first_frame_s'] = round(time.monotonic() - t0, 1)
        # the arena, malloc'ed in the game's thread, is on Doom's list
        # (struct app: allocs at +16, n_allocs at +20; kit.arena at +164)
        app = qt.read(apps_list)
        allocs, n_allocs = qt.read(app + 16), qt.read(app + 20)
        arena = var('kit', 164)
        result['allocs'] = n_allocs
        assert arena and arena in [qt.read(allocs + 4 * i) for i in range(n_allocs)], result
        if args.dark:
            # Still lit past the normal timeout: keys count for the limit, the
            # last one was launching it
            s = status(qt, base)
            result['light_at_first_frame'] = s['ui_light']
            frames_before = var('kit')
            until(lambda: var('kit') > frames_before + 10, 60, 'not running')
            # Left alone: dim, then dark
            wait_for(qt, base, lambda s: s['ui_light'] == 2, 90, 'never went dark')
            time.sleep(2)
            f0, gt0 = var('kit'), var('gametic')
            d0 = status(qt, base)
            time.sleep(4)
            d1 = status(qt, base)
            result['frames_dark'] = var('kit') - f0
            result['gametics_dark'] = var('gametic') - gt0
            result['mhz_dark'] = d1['cpu_mhz']
            assert result['frames_dark'] == 0, result
            assert result['gametics_dark'] == 0, result
            until(lambda: status(qt, base)['cpu_mhz'] == 26, 10, 'clock not down while paused')
            result['mhz_dark'] = 26
            # A key: lit, and the game goes on with no jump in its clock
            key('down', wait=0.3, hold=60)
            wait_for(qt, base, lambda s: s['ui_light'] == 0, 5, 'no wake')
            g0 = var('gametic')
            time.sleep(4)
            result['gametics_in_4_s_after'] = var('gametic') - g0
            result['frames_after'] = var('kit') - f0
            assert result['frames_after'] > 5, result
            # 35 tics a second at most, and a jump of the paused time would be far more
            assert 0 < result['gametics_in_4_s_after'] <= 4 * 35 + 10, result
            key('f2', wait=0.5)
            qmp.command('quit')
            p.wait(timeout=20)
            log = (card_file(card.read_bytes(), '/b310e-os/log.txt') or b'').decode(errors='replace')
            lines = [x for x in log.splitlines() if 'the screen is dark' in x or 'going on after' in x]
            result['log'] = lines
            assert any('going on after' in x for x in lines), lines
            print('PASS ' + json.dumps(result))
            return
        until(lambda: var('kit') > 60, 180, 'the frames stopped')
        time.sleep(3)
        result['title_frames'] = var('kit')
        result['gamestate_title'] = flag('gamestate')
        result['error'] = string(qt, qt.read(apps_list) and qt.read(qt.read(apps_list) + 8) + offs['kit'] + 4)
        result['title_colours'] = colours(shot('doom-title'))
        assert result['gamestate_title'] in (GS_LEVEL, GS_DEMOSCREEN), result
        assert result['title_colours'] > 16, result

        # ---- a new game: the menu, New game, the episode, the skill
        key('f2', wait=1.0)
        result['menu'] = flag('menuactive')
        shot('doom-menu')
        for _ in range(3):                                    # shareware: no
            key('ret', wait=1.2)                              # episodes, a shot
        until(lambda: flag('gamestate') == GS_LEVEL and flag('usergame')
              and not flag('demoplayback'), 30, 'no new game')
        time.sleep(2)
        x0, y0 = var('viewx'), var('viewy')
        key('right', wait=1.0, hold=1500)
        x1, y1 = var('viewx'), var('viewy')
        result['walked'] = [x0, y0, x1, y1]
        assert (x0, y0) != (x1, y1), result
        key('ret', wait=0.5)                                  # fire
        result['game_colours'] = colours(shot('doom-e1m1'))
        result['gametic'] = var('gametic')

        # ---- save in slot 0 (Save game: the menu's fourth; sideways, Left
        # is down), walk on, load it: the view is back where it was saved
        saved = settled()
        key('f2', wait=0.8)
        for _ in range(3):
            key('left', wait=0.3)
        key('ret', wait=0.8)                                  # Save game
        key('ret', wait=0.8)                                  # slot 0: named "E1M1 (DOOM1)"
        key('ret', wait=2.0)                                  # saved
        shot('doom-saved')
        key('right', wait=1.0, hold=1000)
        moved = settled()
        key('f2', wait=0.8)
        key('right', wait=0.3)                                # up: Load game
        key('ret', wait=0.8)
        key('ret', wait=3.0)                                  # slot 0
        loaded = settled()
        result['save_load'] = [saved, moved, loaded]
        assert moved != saved and loaded == saved, result

        # ---- 7: quick save (the first time, the Save game menu: slot 0),
        # a walk, 7 again (overwrite? LSoft: yes), a walk back, 9: quick
        # load (LSoft: yes): the view is back at the second quick save
        key('7', wait=0.8)
        key('ret', wait=0.8)                                  # slot 0
        key('ret', wait=2.0)                                  # saved
        key('right', wait=1.0, hold=1000)
        qsaved = settled()
        key('7', wait=0.8)
        shot('doom-quicksave')
        key('f1', wait=2.0)                                   # yes: saved over
        key('left', wait=1.0, hold=1000)
        qmoved = settled()
        key('9', wait=0.8)
        key('f1', wait=3.0)                                   # yes: loaded
        qloaded = settled()
        result['quick_save_load'] = [qsaved, qmoved, qloaded]
        assert qmoved != qsaved and qloaded == qsaved, result

        # ---- * + # over the game, Right held: the game is let go of it
        hold('right', True)
        time.sleep(0.8)
        chord('Quick panel')
        shot('doom-panel')
        # its sound, started on its own thread, has a volume of its own
        result['volume_tag'] = string(qt, symbol(elf, 'tags'))
        assert result['volume_tag'] == 'Doom', result
        stopped = settled()                                   # not walking on
        time.sleep(1.0)
        result['panel_still'] = [stopped, (var('viewx'), var('viewy'))]
        assert result['panel_still'][1] == stopped, result
        hold('right', False)
        chord('Doom')

        # ---- END: quit? LSoft: yes. Back in Apps, Doom unloaded
        key('esc', wait=1.0)
        shot('doom-quit')
        key('f1')
        wait_for(qt, base, lambda s: string(qt, s['ui_title']) == 'Apps', 20, 'not back in Apps')
        wait_for(qt, base, lambda s: s['apps_loaded'] == 0, 10, 'Doom still loaded')
        # The game's sound left the codec on until power-off once: off after 5 s
        time.sleep(7)
        assert qt.read(symbol(elf, 'initialized')) & 0xff == 0, 'codec still on after the game'
        assert qt.read(symbol(elf, 'holds')) == 0, 'the clock still held after the game'
        time.sleep(1.5)
        result['heap_after'] = status(qt, base)['heap_free']
        shot('doom-after')
        # within 16 KB of before (the log's buffers, the card's caches)
        assert abs(result['heap_after'] - result['heap_before']) < 16 * 1024, result

        # ---- power off, so the log reaches the card
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
    result['log'] = [l.strip() for l in log.splitlines() if 'doom' in l]
    result['config'] = bool(card_file(img, f'{DIR}/default.cfg'))
    save = card_file(img, f'{DIR}/doomsav0.dsg') or b''
    result['save'] = [len(save), save[:24].split(b'\0')[0].decode(errors='replace')]
    result['temp_left'] = card_file(img, f'{DIR}/temp.dsg') is not None
    frames = wav_out.read_bytes()[44:]                  # (a capture cut off has no sizes)
    samples = memoryview(frames[:len(frames) // 2 * 2]).cast('h')
    result['audio_peak'] = max((abs(s) for s in samples), default=0)
    print(json.dumps(result, indent=2))
    assert any('doom: ended (0)' in l for l in result['log']), result
    assert any(f'doom: {DIR},' in l for l in result['log']), result     # the WAD beside it
    assert result['config'], result
    assert result['save'][0] > 1000 and 'E1M1' in result['save'][1], result   # the map's name: the slot was empty
    assert not result['temp_left'], result
    assert result['audio_peak'] > 200, result
    print('test-doom: PASS')


if __name__ == '__main__':
    main()
