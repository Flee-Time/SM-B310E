#!/usr/bin/env python3
"""DMA channels left by the firmware before the OS are switched off at boot.

On the phone the stock firmware and the boot menu run before the OS; a
channel they left bound to a codec request woke when the audio code
enabled the DMA clock and wrote samples into the UI's variables (crash
on leaving the Stopwatch; at the next boots the torch came on and the
phone powered off at once). QEMU runs nothing before the OS, so this
test starts it paused, plants such leftovers through QTest (every
channel enabled, pointing at the UI's screen stack, every request bound),
lets it run with a 4-pole headset plugged in (the codec is powered for
the headset check) and checks that:
  - every channel is disabled and every request unbound after boot;
  - the UI's screen stack is intact and the UI answers keys;
  - the torch is off and the machine did not power itself off.

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
MAGIC = boot['MAGIC']
DMA_CH = 0x20101000
DMA_MAP = 0x20102000


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qemu', type=Path, default=REPO / 'build/qemu/build/qemu-system-arm.exe')
    parser.add_argument('--build', type=Path, default=REPO / 'build/os')
    args = parser.parse_args()
    out = (args.build / 'test-dma').resolve()
    out.mkdir(parents=True, exist_ok=True)
    elf, image = args.build / 'b310e-os.elf', args.build / 'b310e-os.bin'
    base, stack, depth = (symbol(elf, n) for n in ('os_status', 'stack', 'depth'))
    card = out / 'card.img'
    subprocess.run([sys.executable, str(REPO / 'ports/rockbox/tests/make-sd-image.py'),
                    '--fat32', '--size-mib', '64', '--seconds', '1', '--force',
                    '--runtime', str(REPO / 'sdcard/.rockbox'),
                    '--rockbox', str(REPO / 'sdcard/payloads/rockbox.bin'),
                    '--output', str(card)], check=True, capture_output=True)
    qmp_port, qtest_port = free_port(), free_port()
    cmd = [str(args.qemu.resolve()), '-M', 'b310e,boot-mode=rockbox', '-S',
           '-display', 'none', '-serial', 'none', '-monitor', 'none',
           '-drive', f'file={image.resolve().as_posix()},format=raw,if=none,id=os,readonly=on',
           '-drive', f'file={card.as_posix()},format=raw,if=none,id=sdcard',
           '-qmp', f'tcp:127.0.0.1:{qmp_port},server=on,wait=off',
           '-qtest', f'tcp:127.0.0.1:{qtest_port},server=on,wait=off',
           '-global', 'sc6530_aux.headset-present=on', '-global', 'sc6530_adi.headset-mic=on',
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
        # the firmware's leftovers: every channel on, aimed at the UI's stack
        for n in range(32):
            ch = DMA_CH + n * 0x40
            qt.write(ch + 16, 0x04000000)       # source
            qt.write(ch + 20, depth)            # destination: the UI's variables
            qt.write(ch + 24, 0x50300140)
            qt.write(ch + 28, 64)
            qt.write(ch + 8, 0x3001)            # enabled
        for r in range(1, 65):
            qt.write(DMA_MAP + 4 * (r - 1), (r % 32) + 1)
        result['planted'] = hex(qt.read(DMA_CH + 8))
        qmp.command('cont')
        wait_for(qt, base, lambda s: s['magic'] == MAGIC, 10, 'OS never started')
        wait_for(qt, base, lambda s: s['ui_depth'] == 1, 60, 'no UI')
        time.sleep(3)                           # the headset check powers the codec
        enabled = [n for n in range(32) if qt.read(DMA_CH + n * 0x40 + 8) & 1]
        bound = [r for r in range(1, 65) if qt.read(DMA_MAP + 4 * (r - 1))]
        result.update({'channels_enabled': enabled, 'requests_bound': bound,
                       'depth': qt.read(depth), 'stack0': hex(qt.read(stack))})
        assert not enabled and not bound, result
        assert result['depth'] == 1 and qt.read(stack) >> 24 == 0x04, result
        k0 = status(qt, base)['keys']
        qmp.hmp('sendkey ret 100')
        wait_for(qt, base, lambda s: string(qt, s['ui_title']) == 'Menu', 5, 'UI did not answer')
        result['keys'] = status(qt, base)['keys'] - k0
        result['torch_gpio7'] = bool(qt.read(0x8a000000) & 0x80)
        assert not result['torch_gpio7'], result
        assert p.poll() is None, 'the machine powered off'
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
