#!/usr/bin/env python3
"""Read the emulated SIM cards through the OS's SIM screen in QEMU.

The QEMU machine models the SIM controller (0x85000000) and a GSM test
card (emulator/qemu/machine/hw/misc/sc6530_sim.c); -global
sc6530_sim.sim-slots=<mask> inserts it. For each case the test boots the
OS, finds "SIM" in the main menu, waits for both slots to be read and
reads the screen's result block (ELF symbol `sim_app_status`, apps/sim.c)
through QTest:
  - the card's slot: an ATR at 1.8 V, T=0, 21 bytes (the model's ATR,
    whose TCK is flagged as wrong), SELECT MF answered 9F0F; the empty
    slot: no card;
  - Center reads again;
  - only SELECT (A4) reaches the card (the model's APDU trace): nothing
    reads the ICCID or the IMSI;
then powers off and checks the card log for the sim lines, without the
model's ICCID, IMSI or ATR historical bytes.

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
SLOT_FIELDS = 'state result mv proto protocols atr_len sw flags'.split()
MAGIC = 0x534f3342
ST_ALL = 0x3f
ICCID = '8944110012345678901'
IMSI = '234150123456789'
# The model's ATR (sc6530_sim.c card_reset): T=0 and T=15, 14 historical
# bytes. Its TCK (0xb2) is wrong (the XOR gives 0x47), so the driver
# reports "TCK bad" (F_TCK_BAD) until the model is fixed.
MODEL_ATR = bytes.fromhex('3b9e95801fc78031e073fe211b66d00176211200b2')
ATR_HISTORICAL = MODEL_ATR[6:20].hex()             # must stay out of the log
F_TCK_BAD = 2
SIM_ENOCARD = -1


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


def run(args, name, slots):
    out = args.output / name
    out.mkdir(parents=True, exist_ok=True)
    base = symbol(args.elf, 'os_status')
    try:
        sim = symbol(args.elf, 'sim_app_status')
    except KeyError:
        raise SystemExit('sim_app_status is not in the image: app_sim() is not wired '
                         '(add { "SIM", app_sim } to the main menu)')
    card = out / 'card.img'
    subprocess.run([sys.executable, str(REPO / 'ports/rockbox/tests/make-sd-image.py'),
                    '--fat32', '--size-mib', '64', '--seconds', '1', '--force',
                    '--runtime', str(REPO / 'sdcard/.rockbox'),
                    '--rockbox', str(REPO / 'sdcard/progs/rockbox.bin'),
                    '--output', str(card)], check=True, capture_output=True)
    trace = out / 'qemu.log'
    qmp_port, qtest_port = free_port(), free_port()
    command = [str(args.qemu.resolve()), '-M', 'b310e,boot-mode=rockbox',
               '-display', 'none', '-serial', 'none', '-monitor', 'none',
               '-drive', f'file={args.image.resolve().as_posix()},format=raw,if=none,id=os,readonly=on',
               '-drive', f'file={card.as_posix()},format=raw,if=none,id=sdcard',
               '-qmp', f'tcp:127.0.0.1:{qmp_port},server=on,wait=off',
               '-qtest', f'tcp:127.0.0.1:{qtest_port},server=on,wait=off',
               '-global', 'sc6530_adi.watchdog=on',
               '-global', f'sc6530_sim.sim-slots={slots}',
               '-global', f'sc6530_sim.sim-iccid={ICCID}',
               '-global', f'sc6530_sim.sim-imsi={IMSI}',
               '--trace', 'sc6530_sim_*', '-D', str(trace)]
    env = os.environ.copy()
    if os.name == 'nt':
        env['PATH'] = r'D:\Toolchains\msys64\mingw64\bin;C:\msys64\mingw64\bin;' + env.get('PATH', '')
    result = {'case': name, 'sim_slots': slots}
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

            def report():
                r = {'scans': qt.read(sim), 'busy': qt.read(sim + 4)}
                for s in range(2):
                    r[s] = {f: qt.read(sim + 8 + 32 * s + 4 * i)
                            for i, f in enumerate(SLOT_FIELDS)}
                    for f in ('result', 'sw'):
                        r[s][f] = signed(r[s][f])
                return r

            def wait(test, seconds, what):
                end = time.monotonic() + seconds
                while time.monotonic() < end:
                    if test():
                        return
                    time.sleep(0.1)
                raise AssertionError(f'{what}: {title()!r} {json.dumps(status())}')

            def shot(n):
                qmp.command('screendump', {'filename': str(out / f'{n}.png'), 'format': 'png'})

            def key(k, hold=100):
                qmp.hmp(f'sendkey {k} {hold}')

            def press(k, want, seconds=6):
                key(k)
                wait(lambda: title() == want, seconds, f'after {k}: wanted {want!r}')
                time.sleep(0.4)

            def open_entry(parent='Menu'):
                """Center on the selected entry; the new screen's title."""
                key('ret')
                end = time.monotonic() + 4
                while time.monotonic() < end:
                    t = title()
                    if t != parent:
                        time.sleep(0.4)
                        return t
                    time.sleep(0.1)
                return parent

            wait(lambda: status()['magic'] == MAGIC, 10, 'OS never started')
            wait(lambda: status()['selftest_done'] == ST_ALL, 60, 'self-test incomplete')
            wait(lambda: status()['ui_depth'] == 1, 10, 'no UI')
            def search(want, parent, titles):
                """In the list screen parent: open each entry until one is
                titled want; look inside Tools too."""
                for i in range(status()['ui_list_count']):
                    if i:
                        press('down', parent)
                    t = open_entry(parent)
                    titles.append(t)
                    if t == want:
                        return True
                    if t == 'Tools' and parent == 'Menu':
                        if search(want, 'Tools', titles):
                            return True
                    if t != parent:
                        press('f2', parent)
                return False

            def find(want):
                """From home: the menu entry (or Tools entry) titled want."""
                press('ret', 'Menu')            # the menu starts at the top
                titles = []
                if search(want, 'Menu', titles):
                    return titles
                raise AssertionError(f'no {want} entry in the menu or Tools: {titles}')

            result['menu'] = find('SIM')

            wait(lambda: report()['scans'] >= 1 and not report()['busy'], 20, 'slots never read')
            time.sleep(0.5)
            first = report()
            shot('sim')
            key('ret')                          # Center: read again
            wait(lambda: report()['scans'] >= 2 and not report()['busy'], 20, 'no second read')
            again = report()
            result['first'] = {str(k): v for k, v in first.items()}
            result['again'] = {str(k): v for k, v in again.items()}
            tck = 0
            for b in MODEL_ATR[1:]:
                tck ^= b
            tck_ok = tck == 0
            for r in (first, again):
                for s in range(2):
                    got = r[s]
                    if slots >> s & 1:
                        want = {'state': 2, 'result': 0, 'mv': 1800, 'proto': 0,
                                'protocols': 1, 'atr_len': len(MODEL_ATR), 'sw': 0x9f0f,
                                'flags': 0 if tck_ok else F_TCK_BAD}
                    else:
                        want = {'state': 2, 'result': SIM_ENOCARD, 'mv': 0, 'atr_len': 0,
                                'sw': 0}
                    for f, v in want.items():
                        assert got[f] == v, f'slot {s} {f}: {got[f]:#x}, wanted {v:#x} ({got})'

            press('esc', 'B310E OS')            # END: home, then Power off -> Yes
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
    inss = sorted(set(re.findall(r'SIM APDU cla=0x\w+ ins=(0x\w+)', tr)))
    resets = re.findall(r'SIM cold reset slot=(\d) card=(\d)', tr)
    result['apdu_ins'] = inss
    result['resets'] = len(resets)
    assert inss == ['0xa4'], f'commands other than SELECT reached the card: {inss}'
    assert {s for s, c in resets if c == '1'} == {str(s) for s in range(2) if slots >> s & 1}, resets

    image = card.read_bytes()
    log_at = image.find(b'lcd: transfer spacing')       # the log's first line
    assert log_at >= 0, 'no card log'
    log_at = log_at // 512 * 512
    log_text = image[log_at:image.find(b'\0', log_at)].decode(errors='replace')
    (out / 'card-log.txt').write_text(log_text)
    lines = [l[l.index('sim: '):] for l in log_text.splitlines() if 'sim: ' in l]
    result['log'] = lines
    for s in range(2):
        want = f'sim: slot {s} card at 1800 mV' if slots >> s & 1 else f'sim: slot {s} no card'
        assert any(l.startswith(want) for l in lines), (want, lines)
    flat = log_text.lower().replace(' ', '')
    for secret in (ICCID, IMSI, ATR_HISTORICAL, ATR_HISTORICAL[:8]):
        assert secret not in flat, f'{secret} in the card log'
    print('PASS ' + json.dumps(result), flush=True)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qemu', type=Path, default=REPO / 'build/qemu/build/qemu-system-arm.exe')
    parser.add_argument('--elf', type=Path, default=REPO / 'build/os/b310e-os.elf')
    parser.add_argument('--image', type=Path, default=REPO / 'build/os/b310e-os.bin')
    parser.add_argument('--output', type=Path, default=REPO / 'build/os/test-sim')
    args = parser.parse_args()
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    results = [run(args, 'slot0', 1), run(args, 'slot1', 2)]
    (args.output / 'results.json').write_text(json.dumps(results, indent=2))


if __name__ == '__main__':
    main()
