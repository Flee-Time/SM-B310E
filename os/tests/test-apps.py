#!/usr/bin/env python3
"""The Tools apps in QEMU: Calculator and Alarm, through the keypad.

Calculator: 12 + 30 = 42; 7.5 x 2 = 15; 1 / 0 is an error; Del clears it.
The result and state are read from the calculator's variables (calc_*).
Alarm: set to 00:01 through the editor (the RTC model starts at 00:00:00
at power-on), wait for it to ring (the screen wakes, the ringing screen is
on top, the log says so), Snooze, and check the snooze time and that the
setting reached the card.

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

REPO = Path(__file__).resolve().parents[2]
boot = runpy.run_path(str(Path(__file__).with_name('test-boot.py')))
QT, capture = boot['QT'], boot['capture']
symbol, status, string, wait_for, free_port, card_file = (
    boot[n] for n in ('symbol', 'status', 'string', 'wait_for', 'free_port', 'card_file'))
MENU, MAGIC = boot['MENU'], boot['MAGIC']
TOOLS = ['Torch', 'Vibrate', 'SIM cards', 'USB storage', 'Calculator', 'Alarm', 'Notes']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qemu', type=Path, default=REPO / 'build/qemu/build/qemu-system-arm.exe')
    parser.add_argument('--build', type=Path, default=REPO / 'build/os')
    args = parser.parse_args()
    out = (args.build / 'test-apps').resolve()
    out.mkdir(parents=True, exist_ok=True)
    elf, image = args.build / 'b310e-os.elf', args.build / 'b310e-os.bin'
    base = symbol(elf, 'os_status')
    acc, entry, error = (symbol(elf, n) for n in ('calc_acc', 'calc_entry', 'calc_error'))
    ringing, snooze = symbol(elf, 'alarm_ringing'), symbol(elf, 'alarm_snooze_min')
    card = out / 'card.img'
    subprocess.run([sys.executable, str(REPO / 'ports/rockbox/tests/make-sd-image.py'),
                    '--fat32', '--size-mib', '64', '--seconds', '1', '--force',
                    '--runtime', str(REPO / 'sdcard/.rockbox'),
                    '--rockbox', str(REPO / 'sdcard/progs/rockbox.bin'),
                    '--output', str(card)], check=True, capture_output=True)
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
            lo, hi = qt.read(acc), qt.read(acc + 4)
            text = bytes(qt.read(entry + i) & 0xff for i in range(13)).split(b'\0')[0].decode()
            return struct.unpack('<d', struct.pack('<II', lo, hi))[0], text, qt.read(error) & 0xff

        def shot(name):
            qmp.command('screendump', {'filename': str(out / f'{name}.png'), 'format': 'png'})

        wait_for(qt, base, lambda s: s['magic'] == MAGIC, 10, 'OS never started')
        wait_for(qt, base, lambda s: s['ui_depth'] == 1, 60, 'no UI')
        time.sleep(1)

        def tool(name):
            key('esc', wait=0.5)
            key('ret', 'Menu')
            for _ in range(MENU.index('Tools')):
                key('down')
            key('ret', 'Tools')
            key('left')                 # Tools keeps its row: page up to the top
            key('left')
            for _ in range(TOOLS.index(name)):
                key('down')
            key('ret', name)

        # ---- Calculator
        tool('Calculator')
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

        # ---- Alarm: 07:00 -> 00:01
        tool('Alarm')
        key('down')
        key('ret')                                            # edit the time
        for _ in range(7):
            key('down')
        key('right')
        key('up')
        shot('alarm-edit')
        key('ret')                                            # save (and on)
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

        # ---- Notes: "Hi" by multi-tap (4 4: G H, a capital first; then
        # 4 4 4: g h i), saved as the first note
        tool('Notes')
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
    result['log'] = [l.strip() for l in log.splitlines() if 'alarm:' in l]
    assert 'alarm_on=1' in result['settings'] and 'alarm_min=1' in result['settings'], result
    assert any('alarm: ringing at 00:01' in l for l in result['log']), result
    result['note'] = (card_file(img, '/b310e-os/notes/NOTE0001.TXT') or b'').decode(errors='replace')
    assert result['note'] == 'Hi', result
    (out / 'result.json').write_text(json.dumps(result, indent=2))
    print('PASS ' + json.dumps(result))


if __name__ == '__main__':
    main()
