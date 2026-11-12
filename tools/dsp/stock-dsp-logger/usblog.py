#!/usr/bin/env python3
"""Read the patched stock firmware's two USB ports.

  usblog.py [--out FILE] [--diag FILE] [--dsp-port COMn] [--diag-port COMn] [--no-stamp]
  usblog.py --decode RAWFILE [--all]

The phone (stock firmware patched by patch.py) is 1782:3d00 with two vendor
interfaces. With the Spreadtrum serial driver, Windows shows them as COM
ports, named after what the unpatched firmware puts there:

  "SPRD AT"    interface 0   the logger's port: the DSP calls as text lines
  "SPRD DIAG"  interface 1   the stock's own log, in its 0x7e framing

Opening a port makes the driver send the request the firmware waits for
before it transmits anything (class request 0x22). This reads the logger's
port, prints its lines and, with --out, saves them. With --diag FILE it
also opens the DIAG port and saves the stock's trace text to FILE and the
undecoded bytes to FILE.raw. Each saved line starts with the PC's time in
seconds since the tool started, so the two files can be lined up; --no-stamp
leaves it out.

The logger's lines, as this tool prints and saves them:

  ARM -> DSP
  ~C tick w0 w1 w2 w3 w4            command ring (w0 = command id)
  ~F tick w0..w10 e0 e1 e2          frame instruction + its 6-byte extension
  ~T tick tag type len bytes        TX block handed to the DSP
  DSP -> ARM (what the ARM reads back)
  ~R tick h0..h8 bytes              RX ring result: 9 header words, 54 data bytes
  ~A tick w0 w1 w2 w3 w4            RESP ring response
  ~P tick w0 w1 w2 w3               power-measure result
  ~B tick w0 w1                     bit-error report
  ~M tick code param ack            message in the control block
  other
  ~S tick old new                   layer-1 state change
  ~H tick service v6                a USB service started
  ~D tick count                     records lost (the logger's ring was full)

tick is the stock millisecond tick at the call, not at the send. On the
wire two kinds are shortened, and this tool undoes it: a ~F line ends in
'*' where the rest of its 14 words are ffff, and a ~R result comes as three
lines "~R tick offset total bytes".

--decode reads a capture of the DIAG port (a .raw file) and prints the '~'
lines in it, or with --all every text run: for captures made with the
earlier logger versions, which sent their lines on the DIAG port.

SPDX-License-Identifier: GPL-2.0-or-later
"""
import argparse
from pathlib import Path
import re
import sys
import threading
import time

VID, PID = 0x1782, 0x3d00
LINE = re.compile(rb'~[A-Z] [0-9a-f]{8}[ -~]*')
TEXT = re.compile(rb'[ -~]{4,}')
COMMANDS = {0: 'record/NV block', 1: 'start frame clock', 2: 'activate', 3: 'dedicated/track',
            4: 'PA key', 0x1b: 'sleep/wake', 0x2f: 'sleep/wake'}


class Deframer:
    """Splits the DIAG byte stream into unescaped frames."""

    def __init__(self):
        self.frame = bytearray()
        self.escape = False

    def feed(self, data):
        for byte in data:
            if byte == 0x7e:
                if self.frame:
                    yield bytes(self.frame)
                self.frame = bytearray()
                self.escape = False
            elif byte == 0x7d:
                self.escape = True
            else:
                self.frame.append(byte ^ 0x20 if self.escape else byte)
                self.escape = False


def lines(frame, everything=False):
    """Text lines in one DIAG frame: the logger's, or any printable run."""
    found = (TEXT if everything else LINE).findall(frame)
    return [t.decode('ascii') for t in found]


class Wire:
    """Turns the logger's wire lines into the full ones."""

    def __init__(self):
        self.rx = None                     # (tick, total, bytearray)

    def feed(self, text):
        """The finished lines this wire line gives: none, one or two."""
        parts = text.split()
        if len(parts) >= 3 and parts[0] == '~F' and text.endswith('*'):
            words = text[:-1].split()
            return [' '.join(words + ['ffff'] * (16 - len(words)))]
        if len(parts) == 5 and parts[0] == '~R':
            try:
                offset, total, data = int(parts[2], 16), int(parts[3], 16), bytes.fromhex(parts[4])
            except ValueError:
                return [text]
            out = []
            if offset == 0:
                if self.rx:
                    out.append(self.partial())
                self.rx = (parts[1], total, bytearray())
            if not self.rx or self.rx[0] != parts[1] or len(self.rx[2]) != offset:
                self.rx = None
                return out + [text]        # a piece without its start: as it came
            self.rx[2].extend(data)
            if len(self.rx[2]) >= total:
                tick, _, block = self.rx
                self.rx = None
                header = ' '.join(f'{block[i] | block[i + 1] << 8:04x}' for i in range(0, 18, 2))
                out.append(f'~R {tick} {header} {bytes(block[18:]).hex()}')
            return out
        return [text]

    def partial(self):
        tick, total, block = self.rx
        return f'~R {tick} [{len(block)} of {total} bytes] {bytes(block).hex()}'


