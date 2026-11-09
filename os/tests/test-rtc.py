#!/usr/bin/env python3
"""The RTC calendar (drivers/rtc.c) and the "Date & time" screen (apps/clock.c) in QEMU.

The test links its own copy of the OS from the build's objects (the
Makefile's link command, app_clock kept with -Wl,-u) and opens the screen
through Menu -> Settings -> Date & time. Then:
  - the RTC model is preset to 2026-03-15 10:20:30 (the stock day count:
    days since 1980-01-01) and the screen must show it;
  - typing 07 11 2026 14 05 09, Up on the seconds, Left, Down on the
    minutes, LSoft must set the RTC to 2026-11-07 14:04:10 (day 17112),
    with the update acknowledgements cleared again;
  - typing 31 02 2028 must save 2028-02-29 (the day cut to the month's
    length) and keep the running time of day;
  - a change and RSoft (cancel) must leave the RTC alone;
  - the card log must have the set lines;
  - a day counter that cannot be right at boot (0xaaaa, as a battery pull left
    it on the phone) goes back to the saved time, or to the build's date when
    the saved time came from such a clock.

SPDX-License-Identifier: GPL-2.0-or-later
"""
import argparse
import datetime
import json
import os
from pathlib import Path
import runpy
import shlex
import subprocess
import sys
import time

REPO = Path(__file__).resolve().parents[2]
BOOT = runpy.run_path(str(Path(__file__).with_name('test-boot.py')), run_name='test_boot')
QT, capture = BOOT['QT'], BOOT['capture']
status, string, wait_for, free_port = (BOOT[n] for n in ('status', 'string', 'wait_for', 'free_port'))
MAGIC, ST_ALL, MENU = BOOT['MAGIC'], BOOT['ST_ALL'], BOOT['MENU']

RTC = 0x82001600
EPOCH = datetime.date(1980, 1, 1)
TITLE = 'Date & time'


def days(y, m, d):
    return (datetime.date(y, m, d) - EPOCH).days


def tool(name):
    for d in os.environ.get('PATH', '').split(os.pathsep) + ['D:/Toolchains/arm-none-eabi/bin']:
        for ext in ('', '.exe'):
            p = Path(d) / f'arm-none-eabi-{name}{ext}'
            if p.is_file():
                return str(p)
    raise FileNotFoundError(f'arm-none-eabi-{name}')


def symbols(elf):
    """name -> [addresses] (local symbols too)."""
    out = {}
    for line in subprocess.run([tool('nm'), str(elf)], capture_output=True, text=True,
                               check=True).stdout.splitlines():
        parts = line.split()
        if len(parts) == 3:
            out.setdefault(parts[2], []).append(int(parts[0], 16))
    return out


