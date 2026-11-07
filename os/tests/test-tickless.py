#!/usr/bin/env python3
"""Tickless idle in QEMU: few wake-ups at rest, and time kept.

On the idle Home screen the kernel lets the timer run to the next timeout
instead of interrupting every millisecond, so the interrupts per second
must be far below the tick's 1000. The kernel's milliseconds must keep
pace with the RTC model (which counts QEMU's virtual seconds) over ten
seconds, so the long timer periods neither lose nor gain time. Then the
Deep idle switch (Settings) turns it off and the tick comes back.

With the tick, QEMU's timer itself falls behind: its periodic ptimer
(PTIMER_POLICY_LEGACY) starts each period when the host runs the callback,
about 0.25 ms late on Windows, so at 1000 periods a second the kernel gets
some 740 ms per RTC second (2026-11-07). The reload on the chip is in
hardware, so that ratio is recorded but not checked.

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
symbol, status, string, wait_for, free_port = (boot[n] for n in
                                               ('symbol', 'status', 'string', 'wait_for', 'free_port'))
MENU, MAGIC = boot['MENU'], boot['MAGIC']
RTC = 0x82001600


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qemu', type=Path, default=REPO / 'build/qemu/build/qemu-system-arm.exe')
    parser.add_argument('--build', type=Path, default=REPO / 'build/os')
    args = parser.parse_args()
    out = (args.build / 'test-tickless').resolve()
    out.mkdir(parents=True, exist_ok=True)
    elf, image = args.build / 'b310e-os.elf', args.build / 'b310e-os.bin'
    base, irq_count, ticks = (symbol(elf, n) for n in ('os_status', 'irq_count', 'ticks'))
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

        def seconds_of_day():
            v = [qt.read(RTC + 4 * i) & 0xffff for i in range(4)]
            return v[3] * 86400 + v[2] * 3600 + v[1] * 60 + v[0]

        def measure(span):
            """(interrupts per kernel second, kernel ms per RTC second)."""
            # start on an RTC second boundary
            s0 = seconds_of_day()
            while seconds_of_day() == s0:
                time.sleep(0.01)
            r0, i0, t0 = seconds_of_day(), qt.read(irq_count), qt.read(ticks)
            while seconds_of_day() - r0 < span:
                time.sleep(0.02)
            r1, i1, t1 = seconds_of_day(), qt.read(irq_count), qt.read(ticks)
            return ((i1 - i0) * 1000 / max(t1 - t0, 1), (t1 - t0) / (r1 - r0))

        wait_for(qt, base, lambda s: s['magic'] == MAGIC, 10, 'OS never started')
        wait_for(qt, base, lambda s: s['ui_depth'] == 1, 60, 'no UI')
        time.sleep(2)
        irqs, ms_per_s = measure(10)
        result.update({'irqs_per_s': round(irqs), 'kernel_ms_per_rtc_s': round(ms_per_s, 1)})
        assert irqs < 300, f'still ticking: {result}'
        assert 970 <= ms_per_s <= 1030, f'time not kept: {result}'

        # Settings -> Deep idle off: the 1 ms tick again
        def press(k, want):
            qmp.hmp(f'sendkey {k} 100')
            wait_for(qt, base, lambda s: string(qt, s['ui_title']) == want, 6, f'not {want}')
            time.sleep(0.4)
        press('ret', 'Menu')
        for _ in range(MENU.index('Settings')):
            press('down', 'Menu')
        press('ret', 'Settings')
        for _ in range(boot['SETTINGS'].index('Power')):
            press('down', 'Settings')
        press('ret', 'Power')               # the Power page: CPU speed, Deep idle
        press('down', 'Power')
        press('ret', 'Power')               # Deep idle off
        irqs, ms_per_s = measure(4)
        result.update({'irqs_per_s_ticking': round(irqs), 'kernel_ms_per_rtc_s_ticking': round(ms_per_s, 1)})
        assert irqs >= 900, f'deep idle switch had no effect: {result}'
        press('ret', 'Power')               # and on again
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
    (out / 'result.json').write_text(json.dumps(result, indent=2))
    print('PASS ' + json.dumps(result))


if __name__ == '__main__':
    main()
