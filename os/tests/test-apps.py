#!/usr/bin/env python3
"""The apps in QEMU through the keypad: Alarm, Calculator, Notes, Music,
copy and paste in Files.

Alarm (the Alarm app, os/modules/alarm): Add alarm, the time editor from
07:00 to 00:01 (the RTC model starts at 00:00:00 at power-on), then on
its page every day of the week switched on and the label "A" by
multi-tap; wait for the OS to ring it (the screen wakes, the ringing
screen is on top, the log says so), Snooze, and check the snooze time,
that it stays on (it repeats) and that /b310e-os/alarms.txt has it.
Calculator: 12 + 30 = 42; 7.5 x 2 = 15; 1 / 0 is an error; Del clears it.
The result and state are read from the calculator's variables (calc_*).
Notes: "Hi" by multi-tap. Music (music.app): two short tones in /music;
Apps -> Music plays the first and goes on to the second by itself, Center
pauses it (the WAV codec module is on the card); closing the app keeps
the queue playing; Center on b.wav in Files opens Music with its path:
Now playing, b.wav first. Files: /music/a.wav, which the playing queue
has open, copied and pasted into its own folder as "Copy of a.wav", the
same bytes (a locked name once stopped the paste).

  python os/tests/test-apps.py

SPDX-License-Identifier: GPL-2.0-or-later
"""
import argparse
import json
import os
from pathlib import Path
import runpy
import struct
import subprocess
import sys
import time
import wave

REPO = Path(__file__).resolve().parents[2]
boot = runpy.run_path(str(Path(__file__).with_name('test-boot.py')))
QT, capture = boot['QT'], boot['capture']
symbol, status, string, wait_for, free_port, card_file = (
    boot[n] for n in ('symbol', 'status', 'string', 'wait_for', 'free_port', 'card_file'))
card_list = boot['card_list']


def tone_wav(path, hz, seconds):
    import math
    rate = 16000
    with wave.open(str(path), 'wb') as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(b''.join(struct.pack('<h', int(8000 * math.sin(2 * math.pi * hz * i / rate)))
                               for i in range(int(rate * seconds))))