def link_test_image(build, fatfs, out):
    """The build's own link command, with app_clock kept, into out/."""
    build = build.resolve()
    rel = os.path.relpath(build, REPO / 'os').replace('\\', '/')
    make = ['make', '-C', str(REPO / 'os'), f'BUILD={rel}', f'FATFS={fatfs}', 'PYTHON=python']
    subprocess.run(make + ['-j8'], check=True, capture_output=True)    # up to date first
    dry = subprocess.run(make + ['-n', '--always-make', f'{rel}/b310e-os.elf'],
                         capture_output=True, text=True, check=True).stdout
    line = next(l for l in dry.splitlines() if '-T link/os.ld' in l and f'{rel}/b310e-os.elf' in l)
    args = shlex.split(line)
    elf, binary = out / 'b310e-os.elf', out / 'b310e-os.bin'
    args[args.index('-o') + 1] = str(elf)
    args = [a.replace(f'{rel}/b310e-os.map', str(out / 'b310e-os.map')) for a in args]
    args[0] = tool('gcc')
    subprocess.run(args + ['-Wl,-u,app_clock'], cwd=REPO / 'os', check=True)
    subprocess.run([tool('objcopy'), '-O', 'binary', str(elf), str(binary)], check=True)
    return elf, binary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qemu', type=Path, default=REPO / 'build/qemu/build/qemu-system-arm.exe')
    parser.add_argument('--build', type=Path, default=REPO / 'build/os',
                        help='the OS build folder: made, then its objects linked again')
    parser.add_argument('--fatfs', default='../build/fatfs/source')
    parser.add_argument('--output', type=Path, default=None)
    args = parser.parse_args()
    out = (args.output or args.build / 'test-rtc').resolve()
    out.mkdir(parents=True, exist_ok=True)
    elf, image = link_test_image(args.build, args.fatfs, out)
    sym = symbols(elf)
    base = sym['os_status'][0]
    state = sym['clock_state'][0]

    card = out / 'card.img'
    # the clock saved at the last power-off: 2025-12-31 12:00:00; the RTC
    # model starts cleared (day 0), as the phone's after a reset
    saved_day = days(2025, 12, 31)
    clock_txt = out / 'clock.txt'
    clock_txt.write_text(f'{saved_day * 86400 + 12 * 3600}\n')
    subprocess.run([sys.executable, str(REPO / 'ports/rockbox/tests/make-sd-image.py'),
                    '--fat32', '--size-mib', '64', '--seconds', '1', '--force',
                    '--runtime', str(REPO / 'sdcard/.rockbox'),
                    '--rockbox', str(REPO / 'sdcard/payloads/rockbox.bin'),
                    '--add', f'b310e-os/clock.txt={clock_txt}',
                    '--output', str(card)], check=True, capture_output=True)

    qmp_port, qtest_port = free_port(), free_port()
    command = [str(args.qemu.resolve()), '-M', 'b310e,boot-mode=rockbox',
               '-display', os.environ.get('B310E_QEMU_DISPLAY', 'none'), '-serial', 'none', '-monitor', 'none',
               '-drive', f'file={image.as_posix()},format=raw,if=none,id=os,readonly=on',
               '-drive', f'file={card.as_posix()},format=raw,if=none,id=sdcard',
               '-qmp', f'tcp:127.0.0.1:{qmp_port},server=on,wait=off',
               '-qtest', f'tcp:127.0.0.1:{qtest_port},server=on,wait=off',
               '-D', str(out / 'qemu.log')]
    env = os.environ.copy()
    if os.name == 'nt':
        env['PATH'] = r'D:\Toolchains\msys64\mingw64\bin;C:\msys64\mingw64\bin;' + env.get('PATH', '')
    result = {}
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

            def title():
                return string(qt, status(qt, base)['ui_title'])

            def shot(name):
                qmp.command('screendump', {'filename': str(out / f'{name}.png'), 'format': 'png'})

            def key(name, settle=0.35):
                keys = status(qt, base)['keys']
                qmp.hmp(f'sendkey {name} 100')
                end = time.monotonic() + 5
                while status(qt, base)['keys'] < keys + 2 and time.monotonic() < end:
                    time.sleep(0.05)            # press and release seen
                time.sleep(settle)

            def to(name, want):
                key(name)
                end = time.monotonic() + 6
                while title() != want and time.monotonic() < end:
                    time.sleep(0.1)
                assert title() == want, f'after {name}: {title()!r}, wanted {want!r}'
                time.sleep(0.3)

            def fields():
                return [qt.read(state + 4 * i) for i in range(6)]

            def rtc():
                return [qt.read(RTC + 4 * i) & 0xffff for i in range(4)]   # s m h day

            wait_for(qt, base, lambda s: s['magic'] == MAGIC, 10, 'OS never started')
            wait_for(qt, base, lambda s: s['selftest_done'] == ST_ALL, 60, 'self-test incomplete')
            wait_for(qt, base, lambda s: s['ui_depth'] == 1, 10, 'no UI')
            result['restored'] = rtc()
            assert result['restored'][3] == saved_day and result['restored'][2] == 12, result

            # A clock as the stock firmware leaves it: 2026-03-15 10:20:30.
            preset = days(2026, 3, 15)
            for i, v in enumerate((30, 20, 10, preset)):
                qt.write(RTC + 0x10 + 4 * i, v)
            qt.write(RTC + 0x38, 0xf00)                # the model's acknowledgements
            assert rtc()[3] == preset, rtc()

            to('ret', 'Menu')                        # Menu -> Settings -> Date & time
            for _ in range(MENU.index('Settings')):
                key('down')
            to('ret', 'Settings')
            for _ in range(BOOT['SETTINGS'].index('Date & time')):    # the 4th category
                key('down')
            to('ret', TITLE)                     # the page: Set the clock first
            to('ret', TITLE)
            time.sleep(0.6)
            f = fields()
            result['shown_preset'] = f
            assert f[:5] == [15, 3, 2026, 10, 20] and 30 <= f[5] <= 40, f
            shot('clock-preset')

            for d in '07112026140509':
                key(d, 0.15)
            key('up')
            key('left')
            key('down')
            time.sleep(0.4)
            f = fields()
            result['typed'] = f
            assert f == [7, 11, 2026, 14, 4, 10], f
            shot('clock-typed')
            to('f1', TITLE)                     # Save
            r = rtc()
            result['rtc_after_save'] = r
            assert r[3] == days(2026, 11, 7) == 17112 and r[2] == 14 and r[1] == 4, r
            assert 10 <= r[0] <= 15, r
            assert qt.read(RTC + 0x34) & 0xf00 == 0, hex(qt.read(RTC + 0x34))

            to('ret', TITLE)
            time.sleep(0.6)
            f = fields()
            assert f[:5] == [7, 11, 2026, 14, 4], f
            for d in '31022028':
                key(d, 0.15)
            to('f1', TITLE)
            r = rtc()
            result['rtc_leap'] = r
            assert r[3] == days(2028, 2, 29) and r[2] == 14 and r[1] == 4, r

            to('ret', TITLE)
            time.sleep(0.6)
            f = fields()
            result['shown_leap'] = f
            assert f[:5] == [29, 2, 2028, 14, 4], f
            shot('clock-leap')
            key('up')                                # 29.02. -> 01.02.
            assert fields()[0] == 1, fields()
            to('f2', TITLE)                     # Cancel
            assert rtc()[3] == days(2028, 2, 29), rtc()
            result['cancel_kept'] = True
            time.sleep(2.5)                          # the log thread writes
        except Exception:
            print(json.dumps(result, indent=2))
            raise
        finally:
            if qt:
                qt.close()
            if qmp:
                qmp.close()
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=10)

    # The counters as a battery pull left them, with the machine still paused
    def garbage(name, saved_seconds, want_day, planted_day=0xaaaa):
        d = out / name
        d.mkdir(exist_ok=True)
        txt = d / 'clock.txt'
        txt.write_text(f'{saved_seconds}\n')
        img = d / 'card.img'
        subprocess.run([sys.executable, str(REPO / 'ports/rockbox/tests/make-sd-image.py'),
                        '--fat32', '--size-mib', '64', '--seconds', '1', '--force',
                        '--runtime', str(REPO / 'sdcard/.rockbox'),
                        '--rockbox', str(REPO / 'sdcard/payloads/rockbox.bin'),
                        '--add', f'b310e-os/clock.txt={txt}',
                        '--output', str(img)], check=True, capture_output=True)
        qp, tp = free_port(), free_port()
        p = subprocess.Popen([str(args.qemu.resolve()), '-M', 'b310e,boot-mode=rockbox', '-S',
                              '-display', os.environ.get('B310E_QEMU_DISPLAY', 'none'), '-serial', 'none',
                              '-monitor', 'none',
                              '-drive', f'file={image.as_posix()},format=raw,if=none,id=os,readonly=on',
                              '-drive', f'file={img.as_posix()},format=raw,if=none,id=sdcard',
                              '-qmp', f'tcp:127.0.0.1:{qp},server=on,wait=off',
                              '-qtest', f'tcp:127.0.0.1:{tp},server=on,wait=off'], env=env,
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        q = t = None
        try:
            for _ in range(100):
                try:
                    q, t = capture.QMP(qp), QT(tp)
                    break
                except OSError:
                    time.sleep(0.1)
            assert q and t, 'QEMU did not start'
            for i, v in enumerate((12, 40, 17, planted_day)):
                t.write(RTC + 0x10 + 4 * i, v)
            t.write(RTC + 0x38, 0xf00)
            planted = [t.read(RTC + 4 * i) & 0xffff for i in range(4)]
            assert planted[3] == planted_day, planted
            q.command('cont')
            wait_for(t, base, lambda s: s['ui_depth'] == 1, 30, 'no UI')
            end = time.monotonic() + 6
            while (t.read(RTC + 12) & 0xffff) != want_day and time.monotonic() < end:
                time.sleep(0.1)
            got = t.read(RTC + 12) & 0xffff
            result[name] = {'planted_day': planted[3], 'day_after_boot': got, 'wanted': want_day}
            assert got == want_day, result[name]
            time.sleep(2.5)                    # the log reaches the card
            q.command('quit')
            p.wait(timeout=20)
        finally:
            if t:
                t.close()
            if q:
                q.close()
            if p.poll() is None:
                p.terminate()
                p.wait(timeout=10)
        text = img.read_bytes()
        assert b'cannot be right' in text, f'{name}: not in the log'

    garbage('garbage_saved_good', saved_day * 86400 + 12 * 3600, saved_day)
    # 2039: within 30 years of the build, but years past the saved time
    garbage('garbage_near', saved_day * 86400 + 12 * 3600, saved_day, 0x5555)
    built = (args.build / 'build_info.h').read_text().split('"')[1]
    build_day = days(int(built[:4]), int(built[5:7]), int(built[8:10]))
    garbage('garbage_saved_bad', 0xaaaa * 86400 + 3600, build_day)

    text = card.read_bytes()
    lines = [l.decode(errors='replace') for l in text.split(b'\n') if b'rtc: set' in l]
    result['log'] = lines
    assert any('rtc: set 2026-11-07 14:04:10 (day 17112): ok, ack 0xf00' in l for l in lines), lines
    assert any('rtc: set 2028-02-29' in l and ': ok' in l for l in lines), lines
    (out / 'result.json').write_text(json.dumps(result, indent=2))
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
