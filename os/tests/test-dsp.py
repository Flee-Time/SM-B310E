#!/usr/bin/env python3
"""Boot the DSP through the OS's Tools / DSP boot screen in QEMU.

QEMU does not run the DSP; its sc6530_dsp model answers the boot ROM's
share-memory handshake (DSP_READY, READY_TO_COPY, COPY_DONE and the DSP
interrupt per block). For each case the test boots the OS, opens
Tools / DSP boot, waits for the attempt to finish and reads the driver's
`dsp_info` and the screen's `dspboot_status` (ELF symbols) through QTest:
  - no NOR drive (zero-filled NOR): "no DSP image", nothing touched, quick;
  - with --nor (the owner's own NOR dump, never part of the repository):
    the image is found and decoded, the whole program (66861 halfwords,
    66 blocks) goes through the handshake, the DSP is started and then
    held in reset again; the model's trace shows 66 copies;
  - Center runs it again, and the UI stays alive (power-off works);
  - dsp_probe() ends every time, and since the model runs no DSP program it
    must not report one: no records taken, no replies, no messages, no frames
    and no dump. The model does answer the ping (a shortcut for the stock
    image), which is why the verdict never counts the ping alone. Whether a
    real DSP answers is for the phone: /b310e-os/dsp-report.txt on its card.

SPDX-License-Identifier: GPL-2.0-or-later
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import re
import runpy
import shutil
import socket
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
          'sem_rounds_per_s keys last_key worker_loops heap_free stray_irqs cycles idle_cycles '
          'sd_code fs_mounted fs_free_kib sd_retries sd_failures fs_write_kib_per_s '
          'fs_read_kib_per_s ui_depth ui_title ui_list_count').split()
DSP_FIELDS = ('state error stage blocks program_words boot_ms irqs irq_storms records '
              'cmds frames_posted fi_out fi_moves pm_results ready_word').split()
MAGIC = 0x534f3342
ST_ALL = 0x3f
NOR_SIZE = 8 << 20
PROBE_FIELDS = ('phase early_words early_first records_total records_taken cmd_in cmd_out '
                'int_sts replies msgs msg_first msg_last idle_posted idle_consumed frame_us '
                'ping_before ping_after ping_ms dump_blocks dump_nonzero').split()
SELFTEST_FIELDS = 'result mark0 mark1 echo busy idle mcu_ctl0 dsp_ctl0'.split()
IRAM_BANKS = 7 << 19
PROBE_DONE = 6
PROBE_TRACE_END = 109       # word index: after reply[16][5] and three trace words
DSP_OFF, DSP_FAILED = 0, 3
DSP_EIMAGE = -1
PROGRAM_HW, BLOCKS = 66861, 66


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


def signed(v):
    return v - (1 << 32) if v & 0x80000000 else v


def string(qt, addr, limit=64):
    out = b''
    while addr and len(out) < limit:
        word = qt.read(addr & ~3).to_bytes(4, 'little')[addr & 3:]
        if b'\0' in word:
            return (out + word.split(b'\0')[0]).decode(errors='replace')
        out += word
        addr = (addr & ~3) + 4
    return out.decode(errors='replace')


def run(args, name, nor):
    out = args.output / name
    out.mkdir(parents=True, exist_ok=True)
    base = symbol(args.elf, 'os_status')
    info = symbol(args.elf, 'dsp_info')
    screen = symbol(args.elf, 'dspboot_status')
    probe = symbol(args.elf, 'dsp_probe_info')
    own = symbol(args.elf, 'dsp_selftest_info')
    card = out / 'card.img'
    settings = out / 'settings.txt'                 # DSP boot shows only in developer mode
    settings.write_text('dev_mode=1\n', newline='\n')
    subprocess.run([sys.executable, str(REPO / 'ports/rockbox/tests/make-sd-image.py'),
                    '--fat32', '--size-mib', '64', '--seconds', '1', '--force',
                    '--runtime', str(REPO / 'sdcard/.rockbox'),
                    '--rockbox', str(REPO / 'sdcard/payloads/rockbox.bin'),
                    '--add', f'b310e-os/settings.txt={settings}',
                    '--output', str(card)], check=True, capture_output=True)
    trace = out / 'qemu.log'
    qmp_port, qtest_port = free_port(), free_port()
    command = [str(args.qemu.resolve()), '-M', 'b310e,boot-mode=rockbox',
               '-display', os.environ.get('B310E_QEMU_DISPLAY', 'none'), '-serial', 'none', '-monitor', 'none',
               '-drive', f'file={args.image.resolve().as_posix()},format=raw,if=none,id=os,readonly=on',
               '-drive', f'file={card.as_posix()},format=raw,if=none,id=sdcard',
               '-qmp', f'tcp:127.0.0.1:{qmp_port},server=on,wait=off',
               '-qtest', f'tcp:127.0.0.1:{qtest_port},server=on,wait=off',
               '-global', 'sc6530_adi.watchdog=on',
               '--trace', 'sc6530_dsp_*', '-D', str(trace)]
    if nor:
        image = out / 'nor.bin'             # the owner's dump, local only
        image.write_bytes(nor[:NOR_SIZE])
        command += ['-drive', f'file={image.as_posix()},format=raw,if=none,id=nor,readonly=on']
    env = os.environ.copy()
    if os.name == 'nt':
        env['PATH'] = r'D:\Toolchains\msys64\mingw64\bin;C:\msys64\mingw64\bin;' + env.get('PATH', '')
    result = {'case': name}
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

            def dsp():
                d = {f: qt.read(info + 4 * i) for i, f in enumerate(DSP_FIELDS)}
                d['error'] = signed(d['error'])
                d['runs'] = qt.read(screen)
                d['busy'] = qt.read(screen + 4)
                d['result'] = signed(qt.read(screen + 8))
                d['probe'] = {f: qt.read(probe + 4 * i) for i, f in enumerate(PROBE_FIELDS)}
                d['probe']['trace_end'] = qt.read(probe + 4 * PROBE_TRACE_END)
                d['selftest'] = {
                    n: {f: qt.read(own + 4 * (at + i)) for i, f in enumerate(SELFTEST_FIELDS)}
                    for n, at in (('set', 0), ('clear', len(SELFTEST_FIELDS)))}
                return d

            def wait(test, seconds, what):
                end = time.monotonic() + seconds
                while time.monotonic() < end:
                    if test():
                        return
                    time.sleep(0.1)
                raise AssertionError(f'{what}: {title()!r} {json.dumps(dsp())}')

            def shot(n):
                qmp.command('screendump', {'filename': str(out / f'{n}.png'), 'format': 'png'})

            def key(k, hold=100):
                qmp.hmp(f'sendkey {k} {hold}')

            def press(k, want, seconds=6):
                key(k)
                wait(lambda: title() == want, seconds, f'after {k}: wanted {want!r}')
                time.sleep(0.4)

            def open_entry(parent):
                key('ret')
                end = time.monotonic() + 4
                while time.monotonic() < end:
                    t = title()
                    if t != parent:
                        time.sleep(0.4)
                        return t
                    time.sleep(0.1)
                return parent

            def search(want, parent, titles):
                for i in range(status()['ui_list_count']):
                    if i:
                        press('down', parent)
                    t = open_entry(parent)
                    titles.append(t)
                    if t == want:
                        return True
                    if t == 'Tools' and parent == 'Menu' and want != 'Power':
                        if search(want, 'Tools', titles):
                            return True
                    if t != parent:
                        press('f2', parent)
                return False

            def find(want):
                press('ret', 'Menu')
                titles = []
                if search(want, 'Menu', titles):
                    return titles
                raise AssertionError(f'no {want} entry in the menu or Tools: {titles}')

            wait(lambda: status()['magic'] == MAGIC, 10, 'OS never started')
            wait(lambda: status()['selftest_done'] == ST_ALL, 60, 'self-test incomplete')
            wait(lambda: status()['ui_depth'] == 1, 10, 'no UI')
            result['menu'] = find('DSP boot')

            for attempt in (1, 2):
                if attempt == 2:
                    key('ret')                  # Center: run again
                wait(lambda: dsp()['runs'] >= attempt and not dsp()['busy'], 30,
                     f'boot attempt {attempt} never finished')
                d = dsp()
                result[f'attempt{attempt}'] = d
                shot(f'dsp-boot-{attempt}')
                if nor:
                    assert d['result'] == 0, d
                    assert d['stage'] == 6, d
                    assert d['blocks'] == BLOCKS and d['program_words'] == PROGRAM_HW, d
                    assert d['ready_word'] & 1, d
                    assert d['state'] == DSP_OFF, d        # held in reset after
                    q = d['probe']
                    assert q['phase'] == PROBE_DONE, d
                    assert q['records_total'] > 0 and q['records_taken'] == 0, d
                    assert q['replies'] == 0 and q['msgs'] == 0, d
                    assert q['idle_consumed'] == 0, d
                    assert q['ping_before'] == 0 and q['ping_after'] == 1, d
                    assert q['dump_blocks'] == 0, d
                    assert q['trace_end'] == 0, d          # no program runs here
                else:
                    assert d['result'] == DSP_EIMAGE and d['stage'] == 0, d
                    assert d['blocks'] == 0 and d['state'] == DSP_FAILED, d
                    assert d['boot_ms'] < 1000, d
                # The model loads our test program too, and runs nothing.
                for n, c in d['selftest'].items():
                    assert c['result'] == 0 and c['mark0'] == 0 and c['busy'] == 0, d
                # IRAM bits as asked, then given back to the ARM.
                assert d['selftest']['set']['mcu_ctl0'] & IRAM_BANKS == IRAM_BANKS, d
                assert d['selftest']['clear']['mcu_ctl0'] & IRAM_BANKS == 0, d
                assert qt.read(0x8b0001a0) & IRAM_BANKS == IRAM_BANKS, 'IRAM bits not restored'
            assert title() == 'DSP boot'

            press('esc', 'B310E OS')
            find('Power')
            press('ret', 'Power off')
            key('f1')
            try:
                process.wait(timeout=15)
                result['powered_off'] = True
            except subprocess.TimeoutExpired:
                result['powered_off'] = False
            assert result['powered_off'], 'no power-off'
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

    tr = trace.read_text(errors='replace')
    copies = len(re.findall(r'SM_COPY', tr))
    boots = len(re.findall(r'SM_BOOT', tr))
    result['trace_copies'] = copies
    result['trace_boots'] = boots
    if nor:
        assert copies >= 2 * BLOCKS, f'{copies} copies in the model trace'
        image = card.read_bytes()
        assert b'verdict ping only' in image, 'no probe report on the card'
    else:
        # only our own two-block self-test ran: 2 cases, 2 attempts
        assert copies < BLOCKS, f'{copies} copies: the DSP was loaded without an image?'
    print('PASS ' + json.dumps(result), flush=True)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qemu', type=Path, default=REPO / 'build/qemu/build/qemu-system-arm.exe')
    parser.add_argument('--elf', type=Path, default=REPO / 'build/os/b310e-os.elf')
    parser.add_argument('--image', type=Path, default=REPO / 'build/os/b310e-os.bin')
    parser.add_argument('--nor', type=Path,
                        help="the phone's own NOR dump (8 or 16 MB); without it only "
                             'the no-image case runs')
    parser.add_argument('--output', type=Path, default=REPO / 'build/os/test-dsp')
    args = parser.parse_args()
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    results = [run(args, 'no-image', None)]
    if args.nor:
        results.append(run(args, 'nor', args.nor.read_bytes()))
    (args.output / 'results.json').write_text(json.dumps(results, indent=2))


if __name__ == '__main__':
    main()
