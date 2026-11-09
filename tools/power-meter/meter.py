#!/usr/bin/env python3
"""The PC side of the B310E power meter (power-meter.ino, an Arduino Leonardo).

  meter.py --scan                 print what every ADC code reads across the resistor (the
                                  phone drawing current: screen on). A pair's codes stand in
                                  the ratio of their gains; choose from those, then:
  meter.py --range M:G --range M:G   store them in the Leonardo, most sensitive first
  meter.py --seconds 30           mA over that long
  meter.py --bench 60 [--dark]    the phone's USB off for 60 s (its shell's "bench"), the
                                  current measured meanwhile
  meter.py --ab 70                dark, chip sleep on, off, on: 70 s each

The firmware reports the steady part of the current, without the sharp pulses on the line
(power-meter.ino, loop). The resistor read 0.26 ohm on a multimeter (--ohms); check the
scale against a multimeter in series before trusting absolute mA.

--port COMx if it is not found. --calibrate MA: the meter reads MA now (your multimeter's
figure), so print the ohms that make that true. Needs pyserial.

SPDX-License-Identifier: GPL-2.0-or-later
"""
import argparse
import csv
from pathlib import Path
import statistics
import subprocess
import sys
import time

import serial
from serial.tools import list_ports

SHELL = Path(__file__).resolve().parents[2] / 'os/tools/b310e-shell.py'
VREF_MV = 2560.0
USE_MUX, USE_GAIN = 9, 10            # see power-meter.ino


def find_port():
    for p in list_ports.comports():
        if p.device and (p.vid in (0x2341, 0x2a03) or (p.description and 'Leonardo' in p.description)):
            return p.device
    raise SystemExit('no Leonardo found: use --port')


def open_meter(port):
    s = serial.Serial(port or find_port(), 115200, timeout=1)
    time.sleep(1.5)
    s.write(b'stop\n')
    time.sleep(0.2)
    s.reset_input_buffer()
    return s


def read_lines(s, seconds):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        line = s.readline().decode(errors='replace').strip()
        if line:
            yield line


def scan(s):
    """A pair reads the same voltage at every gain, so its codes' counts stand in the ratio
    of the gains (1, 10, 40, 200); at full scale a code says nothing. This prints; choosing
    needs a look, because floating inputs read something too."""
    s.write(b'scan\n')
    rows = {}
    for line in read_lines(s, 30):
        p = line.split(',')
        if p[0] == 'scan' and p[1] == 'done':
            break
        if p[0] == 'scan':
            rows[int(p[1])] = (int(p[2]), int(p[3]), int(p[4]))
    for m, (mean, lo, hi) in sorted(rows.items()):
        print(f'{m:2d} {m:06b}  mean {mean:6d}  {lo:6d} .. {hi:6d}')
    return rows


def store(s, ranges):
    for i, (mux, gain) in enumerate(ranges):
        s.write(f'range {i} {mux} {gain}\n'.encode())
        time.sleep(0.1)
    s.reset_input_buffer()
    print('stored:', ranges)


def measure(s, ohms, seconds, csv_path=None):
    s.write(f'range 0 {USE_MUX} {USE_GAIN}\n'.encode())
    time.sleep(0.1)
    s.reset_input_buffer()
    s.write(b'ranges\n')
    gains = {}
    for line in read_lines(s, 3):
        p = line.split(',')
        if p[0] == 'range':
            gains[int(p[1])] = int(p[3])
        if p[0] == 'ranges':
            break
    if not gains:
        raise SystemExit('the Leonardo has no ranges: --scan, then --range')
    s.write(b'go\n')
    out = []
    writer = None
    f = open(csv_path, 'a', newline='') if csv_path else None
    if f:
        writer = csv.writer(f)
    for line in read_lines(s, seconds):
        p = line.split(',')
        if len(p) != 6 or not p[0].isdigit():
            continue
        r, mean, lo, hi = int(p[1]), float(p[2]), int(p[3]), int(p[4])
        k = VREF_MV / (512 * gains[r] * ohms)
        ma, ma_lo, ma_hi = abs(mean) * k, min(abs(lo), abs(hi)) * k, max(abs(lo), abs(hi)) * k
        out.append((time.monotonic(), ma, ma_lo, ma_hi, r))
        if writer:
            writer.writerow((round(time.time(), 2), round(ma, 3), round(ma_lo, 2), round(ma_hi, 2), r))
    s.write(b'stop\n')
    time.sleep(0.2)
    s.reset_input_buffer()
    if f:
        f.close()
    return out


