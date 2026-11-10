#!/usr/bin/env python3
"""An app's store into the NOR window in QEMU: the OS goes on.

The MMU maps 0x00000000..0x03ffffff (the NOR window) read-only. An app's
thread that stores there (a NULL pointer's store) is not the end of the OS:
the thread goes to the handler it set with app_on_fault (2.16), or ends.
os/tests/faultapp.c is the app: its thread sets a handler and stores to
0x100; the handler stores to 0x200, which ends the thread. Checked: the app's
fault_stage is 2 (the handler ran, nothing after either store did), the log
names both stores and the thread's end, the OS's clock goes on, and Close
still closes the app and unloads it.

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
    qmp_port, qtest_port = free_port(), free_port()
    cmd = [str(args.qemu.resolve()), '-M', 'b310e,boot-mode=rockbox',
           '-display', os.environ.get('B310E_QEMU_DISPLAY', 'none'), '-serial', 'none', '-monitor', 'none',
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
        qmp.command('screendump', {'filename': str(out / 'last.png'), 'format': 'png'})
    finally:
        if qmp:
            qmp.close()
        if p.poll() is None:
            p.terminate()
            p.wait()
    log = (card_file(card.read_bytes(), '/b310e-os/log.txt') or b'').decode(errors='replace')
    result['log'] = [l.strip() for l in log.splitlines() if 'FaultTest' in l or 'faulttest' in l]
    print(json.dumps(result, indent=2))
    assert result['stage'] == 2, result
    assert result['ticks_on'] >= 1000, result      # (the status ticks once a second)
    lines = '\n'.join(result['log'])
    assert 'a store to 0x00000100' in lines, result
    assert 'handler: It wrote to 0x00000100' in lines, result
    assert 'a store to 0x00000200' in lines, result
    assert 'its thread faulttest ends' in lines, result
    print('test-fault: ok')


if __name__ == '__main__':
    main()