def annotate(line):
    parts = line.split()
    if len(parts) >= 3 and parts[0] == '~C':
        try:
            name = COMMANDS.get(int(parts[2], 16))
        except ValueError:
            name = None
        if name:
            return f'{line}    ; {name}'
    return line


def find_ports():
    """(logger port, DIAG port) device names, None where absent."""
    from serial.tools import list_ports
    dsp = diag = None
    for port in list_ports.comports():
        if port.vid != VID or port.pid != PID:
            continue
        text = f'{port.description or ""} {port.hwid or ""}'.upper()
        if 'DIAG' in text or 'X.1' in text:
            diag = port.device
        else:
            dsp = port.device
    return dsp, diag


class Reader(threading.Thread):
    """Reads one COM port until told to stop, reopening it when it comes back."""

    def __init__(self, name, port, handle, stop):
        super().__init__(daemon=True)
        self.name, self.port, self.handle, self.stop = name, port, handle, stop

    def run(self):
        import serial
        while not self.stop.is_set():
            device = self.port()
            if not device:
                time.sleep(0.5)
                continue
            try:
                link = serial.Serial(device, 115200, timeout=0.2)
            except serial.SerialException:
                time.sleep(0.5)
                continue
            print(f'# {self.name}: connected on {device}', file=sys.stderr)
            total = 0
            try:
                while not self.stop.is_set():
                    data = link.read(4096)
                    if data:
                        total += len(data)
                        self.handle(data)
            except serial.SerialException:
                print(f'# {self.name}: {device} gone after {total} bytes', file=sys.stderr)
            finally:
                try:
                    link.close()
                except Exception:
                    pass


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--out', type=Path, help="save the logger's lines to this file")
    ap.add_argument('--diag', type=Path, help="also read the DIAG port: the stock's text here, the bytes in FILE.raw")
    ap.add_argument('--dsp-port', help="the logger's COM port (default: found by its USB id)")
    ap.add_argument('--diag-port', help='the DIAG COM port (default: found by its USB id)')
    ap.add_argument('--no-stamp', action='store_true', help='no PC time at the start of saved lines')
    ap.add_argument('--decode', type=Path, help='decode a DIAG capture instead of reading the phone')
    ap.add_argument('--all', action='store_true', help='with --decode: every text run, not only the ~ lines')
    args = ap.parse_args()

    if args.decode:
        for frame in Deframer().feed(args.decode.read_bytes()):
            for text in lines(frame, args.all):
                print(annotate(text))
        return

    started = time.monotonic()
    lock = threading.Lock()

    def stamp():
        return '' if args.no_stamp else f'{time.monotonic() - started:9.3f} '

    out = args.out.open('a', encoding='ascii', newline='\n') if args.out else None
    pending = bytearray()
    wire = Wire()

    def dsp_data(data):
        pending.extend(data)
        while True:
            end = pending.find(b'\n')
            if end < 0:
                break
            text = bytes(pending[:end]).decode('ascii', 'replace').strip()
            del pending[:end + 1]
            for full in wire.feed(text) if text else ():
                full = annotate(full)
                with lock:
                    print(full, flush=True)
                    if out:
                        out.write(stamp() + full + '\n')
                        out.flush()

    stop = threading.Event()
    readers = [Reader('logger', lambda: args.dsp_port or find_ports()[0], dsp_data, stop)]
    if args.diag:
        diag_text = args.diag.open('a', encoding='ascii', newline='\n')
        diag_raw = args.diag.with_name(args.diag.name + '.raw').open('ab')
        deframer = Deframer()

        def diag_data(data):
            diag_raw.write(data)
            diag_raw.flush()
            for frame in deframer.feed(data):
                for text in lines(frame, True):
                    diag_text.write(stamp() + text + '\n')
            diag_text.flush()

        readers.append(Reader('diag', lambda: args.diag_port or find_ports()[1], diag_data, stop))

    print(f'# waiting for {VID:04x}:{PID:04x} (stop with Ctrl+C)', file=sys.stderr)
    for reader in readers:
        reader.start()
    try:
        while True:
            time.sleep(0.5)
    except KeyboardInterrupt:
        stop.set()
        for reader in readers:
            reader.join(2)


if __name__ == '__main__':
    main()
