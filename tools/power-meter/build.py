#!/usr/bin/env python3
"""Build power-meter.ino for the Arduino Leonardo with the compiler the Arduino IDE
installed (no arduino-cli needed), and flash it with --flash [--port COMx].

SPDX-License-Identifier: GPL-2.0-or-later
"""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

HERE = Path(__file__).resolve().parent
ARDUINO_VIDS = (0x2341, 0x2a03)         # arduino.cc, arduino.org
PK = Path(os.environ['LOCALAPPDATA']) / 'Arduino15/packages/arduino'
AVR = PK / 'hardware/avr/1.8.8'
GCC = next((PK / 'tools/avr-gcc').glob('*/bin'))
DUDE = next((PK / 'tools/avrdude').glob('*'))
DEFS = ['-DF_CPU=16000000L', '-DARDUINO=10819', '-DARDUINO_AVR_LEONARDO', '-DARDUINO_ARCH_AVR',
        '-DUSB_VID=0x2341', '-DUSB_PID=0x8036', '-DUSB_MANUFACTURER="Unknown"',
        '-DUSB_PRODUCT="Arduino Leonardo"']
INC = [f'-I{AVR / "cores/arduino"}', f'-I{AVR / "variants/leonardo"}', f'-I{AVR / "libraries/EEPROM/src"}']
COMMON = ['-mmcu=atmega32u4', '-Os', '-w', '-ffunction-sections', '-fdata-sections', '-MMD', *DEFS, *INC]


def run(*cmd):
    subprocess.run([str(c) for c in cmd], check=True)


def build(out):
    objs = []
    sources = [p for p in (AVR / 'cores/arduino').iterdir() if p.suffix in ('.c', '.cpp', '.S')]
    wrapper = out / 'sketch.cpp'
    wrapper.write_text('#include <Arduino.h>\n#line 1 "power-meter.ino"\n' +
                       (HERE / 'power-meter.ino').read_text())
    for src in [wrapper, *sources]:
        obj = out / (src.name + '.o')
        objs.append(obj)
        if src.suffix == '.c':
            run(GCC / 'avr-gcc.exe', '-c', '-std=gnu11', *COMMON, src, '-o', obj)
        elif src.suffix == '.S':
            run(GCC / 'avr-gcc.exe', '-c', '-x', 'assembler-with-cpp', *COMMON, src, '-o', obj)
        else:
            run(GCC / 'avr-g++.exe', '-c', '-std=gnu++11', '-fno-exceptions', '-fno-threadsafe-statics',
                '-fpermissive', *COMMON, src, '-o', obj)
    elf, hex_ = out / 'power-meter.elf', out / 'power-meter.hex'
    run(GCC / 'avr-gcc.exe', '-mmcu=atmega32u4', '-Os', '-Wl,--gc-sections', *objs, '-lm', '-o', elf)
    run(GCC / 'avr-objcopy.exe', '-O', 'ihex', '-R', '.eeprom', elf, hex_)
    subprocess.run([str(GCC / 'avr-size.exe'), str(elf)])
    return hex_


def flash(hex_, port):
    import serial
    from serial.tools import list_ports

    if not port:
        for p in list_ports.comports():
            if p.vid in ARDUINO_VIDS and p.device:
                port = p.device
    if not port:
        sys.exit('no Leonardo found: use --port')
    before = {p.device for p in list_ports.comports()}
    serial.Serial(port, 1200).close()   # the Leonardo's way into its boot loader
    end = time.monotonic() + 8
    boot = None
    while time.monotonic() < end and not boot:
        time.sleep(0.2)
        # a port that was not there: the old one can still be listed for a moment
        new = {p.device for p in list_ports.comports()} - before
        boot = next(iter(new), None)
    run(DUDE / 'bin/avrdude.exe', '-C', DUDE / 'etc/avrdude.conf', '-p', 'atmega32u4', '-c', 'avr109',
        '-P', boot or port, '-b', '57600', '-D', f'-Uflash:w:{hex_}:i')


def wait_flash(hex_, seconds):
    """For a Leonardo put in its boot loader by hand (reset twice: about 8 s). Flash as soon
    as the boot loader's port shows up."""
    from serial.tools import list_ports

    before = {p.device for p in list_ports.comports()}
    print('waiting for the boot loader; ports now:', sorted(before), flush=True)
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        for p in list_ports.comports():
            if not p.device:            # just appeared: not named yet
                continue
            if p.device not in before or (p.vid in ARDUINO_VIDS and p.pid == 0x0036):
                print('boot loader on', p.device, hex(p.vid or 0), hex(p.pid or 0), flush=True)
                time.sleep(0.3)
                run(DUDE / 'bin/avrdude.exe', '-C', DUDE / 'etc/avrdude.conf', '-p', 'atmega32u4',
                    '-c', 'avr109', '-P', p.device, '-b', '57600', '-D', f'-Uflash:w:{hex_}:i')
                print('flashed', flush=True)
                return
        before &= {p.device for p in list_ports.comports()}
        time.sleep(0.05)
    sys.exit('no boot loader appeared')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--flash', action='store_true')
    ap.add_argument('--wait-boot', type=int, metavar='SECONDS',
                    help='wait for a Leonardo put in its boot loader by hand, then flash')
    ap.add_argument('--port')
    ap.add_argument('--out', type=Path, default=Path(tempfile.gettempdir()) / 'b310e-power-meter')
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    hex_ = build(args.out)
    print('built', hex_)
    if args.wait_boot:
        wait_flash(hex_, args.wait_boot)
    elif args.flash:
        flash(hex_, args.port)


if __name__ == '__main__':
    main()