def report(name, rows, skip=0.0):
    if not rows:
        print(name, 'no data')
        return None
    t0 = rows[0][0]
    ma = [r[1] for r in rows if r[0] - t0 >= skip]
    if not ma:
        return None
    print(f'{name}: mean {statistics.fmean(ma):.2f} mA, median {statistics.median(ma):.2f}, '
          f'min {min(ma):.2f}, max {max(ma):.2f}, {len(ma)} readings')
    return statistics.fmean(ma)


def shell(*words, timeout=20):
    return subprocess.run([sys.executable, str(SHELL), *words], capture_output=True, text=True,
                          timeout=timeout).stdout


def wait_shell(seconds=60):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        try:
            if 'built' in shell('cmd', 'info', timeout=10):
                return True
        except subprocess.TimeoutExpired:
            pass
        time.sleep(1)
    return False


def bench(s, ohms, seconds, dark, name, csv_path, nosleep=False):
    """Settle first: after USB stops, the clock takes some seconds to drop."""
    try:
        reply = shell('cmd', f'bench {seconds}' + (' dark' if dark else '') +
                      (' nosleep' if nosleep else ''), timeout=10)
    except subprocess.TimeoutExpired:
        reply = 'no answer'
    if not reply.startswith('OK'):      # or this would report USB-on current as a bench's
        raise SystemExit(f'bench did not start: {reply.strip()}')
    rows = measure(s, ohms, seconds - 1, csv_path)
    mean = report(name, rows, skip=min(25, seconds / 2))    # the clock takes up to 18 s to drop
    if not wait_shell():
        print('the phone did not come back')
    return mean


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--port')
    ap.add_argument('--scan', action='store_true')
    ap.add_argument('--range', action='append', default=[], metavar='MUX:GAIN',
                    help='store these, most sensitive first (what the scan points to)')
    ap.add_argument('--ohms', type=float, default=0.26)
    ap.add_argument('--seconds', type=float)
    ap.add_argument('--bench', type=int)
    ap.add_argument('--ab', type=int, help='Chip sleep on, off, on: this many seconds each')
    ap.add_argument('--dark', action='store_true')
    ap.add_argument('--csv')
    ap.add_argument('--calibrate', type=float, metavar='MA')
    args = ap.parse_args()
    s = open_meter(args.port)
    if args.scan:
        scan(s)
    if args.range:
        store(s, [tuple(int(x) for x in r.split(':')) for r in args.range])
    if args.calibrate:
        rows = measure(s, 1.0, 5)
        v = statistics.fmean(r[1] for r in rows)
        print(f'{v:.3f} mV across it: --ohms {v / args.calibrate:.4f}')
    if args.seconds:
        report('now', measure(s, args.ohms, args.seconds, args.csv))
    if args.bench:
        bench(s, args.ohms, args.bench, args.dark, 'bench', args.csv)
    if args.ab:
        on = bench(s, args.ohms, args.ab, True, 'chip sleep on ', args.csv)
        off = bench(s, args.ohms, args.ab, True, 'chip sleep off', args.csv, nosleep=True)
        again = bench(s, args.ohms, args.ab, True, 'chip sleep on ', args.csv)
        if on and off and again:
            print(f'chip sleep saves {off - (on + again) / 2:.2f} mA')


if __name__ == '__main__':
    main()
