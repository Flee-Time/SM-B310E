#!/usr/bin/env python3
"""Screenshots of every built-in screen, for looking at the UI (no checks).

Boots the image in QEMU with a card (the stopwatch on it) and a 4-pole
headset, walks Menu -> each entry (and Tools -> each tool, Settings ->
Date & time, Apps -> Stopwatch, a dialog), and writes one PNG per screen
plus build/os/ui-tour/sheet.png with all of them side by side.

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
MENU = boot['MENU']


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
                    '--rockbox', str(REPO / 'sdcard/progs/rockbox.bin'),
                    '--add', f'b310e-os/apps/stopwatch.app={(args.build / "modules/stopwatch.app").resolve()}',
                    '--output', str(card)], check=True, capture_output=True)
    qmp_port, qtest_port = free_port(), free_port()
    cmd = [str(args.qemu.resolve()), '-M', 'b310e,boot-mode=rockbox', '-display', 'none',
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

        end = time.monotonic() + 60
        while time.monotonic() < end and status(qt, base)['ui_depth'] != 1:
            time.sleep(.2)
        time.sleep(2)
        shot('home')
        key('ret')
        shot('menu')
        key('ret', 1)
        shot('apps')
        key('ret', 1.5)
        key('ret', 1.5)
        shot('stopwatch')
        key('esc')
        menu('Files')
        shot('files')
        menu('Sound')
        shot('sound')
        key('ret', 1.5)
        shot('sound-tone')
        key('ret')
        menu('FM radio')
        time.sleep(4)
        shot('radio')
        menu('Tools')
        shot('tools')
        key('down', 0.3)
        key('down', 0.3)
        key('ret', 3)
        shot('sim')
        key('f2')
        key('down', 0.3)
        key('ret', 2)
        shot('usb')
        key('f2', 1.5)
        menu('System')
        time.sleep(1.2)
        shot('system')
        menu('Settings')
        shot('settings')
        for _ in range(3):
            key('down', 0.3)
        key('ret', 1)
        shot('date-time')
        key('f2')
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
    cols = 8
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
