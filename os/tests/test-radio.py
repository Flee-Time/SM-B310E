#!/usr/bin/env python3
"""Run Tools / Radio status (apps/gsmstatus.c) in QEMU.

QEMU runs no DSP program: its sc6530_dsp model answers the boot ROM's
download handshake and nothing after it. So this test checks the screen's
plumbing and its honesty, not the radio. For each case it boots the OS,
opens Tools / Radio status, waits for the run to finish and reads the
screen's `radio_status` and the driver's `dsp_info` (ELF symbols):
  - no NOR image, a ready card in slot 1: SIM 1 "ready", SIM 2 "no card",
    dsp_boot() says "no DSP image", no scan, the report is on the card;
  - with --nor (the owner's own dump, never in the repository) and a
    PIN-locked card: SIM 1 "PIN", the DSP boots, layer 1 gets no answer
    (the model takes no parameter records), so there is no frame clock and
    no scan; the DSP is held in reset again afterwards.
Center runs it again and Back leaves; power-off still works.
Whether the phone's DSP answers is for the phone: radio-report.txt on its card.

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
DSPT = runpy.run_path(str(REPO / 'os/tests/test-dsp.py'))
QT, capture = DSPT['QT'], DSPT['capture']
symbol, signed, free_port, string = DSPT['symbol'], DSPT['signed'], DSPT['free_port'], DSPT['string']
FIELDS, DSP_FIELDS, MAGIC, ST_ALL = DSPT['FIELDS'], DSPT['DSP_FIELDS'], DSPT['MAGIC'], DSPT['ST_ALL']
NOR_SIZE = DSPT['NOR_SIZE']

RADIO_FIELDS = ('runs busy phase sim0 sim1 sim_sw0 sim_sw1 sim_mv0 sim_mv1 dsp_result l1_result '
                'records frames_taken frame_us sweeps pm_frames pm_results pm_valid pm_sat_hi '
                'pm_sat_lo pm_other pm_stray channels_heard stalled').split()
SIGNED = {'sim_sw0', 'sim_sw1', 'dsp_result', 'l1_result'}
R_DONE = 9
SIM_NONE, SIM_READY, SIM_PIN = 0, 1, 2
DSP_OFF, DSP_FAILED = 0, 3
DSP_EIMAGE, DSP_ECMD = -1, -7
IMSI = '234150123456789'               # must never reach the card


def run(args, name, nor, pin):
    out = args.output / name
    out.mkdir(parents=True, exist_ok=True)
    base = symbol(args.elf, 'os_status')
    info = symbol(args.elf, 'dsp_info')
    radio = symbol(args.elf, 'radio_status')
    dspboot = symbol(args.elf, 'dspboot_status')
    card = out / 'card.img'
    subprocess.run([sys.executable, str(REPO / 'ports/rockbox/tests/make-sd-image.py'),
                    '--fat32', '--size-mib', '64', '--seconds', '1', '--force',
                    '--runtime', str(REPO / 'sdcard/.rockbox'),
                    '--rockbox', str(REPO / 'sdcard/payloads/rockbox.bin'),
                    '--output', str(card)], check=True, capture_output=True)
    qmp_port, qtest_port = free_port(), free_port()
    command = [str(args.qemu.resolve()), '-M', 'b310e,boot-mode=rockbox',
               '-display', os.environ.get('B310E_QEMU_DISPLAY', 'none'), '-serial', 'none',
               '-monitor', 'none',
               '-drive', f'file={args.image.resolve().as_posix()},format=raw,if=none,id=os,readonly=on',
               '-drive', f'file={card.as_posix()},format=raw,if=none,id=sdcard',
               '-qmp', f'tcp:127.0.0.1:{qmp_port},server=on,wait=off',
               '-qtest', f'tcp:127.0.0.1:{qtest_port},server=on,wait=off',
               '-global', 'sc6530_adi.watchdog=on',
               '-global', 'sc6530_sim.sim-slots=1',
               '-global', f'sc6530_sim.sim-imsi={IMSI}',
               '-global', f'sc6530_sim.sim-pin={"on" if pin else "off"}']
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

            def state():
                r = {f: qt.read(radio + 4 * i) for i, f in enumerate(RADIO_FIELDS)}
                for f in SIGNED:
                    r[f] = signed(r[f])
                r['dsp_state'] = qt.read(info + 4 * DSP_FIELDS.index('state'))
                return r

            def wait(test, seconds, what):
                end = time.monotonic() + seconds
                while time.monotonic() < end:
                    if test():
                        return
                    time.sleep(0.1)
                raise AssertionError(f'{what}: {title()!r} {json.dumps(state())}')

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
                    if t == 'Tools' and parent == 'Menu' and search(want, 'Tools', titles):
                        return True
                    if t != parent:
                        # DSP boot keeps the DSP until its thread ends
                        wait(lambda: not state()['busy'] and not qt.read(dspboot + 4), 60,
                             f'{t} still busy')
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
            result['menu'] = find('Radio status')

            for attempt in (1, 2):
                if attempt == 2:
                    key('ret')                  # Center: run again
                wait(lambda: state()['runs'] >= attempt and not state()['busy'], 40,
                     f'run {attempt} never finished')
                r = state()
                result[f'run{attempt}'] = r
                qmp.command('screendump', {'filename': str(out / f'radio-{attempt}.png'),
                                           'format': 'png'})
                assert r['phase'] == R_DONE, r
                assert r['sim0'] == (SIM_PIN if pin else SIM_READY), r
                assert r['sim_sw0'] == (0x9804 if pin else 0x9000), r
                assert r['sim1'] == SIM_NONE, r
                # nothing radio-related may be claimed without a DSP program
                assert r['frames_taken'] == 0 and r['pm_results'] == 0, r
                assert r['channels_heard'] == 0 and r['sweeps'] == 0, r
                if nor:
                    assert r['dsp_result'] == 0, r
                    assert r['l1_result'] == DSP_ECMD and r['records'] == 0, r
                    assert r['dsp_state'] == DSP_OFF, r        # held in reset after
                else:
                    assert r['dsp_result'] == DSP_EIMAGE and r['l1_result'] == 1, r
                    assert r['dsp_state'] == DSP_FAILED, r
            assert title() == 'Radio status'

            press('f2', 'Tools')
            # back from a screen it opened, a list keeps its place...
            key('ret')
            wait(lambda: title() == 'Radio status' and state()['runs'] >= 3 and
                 not state()['busy'], 40, 'the list lost its place')
            press('f2', 'Tools')
            # ...and opened anew it starts at the top: Center is the Torch's, on
            # and off again, not Radio status
            press('f2', 'Menu')
            press('ret', 'Tools')
            for _ in range(2):
                key('ret')
                time.sleep(0.6)
                assert title() == 'Tools', f'the list kept its old place: {title()!r}'
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

    image = card.read_bytes()
    assert b'# B310E OS radio status (receive only)' in image, 'no report on the card'
    assert (b'sim1 pin' if pin else b'sim1 ready') in image, 'SIM state not in the report'
    assert b'sim2 none' in image
    assert IMSI.encode() not in image, 'card data leaked into the report'
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
    parser.add_argument('--output', type=Path, default=REPO / 'build/os/test-radio')
    args = parser.parse_args()
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    results = [run(args, 'no-image', None, pin=False)]
    if args.nor:
        results.append(run(args, 'nor', args.nor.read_bytes(), pin=True))
    (args.output / 'results.json').write_text(json.dumps(results, indent=2))


if __name__ == '__main__':
    main()
