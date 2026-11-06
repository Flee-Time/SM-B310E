#!/usr/bin/env python3
"""Boot the B310E OS in QEMU and check milestone 1.

Reads the OS's `os_status` block (address from the ELF symbol table)
through QTest while the guest runs:
  - the kernel self-test passes (sleep, semaphore, priority inheritance,
    queue, heap);
  - the tick runs and the CPU is mostly idle;
  - injected key presses arrive as down/up events;
  - holding END for 2.5 s reboots through the watchdog;
and saves screenshots.

SPDX-License-Identifier: GPL-2.0-or-later
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import runpy
import shutil
import socket
import struct
import subprocess
import sys
import time

REPO = Path(__file__).resolve().parents[2]
QT = runpy.run_path(str(REPO / 'emulator/qemu/scripts/test-audio.py'))['QTest']
spec = importlib.util.spec_from_file_location(
    'capture', REPO / 'emulator/qemu/scripts/capture-stock.py')
capture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(capture)

FIELDS = ('magic ticks idle_permille selftest_done selftest_pass pi_wait_ms '
          'sem_rounds_per_s keys last_key worker_loops heap_free stray_irqs cycles idle_cycles').split()
MAGIC = 0x534f3342


def free_port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


def symbol(elf, name):
    nm = shutil.which('arm-none-eabi-nm') or 'D:/Toolchains/arm-none-eabi/bin/arm-none-eabi-nm.exe'
    for line in subprocess.run([nm, str(elf)], capture_output=True, text=True,
                               check=True).stdout.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] == name:
            return int(parts[0], 16)
    raise KeyError(name)


def status(qt, base):
    return {name: qt.read(base + 4 * i) for i, name in enumerate(FIELDS)}


def wait_for(qt, base, test, seconds, what):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        s = status(qt, base)
        if test(s):
            return s
        time.sleep(0.1)
    raise AssertionError(f'{what}: {json.dumps(status(qt, base))}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qemu', type=Path, default=REPO / 'build/qemu/build/qemu-system-arm.exe')
    parser.add_argument('--elf', type=Path, default=REPO / 'build/os/b310e-os.elf')
    parser.add_argument('--image', type=Path, default=REPO / 'build/os/b310e-os.bin')
    parser.add_argument('--output', type=Path, default=REPO / 'build/os/test-boot')
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    base = symbol(args.elf, 'os_status')

    qmp_port, qtest_port = free_port(), free_port()
    command = [str(args.qemu.resolve()), '-M', 'b310e,boot-mode=rockbox',
               '-display', 'none', '-serial', 'none', '-monitor', 'none',
               '-drive', f'file={args.image.resolve().as_posix()},format=raw,if=none,id=os,readonly=on',
               '-qmp', f'tcp:127.0.0.1:{qmp_port},server=on,wait=off',
               '-qtest', f'tcp:127.0.0.1:{qtest_port},server=on,wait=off',
               '-global', 'sc6530_adi.watchdog=on',
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

            s = wait_for(qt, base, lambda s: s['magic'] == MAGIC, 10, 'OS never started')
            s = wait_for(qt, base, lambda s: s['selftest_done'] == 0x1f, 30, 'self-test incomplete')
            result['selftest_pass'] = hex(s['selftest_pass'])
            result['pi_wait_ms'] = s['pi_wait_ms']
            result['sem_rounds_per_s'] = s['sem_rounds_per_s']
            assert s['selftest_pass'] == 0x1f, f'self-test failed: {s}'

            s1 = wait_for(qt, base, lambda s: s['ticks'] > 0, 5, 'status thread silent')
            time.sleep(2.5)
            s2 = status(qt, base)
            result['ticks_per_s'] = round((s2['ticks'] - s1['ticks']) / 2.5)
            result['idle_permille'] = s2['idle_permille']
            assert s2['ticks'] > s1['ticks'], 'tick stopped'
            assert s2['idle_permille'] > 800, f'not idle: {s2}'
            qmp.command('screendump', {'filename': str(out / 'boot.png'), 'format': 'png'})

            keys0 = s2['keys']
            for key, code in (('5', 0x35), ('ret', 0x0d), ('up', 0x04)):
                qmp.hmp(f'sendkey {key} 100')
                s = wait_for(qt, base, lambda s, k=keys0 + 2: s['keys'] >= k, 5,
                             f'key {key} not seen')
                assert s['last_key'] & 0xff == code, (key, hex(s['last_key']))
                keys0 = s['keys']
            result['keys'] = keys0

            # '5' turned the background load on: idle must drop, the UI stay live
            time.sleep(2.5)
            s = status(qt, base)
            result['idle_permille_loaded'] = s['idle_permille']
            result['worker_loops'] = s['worker_loops']
            assert s['worker_loops'] > 0 and s['idle_permille'] < 100, s
            qmp.command('screendump', {'filename': str(out / 'keys.png'), 'format': 'png'})

            # hold END: reboot (QEMU's tick runs slower than real time on
            # Windows, so hold well past the OS's 2 s)
            qmp.hmp('sendkey esc 4000')
            time.sleep(6)
            text = (out / 'qemu.log').read_text(errors='replace')
            result['rebooted'] = 'watchdog expired' in text
            assert result['rebooted'], 'no watchdog reboot after holding END'
            s = wait_for(qt, base, lambda s: s['magic'] == MAGIC and s['ticks'] < 3000,
                         10, 'OS did not come back after the reboot')
            result['stray_irqs'] = s['stray_irqs']
            qmp.command('quit')
            process.wait(timeout=10)
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
    (out / 'result.json').write_text(json.dumps(result, indent=2))
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
