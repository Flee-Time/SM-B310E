#!/usr/bin/env python3
"""Screenshots of every screen, for looking at the UI (no checks).

Boots the image in QEMU with a card (the apps on it: Alarm, Calculator,
Notes, FM radio, Recorder, Stopwatch) and a 4-pole headset, walks Menu ->
each entry (Tools -> each tool, Settings -> each category and a picker,
Apps -> each app and its screens: recording, the recordings, the radio's
menu and presets, the alarms, an alarm's page, its days and label, dialogs),
and writes one PNG per screen plus build/os/ui-tour/sheet.png with all of
them side by side.

  python os/tests/ui-tour.py

SPDX-License-Identifier: GPL-2.0-or-later
"""
import argparse
import os
from pathlib import Path
import runpy
import subprocess
import sys
import time

from PIL import Image

REPO = Path(__file__).resolve().parents[2]
boot = runpy.run_path(str(Path(__file__).with_name('test-boot.py')))
QT, capture = boot['QT'], boot['capture']
symbol, status, string, free_port = boot['symbol'], boot['status'], boot['string'], boot['free_port']
MENU, SETTINGS = boot['MENU'], boot['SETTINGS']
MODULES = ['alarm', 'calc', 'notes', 'radio', 'recorder', 'stopwatch']
APPS = ['Alarm', 'Calculator', 'Notes', 'FM radio', 'Recorder', 'Stopwatch']   # the card's order
TOOLS = ['Torch', 'SIM cards', 'USB', 'DSP boot', 'Sleep test']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=REPO / 'build/os')
    parser.add_argument('--qemu', type=Path, default=REPO / 'build/qemu/build/qemu-system-arm.exe')
    args = parser.parse_args()
    out = (args.build / 'ui-tour').resolve()
    out.mkdir(parents=True, exist_ok=True)
    elf, image = args.build / 'b310e-os.elf', args.build / 'b310e-os.bin'
    base = symbol(elf, 'os_status')
    card = out / 'card.img'
    subprocess.run([sys.executable, str(REPO / 'ports/rockbox/tests/make-sd-image.py'),
                    '--fat32', '--size-mib', '64', '--seconds', '1', '--force',
                    '--runtime', str(REPO / 'sdcard/.rockbox'),
                    '--rockbox', str(REPO / 'sdcard/payloads/rockbox.bin'),
                    *[a for m in MODULES for a in
                      ('--add', f'b310e-os/apps/{m}.app={(args.build / "modules" / (m + ".app")).resolve()}')],
                    '--output', str(card)], check=True, capture_output=True)
    qmp_port, qtest_port = free_port(), free_port()
    cmd = [str(args.qemu.resolve()), '-M', 'b310e,boot-mode=rockbox', '-display', os.environ.get('B310E_QEMU_DISPLAY', 'none'),
           '-serial', 'none', '-monitor', 'none',
           '-drive', f'file={image.resolve().as_posix()},format=raw,if=none,id=os,readonly=on',
           '-drive', f'file={card.as_posix()},format=raw,if=none,id=sdcard',
           '-qmp', f'tcp:127.0.0.1:{qmp_port},server=on,wait=off',
           '-qtest', f'tcp:127.0.0.1:{qtest_port},server=on,wait=off',
           '-global', 'sc6530_aux.headset-present=on', '-global', 'sc6530_adi.headset-mic=on',
           '-audiodev', 'none,id=audio0', '-global', 'sc6530_adi.audiodev=audio0']
    env = os.environ.copy()
    if os.name == 'nt':
        env['PATH'] = r'D:\Toolchains\msys64\mingw64\bin;' + env.get('PATH', '')
    shots = []
    p = subprocess.Popen(cmd, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    qmp = qt = None
    try:
        for _ in range(100):
            try:
                qmp, qt = capture.QMP(qmp_port), QT(qtest_port)
                break
            except OSError:
                time.sleep(.1)

        def title():
            return string(qt, status(qt, base)['ui_title'])

        def key(k, wait=0.6):
            qmp.hmp(f'sendkey {k} 100')
            time.sleep(wait)

        def shot(name):
            path = out / f'{len(shots):02d}-{name}.png'
            qmp.command('screendump', {'filename': str(path), 'format': 'png'})
            shots.append(path)

        def menu(entry):
            key('esc')
            key('ret')
            for _ in range(MENU.index(entry)):
                key('down', 0.3)
            key('ret', 1.2)

        def app(name, wait=1.5):
            key('esc')
            key('ret')
            key('ret', 1)
            for _ in range(APPS.index(name)):
                key('down', 0.3)
            key('ret', wait)

        end = time.monotonic() + 60
        while time.monotonic() < end and status(qt, base)['ui_depth'] != 1:
            time.sleep(.2)
        time.sleep(2)
        shot('home')
        key('ret')
        shot('menu')
        key('ret', 1)
        shot('apps')
        app('Stopwatch')
        key('ret', 1.5)
        shot('stopwatch')
        menu('Files')
        shot('files')
        key('f1', 1)
        shot('files-options')
        key('f2')

        # Recorder: recording, the list, playing, delete, back to a recording
        app('Recorder')
        shot('recorder')
        key('ret', 2.5)
        shot('recorder-rec')
        key('f1', 1)
        shot('recordings-rec')
        key('f2')
        key('ret', 1)                   # stop
        key('f1', 1)
        key('ret', 1.5)                 # play
        shot('recordings-play')
        key('ret')
        key('f1')
        shot('recordings-delete')
        key('f2')
        key('f2')
        key('ret')                      # record, leave, come back
        key('f2')
        app('Recorder', 2)
        shot('recorder-back')
        key('ret')

        # FM radio: the station, the menu, the presets, saving one; turned off
        app('FM radio', 4)
        shot('radio')
        key('f1')
        shot('radio-menu')
        key('down', 0.3)
        key('down', 0.3)
        key('ret')
        shot('radio-save-preset')
        key('ret')
        shot('radio-preset')
        key('f1')
        key('down', 0.3)
        key('ret')
        shot('radio-presets')
        key('f2')
        key('f1')
        for _ in range(4):
            key('down', 0.3)
        key('ret')                      # Turn off

        # Alarm: none, the time editor, two alarms, a page, days, label, delete
        app('Alarm')
        shot('alarm-empty')
        key('ret')
        shot('alarm-new')
        key('ret')
        key('down', 0.3)                # Add alarm again
        key('ret')
        for _ in range(3):
            key('up', 0.3)
        key('right', 0.3)
        for _ in range(4):
            key('down', 0.3)
        key('ret')
        key('f1')                       # off
        shot('alarm-list')
        key('ret')
        shot('alarm-page')
        key('down', 0.3)
        key('ret')
        for _ in range(5):              # Monday .. Friday
            key('ret', 0.3)
            key('down', 0.3)
        shot('alarm-repeat')
        key('f2')
        key('down', 0.3)
        key('ret')
        for k in ('9', '6', '6', '6', '7', '7', '7'):    # W o r
            key(k, 0.2)
        time.sleep(1)
        for k in ('5', '5'):            # k
            key(k, 0.2)
        time.sleep(1)
        shot('alarm-label')
        key('ret')
        shot('alarm-page-set')
        key('down', 0.3)
        key('down', 0.3)
        key('ret')
        shot('alarm-delete')
        key('f2')
        key('f2')
        shot('alarm-list-2')

        # Calculator and Notes
        app('Calculator')
        shot('calculator')
        for k in ('1', '2', '3', '4', '5', '6', '7', 'right', '8', '9'):
            key(k, 0.25)
        shot('calculator-long')
        app('Notes')
        shot('notes')
        key('ret', 1)
        for k in ('4', '4'):
            key(k, 0.2)
        time.sleep(1)
        for k in ('3', '3', '0', '6', '6', '6', '5', '5', '5', '8', '8', '8'):
            key(k, 0.2)
        time.sleep(1)
        shot('note-edit')
        key('f2')
        key('f2')

        # Tools
        menu('Tools')
        shot('tools')
        key('down', 0.3)
        key('ret', 3)
        shot('sim')
        key('f2')
        key('down', 0.3)
        key('ret', 2)
        shot('usb')
        key('f2', 1.5)
        key('down', 0.3)
        key('ret', 2)
        shot('dsp-boot')
        key('f2')

        menu('System')
        time.sleep(6)                   # some history in the graphs
        shot('system')
        for name in ('system-cpu', 'system-threads', 'system-storage', 'system-power'):
            key('right', 0.8)
            shot(name)

        # Settings: every category, a picker
        menu('Settings')
        shot('settings')
        for i, name in enumerate(SETTINGS):
            key('ret', 1)
            shot('settings-' + name.lower().replace(' & ', '-').replace(' ', '-'))
            if name == 'Display':
                key('ret')
                shot('settings-picker')
                key('f2')
            key('f2')
            key('down', 0.3)

        menu('Power')
        shot('power')
        key('ret')
        shot('dialog')
        key('f2')
    finally:
        if qt:
            qt.close()
        if qmp:
            qmp.close()
        p.terminate()
        p.wait()
    imgs = [Image.open(s).convert('RGB') for s in shots]
    cols = 10
    w, h = imgs[0].size
    rows = (len(imgs) + cols - 1) // cols
    sheet = Image.new('RGB', (cols * (w + 6), rows * (h + 6)), (255, 255, 255))
    for i, im in enumerate(imgs):
        sheet.paste(im, ((i % cols) * (w + 6), (i // cols) * (h + 6)))
    sheet = sheet.resize((sheet.width * 2, sheet.height * 2), Image.NEAREST)
    sheet.save(out / 'sheet.png')
    print(out / 'sheet.png', len(shots), 'screens')


if __name__ == '__main__':
    main()
