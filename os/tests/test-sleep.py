#!/usr/bin/env python3
"""Tools -> Sleep test, and the dark screen's headset, in QEMU.

QEMU does not model the chip's sleep: timer 0 and the stop bit do nothing,
so this checks only that the test's steps run and come back (timer 2 ends
each after 3 s), that the kernel's time and the UI go on afterwards, and that
each step's lines reach the card's log. Whether the chip really stops shows on
the phone, in the same lines.

Dark at 26 MHz the idle loop takes the system sleep and clears the PLL's enable
bit; lit or held up neither, and the bit is back before the clock goes up. A
first read of the analog die with it clear sets it again.

With a 4-pole headset in, the codec stays on for its buttons while the screen
is lit; dark and silent it goes off, so the clock can drop to 26 MHz, and the
buttons work again after waking.

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
MENU, MAGIC = boot['MENU'], boot['MAGIC']
KEY_HS_CENTER = 0x70


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qemu', type=Path, default=REPO / 'build/qemu/build/qemu-system-arm.exe')
    parser.add_argument('--build', type=Path, default=REPO / 'build/os')
    args = parser.parse_args()
    out = (args.build / 'test-sleep').resolve()
    out.mkdir(parents=True, exist_ok=True)
    elf, image = args.build / 'b310e-os.elf', args.build / 'b310e-os.bin'
    base, codec = symbol(elf, 'os_status'), symbol(elf, 'hs_powered')
    sleeps = symbol(elf, 'sleep_idle_count')
    PLL_CTL = 0x8b000020
    settings = out / 'settings.txt'
    settings.write_text('screen_timeout=15\n')
    card = out / 'card.img'
    subprocess.run([sys.executable, str(REPO / 'ports/rockbox/tests/make-sd-image.py'),
                    '--fat32', '--size-mib', '64', '--seconds', '1', '--force',
                    '--runtime', str(REPO / 'sdcard/.rockbox'),
                    '--rockbox', str(REPO / 'sdcard/payloads/rockbox.bin'),
                    '--add', f'b310e-os/settings.txt={settings}',
                    '--output', str(card)], check=True, capture_output=True)
    qmp_port, qtest_port = free_port(), free_port()
    cmd = [str(args.qemu.resolve()), '-M', 'b310e,boot-mode=rockbox',
           '-display', os.environ.get('B310E_QEMU_DISPLAY', 'none'), '-serial', 'none', '-monitor', 'none',
           '-drive', f'file={image.resolve().as_posix()},format=raw,if=none,id=os,readonly=on',
           '-drive', f'file={card.as_posix()},format=raw,if=none,id=sdcard',
           '-global', 'sc6530_aux.headset-present=on', '-global', 'sc6530_adi.headset-mic=on',
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

        def title(want, seconds=6):
            wait_for(qt, base, lambda s: string(qt, s['ui_title']) == want, seconds, f'not {want}')

        def press(k, want):
            qmp.hmp(f'sendkey {k} 100')
            title(want)
            time.sleep(0.4)

        wait_for(qt, base, lambda s: s['magic'] == MAGIC, 10, 'OS never started')
        wait_for(qt, base, lambda s: s['ui_depth'] == 1, 60, 'no UI')
        time.sleep(1.5)                     # the headset check: 400 ms
        s = wait_for(qt, base, lambda s: s['audio_icon'] == 2, 4, 'no headset symbol')
        result['codec_for_buttons'] = qt.read(codec) & 0xff
        assert result['codec_for_buttons'] == 1, result

        # Dark: the codec off, the clock down
        wait_for(qt, base, lambda s: s['ui_light'] == 2, 40, 'never went dark')
        s = wait_for(qt, base, lambda s: s['cpu_mhz'] == 26, 8, 'not 26 MHz, dark with a headset')
        result['mhz_dark_headset'] = s['cpu_mhz']
        result['codec_dark'] = qt.read(codec) & 0xff
        assert result['codec_dark'] == 0, result
        # The idle loop's system sleep, and the kernel's time going on through it
        c0, t0 = qt.read(sleeps), status(qt, base)['ticks']
        time.sleep(3)
        result['idle_sleeps_in_3_s'] = qt.read(sleeps) - c0
        result['ms_in_3_s_dark'] = status(qt, base)['ticks'] - t0
        assert result['idle_sleeps_in_3_s'] > 0, result
        assert result['ms_in_3_s_dark'] > 1500, result
        reads = [qt.read(PLL_CTL) & 1 for _ in range(200)]
        result['pll_off_reads_of_200'] = reads.count(0)
        assert reads.count(0) > 0, result
        starts = symbol(elf, 'pll_starts')
        result['pll_starts_dark'] = qt.read(starts)
        assert result['pll_starts_dark'] > 0, result    # the UI's analog reads
        # The first key only wakes; the buttons come back
        qmp.hmp('sendkey down 100')
        wait_for(qt, base, lambda s: s['ui_light'] == 0, 5, 'no wake')
        time.sleep(0.6)
        result['codec_woken'] = qt.read(codec) & 0xff
        assert result['codec_woken'] == 1, result
        qmp.hmp('sendkey h 300')
        s = wait_for(qt, base, lambda s: (s['last_key'] & 0xff) == KEY_HS_CENTER, 5,
                     'headset button dead after waking')
        result['headset_key'] = hex(s['last_key'])
        time.sleep(0.5)
        c0 = qt.read(sleeps)                # lit: never
        time.sleep(1)
        result['idle_sleeps_lit'] = qt.read(sleeps) - c0
        assert result['idle_sleeps_lit'] == 0, result
        result['pll_lit'] = qt.read(PLL_CTL) & 1
        assert result['pll_lit'] == 1, result

        # Tools -> Sleep test
        press('ret', 'Menu')
        for _ in range(MENU.index('Tools')):
            press('down', 'Menu')
        press('ret', 'Tools')
        for _ in range(5):                  # Torch, SIM, IMEI, USB, Radio status
            press('down', 'Tools')
        t0 = status(qt, base)['ticks']
        qmp.hmp('sendkey ret 100')
        title('System sleep', 15)           # after the core stop
        time.sleep(0.4)
        qmp.hmp('sendkey ret 100')          # Yes
        title('Sleep test', 40)             # the report
        result['ms_for_the_test'] = status(qt, base)['ticks'] - t0
        t1 = status(qt, base)['ticks']
        time.sleep(2)
        result['ms_in_2_s_after'] = status(qt, base)['ticks'] - t1
        assert result['ms_in_2_s_after'] > 1000, result
        press('f2', 'Tools')                # the UI answers
        qmp.hmp('quit')
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
    log = (card_file(card.read_bytes(), '/b310e-os/log.txt') or b'').decode(errors='replace')
    lines = [x for x in log.splitlines() if 'sleep test' in x]
    (out / 'sleep-log.txt').write_text('\n'.join(lines))
    result['log_lines'] = len(lines)
    for want in ('analog id', 'core stop: start', 'system: start', 'system, 26 MHz: start',
                 'states, DSP in reset', 'PLL at 26 MHz: control', 'states, DSP out of reset, no program',
                 'DSP out of reset, gates, T2 on: start', 'DSP program: boot', 'after the last'):
        assert any(want in x for x in lines), (want, lines)
    done = [x for x in lines if 'by the counter' in x]
    assert len(done) >= 7, lines
    print('\n'.join(done))
    print('PASS ' + json.dumps(result))


if __name__ == '__main__':
    main()