MENU, MAGIC = boot['MENU'], boot['MAGIC']
APPS = ['Alarm', 'Calculator', 'Music', 'Notes']   # .app modules, in the card's order (by file name)
ALARM_PAGE = ['Time', 'Repeat', 'Label', 'Alarm on', 'Delete']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qemu', type=Path, default=REPO / 'build/qemu/build/qemu-system-arm.exe')
    parser.add_argument('--build', type=Path, default=REPO / 'build/os')
    args = parser.parse_args()
    out = (args.build / 'test-apps').resolve()
    out.mkdir(parents=True, exist_ok=True)
    elf, image = args.build / 'b310e-os.elf', args.build / 'b310e-os.bin'
    base = symbol(elf, 'os_status')
    # the calculator is a module linked at 0: its variables are at the loaded
    # image (the loader's first app, app_t.image at +8) plus their offsets
    calc_elf = args.build / 'modules/calc.elf'
    acc_o, entry_o, error_o = (symbol(calc_elf, n) for n in ('calc_acc', 'calc_entry', 'calc_error'))
    apps_list = symbol(elf, 'apps')
    ringing, snooze = symbol(elf, 'alarm_ringing'), symbol(elf, 'alarm_snooze_min')
    m_cur, m_on = symbol(elf, 'music_cur'), symbol(elf, 'music_on')
    m_state = symbol(elf, 'media_status')     # its first word: os/media.h's state
    card = out / 'card.img'
    tone_wav(out / 'a.wav', 440, 1.0)
    tone_wav(out / 'b.wav', 880, 1.0)
    subprocess.run([sys.executable, str(REPO / 'ports/rockbox/tests/make-sd-image.py'),
                    '--fat32', '--size-mib', '64', '--seconds', '1', '--force',
                    '--runtime', str(REPO / 'sdcard/.rockbox'),
                    '--rockbox', str(REPO / 'sdcard/payloads/rockbox.bin'),
                    '--add', f'b310e-os/apps/alarm.app={(args.build / "modules/alarm.app").resolve()}',
                    '--add', f'b310e-os/apps/calc.app={(args.build / "modules/calc.app").resolve()}',
                    '--add', f'b310e-os/apps/notes.app={(args.build / "modules/notes.app").resolve()}',
                    '--add', f'b310e-os/apps/music.app={(args.build / "modules/music.app").resolve()}',
                    '--add', f'music/a.wav={out / "a.wav"}',
                    '--add', f'music/b.wav={out / "b.wav"}',
                    '--add', f'b310e-os/codecs/wav.cdc={args.build / "codecs/wav.cdc"}',
                    '--output', str(card)], check=True, capture_output=True)
    root = [n for n, d, h in card_list(card.read_bytes(), '/') if d and not h]
    qmp_port, qtest_port = free_port(), free_port()
    cmd = [str(args.qemu.resolve()), '-M', 'b310e,boot-mode=rockbox',
           '-display', 'none', '-serial', 'none', '-monitor', 'none',
           '-drive', f'file={image.resolve().as_posix()},format=raw,if=none,id=os,readonly=on',
           '-drive', f'file={card.as_posix()},format=raw,if=none,id=sdcard',
           '-qmp', f'tcp:127.0.0.1:{qmp_port},server=on,wait=off',
           '-qtest', f'tcp:127.0.0.1:{qtest_port},server=on,wait=off',
           '-audiodev', 'none,id=audio0', '-global', 'sc6530_adi.audiodev=audio0']
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

        def key(k, want=None, wait=0.35):
            # END is polled every 20 ms; hold it longer, as a thumb does,
            # so a stalled host cannot shorten it to nothing
            qmp.hmp(f'sendkey {k} {200 if k == "esc" else 100}')
            if want:
                wait_for(qt, base, lambda s: string(qt, s['ui_title']) == want, 6, f'not {want}')
            time.sleep(wait)

        def calc():
            image = qt.read(qt.read(apps_list) + 8)
            acc, entry, error = image + acc_o, image + entry_o, image + error_o
            lo, hi = qt.read(acc), qt.read(acc + 4)
            text = bytes(qt.read(entry + i) & 0xff for i in range(13)).split(b'\0')[0].decode()
            return struct.unpack('<d', struct.pack('<II', lo, hi))[0], text, qt.read(error) & 0xff

        def shot(name):
            qmp.command('screendump', {'filename': str(out / f'{name}.png'), 'format': 'png'})

        def alarm_lines():
            """/b310e-os/alarms.txt in the card image as it is now (the
            service writes it at every change)."""
            return ((card_file(card.read_bytes(), '/b310e-os/alarms.txt') or b'')
                    .decode(errors='replace').splitlines())

        wait_for(qt, base, lambda s: s['magic'] == MAGIC, 10, 'OS never started')
        wait_for(qt, base, lambda s: s['ui_depth'] == 1, 60, 'no UI')
        time.sleep(1)

        def app(name):
            key('esc', wait=0.5)
            key('ret', 'Menu')
            key('ret', 'Apps')
            for _ in range(APPS.index(name)):
                key('down')
            key('ret', name, wait=0.8)

        # ---- Alarm: Add alarm, 07:00 -> 00:01, every day, "A"
        app('Alarm')
        shot('alarm-empty')
        key('ret', 'New alarm')                               # Add alarm
        for _ in range(7):
            key('down')
        key('right')
        key('up')
        shot('alarm-edit')
        key('ret', 'Alarm')                                   # save (and on)
        result['alarms_after_add'] = alarm_lines()
        key('ret', 'Edit alarm')
        for _ in range(ALARM_PAGE.index('Repeat')):
            key('down')
        key('ret', 'Repeat')
        for _ in range(7):                                    # every day
            key('ret', wait=0.25)
            key('down', wait=0.25)
        shot('alarm-repeat')
        key('f2', 'Edit alarm')
        for _ in range(ALARM_PAGE.index('Label') - ALARM_PAGE.index('Repeat')):
            key('down')
        key('ret', 'Label')
        key('2')                                              # A
        time.sleep(1.2)
        key('ret', 'Edit alarm')
        shot('alarm-page')
        key('f2', 'Alarm')
        shot('alarm-set')
        # ringing at 00:01:00 of the RTC: well within 90 s of power-on
        end = time.monotonic() + 120
        while time.monotonic() < end and not qt.read(ringing) & 0xff:
            time.sleep(0.2)
        result['rang'] = bool(qt.read(ringing) & 0xff)
        result['rtc_at_ring'] = '%02d:%02d:%02d' % tuple(
            qt.read(0x82001600 + 4 * i) & 0x3f for i in (2, 1, 0))
        assert result['rang'], f'the alarm did not ring: {result}'
        time.sleep(0.5)
        result['title_ringing'] = title()
        result['light_ringing'] = status(qt, base)['ui_light']
        shot('alarm-ringing')
        key('f1', wait=0.6)                                   # Snooze
        result['ringing_after_snooze'] = bool(qt.read(ringing) & 0xff)
        result['snooze_min'] = qt.read(snooze)
        assert result['title_ringing'] == 'Alarm' and result['light_ringing'] == 0, result
        assert not result['ringing_after_snooze'] and result['snooze_min'] == 6, result
        shot('alarm-after')

        # ---- Calculator
        app('Calculator')
        for k in ('1', '2', 'up', '3', '0', 'ret'):
            key(k)
        result['12+30'] = calc()[0]
        shot('calc-42')
        for k in ('7', 'slash', '5', 'left', '2', 'ret'):    # slash: the # key
            key(k)
        result['7.5x2'] = calc()[0]
        for k in ('1', 'right', '0', 'ret'):
            key(k)
        result['1/0 error'] = calc()[2]
        shot('calc-error')
        key('f1')                                             # Del: clears
        result['after Del'] = calc()
        assert result['12+30'] == 42, result
        assert result['7.5x2'] == 15, result
        assert result['1/0 error'] == 1, result
        assert result['after Del'] == (0.0, '', 0), result

        # ---- Notes: "Hi" by multi-tap (4 4: G H, a capital first; then
        # 4 4 4: g h i), saved as the first note
        app('Notes')
        key('ret', 'New note')
        key('4')
        key('4')
        time.sleep(1.2)                                       # the letter is chosen
        for _ in range(3):
            key('4')
        time.sleep(1.2)
        shot('note-edit')
        key('ret', 'Notes')                                   # Save
        time.sleep(0.5)
        result['notes_rows'] = status(qt, base)['ui_list_count']
        shot('notes')
        assert result['notes_rows'] == 2, result              # New note + "Hi"

        # ---- Music: Apps -> Music, the first track, then the second by itself
        app('Music')
        key('ret', 'Now playing')                             # play a.wav
        result['music_first'] = (qt.read(m_cur), qt.read(m_on) & 0xff)
        end = time.monotonic() + 8
        while time.monotonic() < end and qt.read(m_cur) != 1:
            time.sleep(0.1)
        result['music_next'] = (qt.read(m_cur), qt.read(m_on) & 0xff)
        shot('music')
        key('ret')                                            # pause b.wav
        result['music_paused'] = (qt.read(m_on) & 0xff, qt.read(m_state))
        assert result['music_first'] == (0, 1), result
        assert result['music_next'] == (1, 1), result
        assert result['music_paused'] == (1, 3), result     # MEDIA_PAUSED
        key('f2', 'Music')                                    # Back: the list
        key('f2', 'Apps', wait=0.8)                           # Back: the app closes
        result['music_after_close'] = (qt.read(m_on) & 0xff, qt.read(apps_list) != 0)
        # its notification keeps the app loaded (the buttons stay valid)
        assert result['music_after_close'] == (1, True), result

        # ---- the quick panel: Center held on Home; the Music notification
        # first, its middle button (Play, the music is paused) selected
        key('esc', 'B310E OS', wait=0.5)
        qmp.hmp('sendkey ret 1000')                           # held
        wait_for(qt, base, lambda s: string(qt, s['ui_title']) == 'Quick panel', 4,
                 'held Center did not open the quick panel')
        time.sleep(0.6)                                       # let go
        shot('panel')
        result['panel_on_home'] = title()
        def state_is(want, seconds=3):
            end = time.monotonic() + seconds
            while time.monotonic() < end and qt.read(m_state) != want:
                time.sleep(0.05)
            return qt.read(m_state)

        key('ret', 'Quick panel')                             # Play
        result['panel_play'] = state_is(2)
        key('right', 'Quick panel')                           # >|
        key('ret', 'Quick panel')
        result['panel_next'] = (qt.read(m_cur), state_is(2))
        key('left', 'Quick panel')
        key('ret', 'Quick panel')                             # Pause
        result['panel_pause'] = state_is(3)
        key('down', 'Quick panel')                            # Music volume
        shot('panel-volume')
        key('f2', 'B310E OS')                                 # Close
        key('ret', 'Menu')                                    # a short press still opens the Menu
        result['short_press'] = title()
        assert result['panel_play'] == 2, result              # MEDIA_PLAYING
        assert result['panel_next'] == (0, 2), result         # a.wav again (two tracks)
        assert result['panel_pause'] == 3, result             # MEDIA_PAUSED
        assert result['short_press'] == 'Menu', result

        # ---- Files: /music/a.wav, Options -> Copy, Options -> Paste here
        key('esc', wait=0.5)
        key('ret', 'Menu')
        for _ in range(MENU.index('Files')):
            key('down')
        key('ret', '/')
        for _ in range(root.index('music')):
            key('down')
        key('ret', '/music')                                  # a.wav is first
        # Center on b.wav: Music with its path, the folder queued from b.wav
        key('down')
        key('ret', 'Now playing', wait=0)
        end = time.monotonic() + 3                            # b.wav is 1 s long
        while time.monotonic() < end and qt.read(m_state) != 2:
            time.sleep(0.05)
        result['music_from_files'] = (qt.read(m_cur), qt.read(m_on) & 0xff, qt.read(m_state))
        shot('music-from-files')
        assert result['music_from_files'] == (1, 1, 2), result  # MEDIA_PLAYING
        key('f2', '/music')                                   # Back: the app closes
        key('up')                                             # a.wav
        key('f1', 'Options')
        key('down')
        key('down')
        key('ret', '/music')                                  # Copy
        key('f1', 'Options')
        for _ in range(3):
            key('down')
        shot('paste')
        key('ret')                                            # Paste here
        wait_for(qt, base, lambda s: string(qt, s['ui_title']) == '/music', 15, 'paste did not end')
        time.sleep(0.5)
        result['music_rows_after_paste'] = status(qt, base)['ui_list_count']
        shot('pasted')
        assert result['music_rows_after_paste'] == 3, result

        # power off from the menu: the log reaches the card
        key('esc', wait=0.5)
        key('ret', 'Menu')
        for _ in range(MENU.index('Power')):
            key('down')
        key('ret', 'Power')
        key('ret', 'Power off')                               # the confirmation
        qmp.hmp('sendkey f1 100')                             # Yes
        p.wait(timeout=15)                                    # QEMU exits
    except Exception:
        print(json.dumps(result, indent=2))
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
    settings = (card_file(img, '/b310e-os/settings.txt') or b'').decode()
    log = (card_file(img, '/b310e-os/log.txt') or b'').decode(errors='replace')
    result['settings'] = [l for l in settings.splitlines() if l.startswith('alarm')]
    result['alarms'] = ((card_file(img, '/b310e-os/alarms.txt') or b'')
                        .decode(errors='replace').splitlines())
    result['log'] = [l.strip() for l in log.splitlines() if 'alarm' in l]
    # on, 00:01, every day (bits 0..6), "A"
    assert result['alarms'] == ['1 1 127 A'], result
    assert any('alarm app: added 00:01' in l for l in result['log']), result
    assert any('alarm: ringing at 00:01' in l for l in result['log']), result
    result['note'] = (card_file(img, '/b310e-os/notes/NOTE0001.TXT') or b'').decode(errors='replace')
    assert result['note'] == 'Hi', result
    copy = card_file(img, '/music/Copy of a.wav')
    result['copy_bytes'] = len(copy) if copy else None
    assert copy and copy == card_file(img, '/music/a.wav'), result
    result['music_log'] = [l.strip() for l in log.splitlines() if 'playing /music' in l]
    assert any('/music/a.wav' in l for l in result['music_log']) and \
        any('/music/b.wav' in l for l in result['music_log']), result
    (out / 'result.json').write_text(json.dumps(result, indent=2))
    print('PASS ' + json.dumps(result))


if __name__ == '__main__':
    main()
