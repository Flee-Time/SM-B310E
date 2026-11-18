#!/usr/bin/env python3
"""An app's store into the NOR window in QEMU: the OS goes on.

The MMU maps 0x00000000..0x03ffffff (the NOR window) read-only. An app's
thread that stores there (a NULL pointer's store) is not the end of the OS:
the thread goes to the handler it set with app_on_fault (2.16), or ends.
os/tests/faultapp.c is the app: its thread sets a handler and stores to
0x100; the handler stores to 0x200, which ends the thread. Checked: the app's
fault_stage is 2 (the handler ran, nothing after either store did), the log
names both stores and the thread's end, the OS's clock goes on, and Close
still closes the app and unloads it. Then a fault outside an app's thread (an
undefined instruction from the app's key handler, in the UI's thread): the
fault screen restarts the phone by itself (20 s), and the next boot's log has
the kept registers, the pc as FaultTest+offset. The NOR holds the boot loader's
header, so the fault leaves the loader's menu request (0xb1 in the high byte
of analog 0x82001148; QEMU's reset reloads the OS directly and nothing takes
it). Then the same fault with the screen dark (# in the app: from its tick
once the panel sleeps): the fault screen wakes the panel and lights it, and
the next boot logs that fault too (2026-11-18).

  make PYTHON=python ../build/os/b310e-os.bin ../build/os/tests/faultapp.app  (from os/)
  python os/tests/test-fault.py

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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qemu', type=Path, default=REPO / 'build/qemu/build/qemu-system-arm.exe')
    parser.add_argument('--build', type=Path, default=REPO / 'build/os')
    args = parser.parse_args()
    out = (args.build / 'test-fault').resolve()
    out.mkdir(parents=True, exist_ok=True)
    elf, image = args.build / 'b310e-os.elf', args.build / 'b310e-os.bin'
    app = args.build / 'tests/faultapp.app'
    if not app.is_file():
        sys.exit('test-fault: no faultapp.app (make PYTHON=python ../build/os/tests/faultapp.app in os/)')
    base, apps_list = symbol(elf, 'os_status'), symbol(elf, 'apps')
    stage_o = symbol(args.build / 'tests/faultapp.elf', 'fault_stage')
    card = out / 'card.img'
    subprocess.run([sys.executable, str(REPO / 'ports/rockbox/tests/make-sd-image.py'),
                    '--fat32', '--size-mib', '64', '--seconds', '1', '--force',
                    '--runtime', str(REPO / 'sdcard/.rockbox'),
                    '--rockbox', str(REPO / 'sdcard/payloads/rockbox.bin'),
                    '--add', f'b310e-os/apps/faultapp.app={app.resolve()}',
                    '--output', str(card)], check=True, capture_output=True)
    nor = out / 'nor.img'                # zeros, and the loader's header magic
    img = bytearray(4 << 20)
    img[0x3fc008:0x3fc010] = b'B310BOOT'
    nor.write_bytes(bytes(img))
    qmp_port, qtest_port = free_port(), free_port()
    cmd = [str(args.qemu.resolve()), '-M', 'b310e,boot-mode=rockbox',
           '-display', os.environ.get('B310E_QEMU_DISPLAY', 'none'), '-serial', 'none', '-monitor', 'none',
           '-drive', f'file={image.resolve().as_posix()},format=raw,if=none,id=os,readonly=on',
           '-drive', f'file={card.as_posix()},format=raw,if=none,id=sdcard',
           '-qmp', f'tcp:127.0.0.1:{qmp_port},server=on,wait=off',
           '-qtest', f'tcp:127.0.0.1:{qtest_port},server=on,wait=off',
           '-audiodev', 'none,id=audio0', '-global', 'sc6530_adi.audiodev=audio0',
           '-global', 'sc6530_adi.watchdog=on',
           '-drive', f'file={nor.as_posix()},format=raw,if=none,id=nor,readonly=on']
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

        def key(k, want=None, wait=0.35):
            qmp.hmp(f'sendkey {k} {200 if k == "esc" else 100}')
            if want:
                wait_for(qt, base, lambda s: string(qt, s['ui_title']) == want, 6, f'not {want}')
            time.sleep(wait)

        def stage():
            a = qt.read(apps_list)
            return qt.read(qt.read(a + 8) + stage_o) if a else None

        wait_for(qt, base, lambda s: s['magic'] == MAGIC, 10, 'OS never started')
        wait_for(qt, base, lambda s: s['ui_depth'] == 1, 60, 'no UI')
        time.sleep(1)
        key('esc', 'B310E OS', 0.5)
        key('ret', 'Menu', 0.6)
        key('ret', 'Apps', 0.6)
        key('ret', 'Fault test', 1.0)
        t0 = time.time()
        while stage() not in (2, 98, 99) and time.time() - t0 < 10:
            time.sleep(0.2)
        time.sleep(1.0)                 # (anything after the second store would show)
        result['stage'] = stage()
        before = status(qt, base)['ticks']
        time.sleep(1.5)
        result['ticks_on'] = status(qt, base)['ticks'] - before
        key('f2', 'Apps', 1.0)          # Close: the app goes (its thread has ended)
        wait_for(qt, base, lambda s: s['apps_loaded'] == 0, 10, 'the app stayed loaded')
        result['unloaded'] = True
        # A fault outside an app's thread: the fault screen keeps the registers
        # (FAULT_BLOCK), the watchdog restarts the phone, the next boot logs them
        key('ret', 'Fault test', 1.0)
        qmp.hmp('sendkey f1 100')       # (the first may only wake the screen)
        time.sleep(0.5)
        if status(qt, base)['ticks'] > 3000:
            qmp.hmp('sendkey f1 100')
        t0 = time.time()
        while time.time() - t0 < 40 and status(qt, base)['ticks'] > 3000:
            time.sleep(0.5)
        result['restarted_after_s'] = round(time.time() - t0, 1)
        qmp.command('screendump', {'filename': str(out / 'fault.png'), 'format': 'png'})
        wait_for(qt, base, lambda s: s['ui_depth'] == 1, 60, 'no UI after the restart')
        result['request_after_fault'] = hex(qt.read(0x82001148) >> 8 & 0xff)
        qt.write(0x82001148, qt.read(0x82001148) & 0xff)     # (the loader takes it)
        time.sleep(2)
        qmp.command('screendump', {'filename': str(out / 'last.png'), 'format': 'png'})
        # The same fault with the screen dark
        key('esc', 'B310E OS', 0.5)
        key('ret', 'Menu', 0.6)
        key('ret', 'Apps', 0.6)
        key('ret', 'Fault test', 1.0)
        key('slash', 'Fault test', 0.5)  # # arms it
        t0 = time.time()
        while time.time() - t0 < 90 and status(qt, base)['ui_light'] != 2:
            time.sleep(0.2)
        result['went_dark_after_s'] = round(time.time() - t0, 1)
        time.sleep(2)                   # the fault, the panel's 121 ms, the drawing
        led = qt.read(0x82001220)
        result['led_on_fault_dark'] = hex(led)
        qmp.command('screendump', {'filename': str(out / 'fault-dark.png'), 'format': 'png'})
        t0 = time.time()
        while time.time() - t0 < 40 and status(qt, base)['ticks'] > 3000:
            time.sleep(0.5)
        result['restarted_dark_after_s'] = round(time.time() - t0, 1)
        wait_for(qt, base, lambda s: s['ui_depth'] == 1, 60, 'no UI after the second restart')
        result['request_after_dark_fault'] = hex(qt.read(0x82001148) >> 8 & 0xff)
        time.sleep(2)
    finally:
        if qmp:
            qmp.close()
        if p.poll() is None:
            p.terminate()
            p.wait()
    log = (card_file(card.read_bytes(), '/b310e-os/log') or b'').decode(errors='replace')
    result['log'] = [l.strip() for l in log.splitlines() if 'FaultTest' in l or 'faulttest' in l]
    print(json.dumps(result, indent=2))
    assert result['stage'] == 2, result
    assert result['ticks_on'] >= 1000, result      # (the status ticks once a second)
    lines = '\n'.join(result['log'])
    assert 'a store to 0x00000100' in lines, result
    assert 'handler: It wrote to 0x00000100' in lines, result
    assert 'a store to 0x00000200' in lines, result
    assert 'its thread faulttest ends' in lines, result
    kept = [l for l in log.splitlines() if 'the last run stopped on a fault' in l]
    result['kept'] = kept
    assert kept and 'undefined instruction' in kept[0] and '(FaultTest+0x' in kept[0], result
    assert result['restarted_after_s'] < 30, result
    assert result['request_after_fault'] == '0xb1', result
    assert result['request_after_dark_fault'] == '0xb1', result
    assert len(kept) >= 2 and 'undefined instruction' in kept[1], result
    assert int(result['led_on_fault_dark'], 16) & 0x60 == 0x40, result     # lit (stock: bit 6 on)
    print('test-fault: ok')


if __name__ == '__main__':
    main()
