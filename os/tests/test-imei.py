#!/usr/bin/env python3
"""Tools / IMEI in QEMU, from a made-up NOR: the phone's IMEI, read-only from the
fixed NV (item 5, drivers/dsp.c nv_imei). The cases use no real number:
  - a valid IMEI (the usual published example) among other items: shown in full,
    and nowhere in the card's log;
  - a wrong check digit, and no item 5: nothing is shown as an IMEI.

SPDX-License-Identifier: GPL-2.0-or-later
"""
import argparse
import os
from pathlib import Path
import runpy
import struct
import subprocess
import sys
import time

REPO = Path(__file__).resolve().parents[2]
DSPT = runpy.run_path(str(REPO / 'os/tests/test-dsp.py'))
QT, capture = DSPT['QT'], DSPT['capture']
symbol, free_port, string = DSPT['symbol'], DSPT['free_port'], DSPT['string']
FIELDS, MAGIC, ST_ALL, NOR_SIZE = DSPT['FIELDS'], DSPT['MAGIC'], DSPT['ST_ALL'], DSPT['NOR_SIZE']
card_file = runpy.run_path(str(REPO / 'os/tests/test-boot.py'))['card_file']

NV_BASE = 0x680000
IMEI = '490154203237518'
BAD_CHECK = '490154203237519'


def tbcd(imei):
    """Item 5: nibble 0xA, then 15 digits, low nibble first."""
    nibbles = [0xa] + [int(c) for c in imei]
    return bytes(nibbles[i] | nibbles[i + 1] << 4 for i in range(0, 16, 2))


def item(ident, data):
    return struct.pack('<HH', ident, len(data)) + data + b'\0' * (-len(data) & 3)


def nor_with(items):
    nv = b'\x61\x5c\xb3\x0d' + b''.join(item(i, d) for i, d in items) + b'\xff' * 4
    image = bytearray(b'\xff' * NOR_SIZE)
    image[NV_BASE:NV_BASE + len(nv)] = nv
    return bytes(image)


def run(args, name, items, want):
    out = args.output / name
    out.mkdir(parents=True, exist_ok=True)
    base = symbol(args.elf, 'os_status')
    shown = symbol(args.elf, 'tools_imei_shown')
    card = out / 'card.img'
    subprocess.run([sys.executable, str(REPO / 'ports/rockbox/tests/make-sd-image.py'),
                    '--fat32', '--size-mib', '64', '--seconds', '1', '--force',
                    '--runtime', str(REPO / 'sdcard/.rockbox'),
                    '--rockbox', str(REPO / 'sdcard/payloads/rockbox.bin'),
                    '--output', str(card)], check=True, capture_output=True)
    nor = out / 'nor.bin'
    nor.write_bytes(nor_with(items))
    qmp_port, qtest_port = free_port(), free_port()
    command = [str(args.qemu.resolve()), '-M', 'b310e,boot-mode=rockbox',
               '-display', os.environ.get('B310E_QEMU_DISPLAY', 'none'), '-serial', 'none',
               '-monitor', 'none',
               '-drive', f'file={args.image.resolve().as_posix()},format=raw,if=none,id=os,readonly=on',
               '-drive', f'file={card.as_posix()},format=raw,if=none,id=sdcard',
               '-drive', f'file={nor.as_posix()},format=raw,if=none,id=nor,readonly=on',
               '-qmp', f'tcp:127.0.0.1:{qmp_port},server=on,wait=off',
               '-qtest', f'tcp:127.0.0.1:{qtest_port},server=on,wait=off']
    env = os.environ.copy()
    if os.name == 'nt':
        env['PATH'] = r'D:\Toolchains\msys64\mingw64\bin;C:\msys64\mingw64\bin;' + env.get('PATH', '')
    with (out / 'stderr.log').open('wb') as log:
        process = subprocess.Popen(command, env=env, stdout=log, stderr=log)
        qmp = qt = None
        try:
            for _ in range(100):
                try:
                    qmp = capture.QMP(qmp_port)
                    qt = QT(qtest_port)
                    break
                except OSError:
                    time.sleep(0.1)
            assert qmp and qt, 'QEMU did not start'

            def status():
                return {f: qt.read(base + 4 * i) for i, f in enumerate(FIELDS)}

            def title():
                return string(qt, status()['ui_title'])

            def wait(test, seconds, what):
                end = time.monotonic() + seconds
                while time.monotonic() < end:
                    if test():
                        return
                    time.sleep(0.1)
                raise AssertionError(f'{what}: {title()!r}')

            def press(k, want=None):
                qmp.hmp(f'sendkey {k} 100')
                if want:
                    wait(lambda: title() == want, 6, f'after {k}: wanted {want!r}')
                time.sleep(0.4)

            wait(lambda: status()['magic'] == MAGIC, 10, 'OS never started')
            wait(lambda: status()['selftest_done'] == ST_ALL, 60, 'self-test incomplete')
            wait(lambda: status()['ui_depth'] == 1, 10, 'no UI')
            press('ret', 'Menu')
            press('down')
            press('down')
            press('ret', 'Tools')
            press('down')
            press('down')
            press('ret', 'IMEI')
            text = string(qt, shown)
            qmp.command('screendump', {'filename': str(out / 'imei.png'), 'format': 'png'})
            assert text == want, f'{name}: shown {text!r}, wanted {want!r}'
            press('f2', 'Tools')
            press('f2', 'Menu')
            for _ in range(3):                  # Tools to Power
                press('down')
            press('ret', 'Power')
            press('ret', 'Power off')
            press('f1')
            process.wait(timeout=15)
        finally:
            if qt:
                qt.close()
            if qmp:
                qmp.close()
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=10)
    log_text = card_file(card.read_bytes(), '/b310e-os/log.txt') or b''
    assert log_text, 'no log on the card'
    assert IMEI.encode() not in log_text, 'the IMEI reached the log'
    print(f'PASS {name}: shown {text!r}', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qemu', type=Path, default=REPO / 'build/qemu/build/qemu-system-arm.exe')
    parser.add_argument('--elf', type=Path, default=REPO / 'build/os/b310e-os.elf')
    parser.add_argument('--image', type=Path, default=REPO / 'build/os/b310e-os.bin')
    parser.add_argument('--output', type=Path, default=REPO / 'build/os/test-imei')
    args = parser.parse_args()
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    other = (2, b'\x01' * 6), (1, b'\x02' * 12)
    run(args, 'valid', [*other, (5, tbcd(IMEI)), (3, b'\x03' * 8)], IMEI)
    run(args, 'bad-check', [*other, (5, tbcd(BAD_CHECK))], '')
    run(args, 'no-item', list(other), '')


if __name__ == '__main__':
    main()
