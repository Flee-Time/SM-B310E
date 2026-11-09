#!/usr/bin/env python3
"""The PC side of the B310E OS development shell (USB network, os/net/).

On the phone: Tools -> USB -> Network, then plug the cable in. The PC gets
192.168.7.2 by DHCP; the phone is 192.168.7.1. This script talks to the
shell's raw port (2323): plain lines, and the binary transfers that the
telnet port (23) does not carry.

  b310e-shell.py                         interactive shell (Ctrl-C or "exit" ends)
  b310e-shell.py cmd ps                  one command, its output
  b310e-shell.py put app.bin /x/app.bin  upload (CRC32 checked)
  b310e-shell.py get /b310e-os/log.txt   download (to log.txt; CRC32 checked)
  b310e-shell.py run build/os/modules/stopwatch.app [--arg ARG] [--follow]
                                         upload to /b310e-os/apps/ and start it
                                         (with ARG, its app_arg()),
                                         closing the app that is open; --follow
                                         then shows the log until Ctrl-C
  b310e-shell.py shot [screen.png]       the phone's screen as a PNG
  b310e-shell.py log                     follow the log until Ctrl-C
  b310e-shell.py time                    set the phone's clock from this PC's

Live coding: edit os/modules/<app>/, `make -C os`, then
`b310e-shell.py run build/os/modules/<app>.app`.

Only the Python standard library. SPDX-License-Identifier: GPL-2.0-or-later
"""
import argparse
import os
from pathlib import Path
import socket
import struct
import sys
import threading
import zlib

PROMPT = b'b310e> '


class ShellError(Exception):
    pass


class Shell:
    """sock: an already open connection (os/tests/test-net.py passes its
    own); else one to host:port."""

    def __init__(self, host, port, timeout=30, sock=None):
        self.sock = sock or socket.create_connection((host, port), timeout=timeout)
        self.buf = b''
        self.read_until(PROMPT)                 # the banner

    def _fill(self):
        chunk = self.sock.recv(65536)
        if not chunk:
            raise ShellError('the phone closed the connection')
        self.buf += chunk

    def read_until(self, marker):
        while marker not in self.buf:
            self._fill()
        i = self.buf.index(marker) + len(marker)
        out, self.buf = self.buf[:i], self.buf[i:]
        return out

    def read_exact(self, n):
        while len(self.buf) < n:
            self._fill()
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def send(self, data):
        # a socket's timeout is sendall's whole deadline: in pieces, so it is per piece
        for at in range(0, len(data), 32768):
            self.sock.sendall(data[at:at + 32768])

    def cmd(self, line):
        self.send(line.encode() + b'\n')
        return self.read_until(PROMPT)[:-len(PROMPT)].decode(errors='replace')

    def put(self, remote, data):
        self.send(f'put "{remote}" {len(data)}\n'.encode())
        line = self.read_until(b'\n').decode(errors='replace').strip()
        if line != 'READY':
            self.read_until(PROMPT)
            raise ShellError(line)
        self.send(data)
        line = self.read_until(PROMPT)[:-len(PROMPT)].decode(errors='replace').strip()
        want = f'OK {len(data)} {zlib.crc32(data):08x}'
        if line != want:
            raise ShellError(f'{line} (expected {want})')

    def _blob(self):
        head = self.read_until(b'\n').decode(errors='replace').strip()
        if not head.startswith('DATA '):
            self.read_until(PROMPT)
            raise ShellError(head)
        data = self.read_exact(int(head.split()[1]))
        end = self.read_until(PROMPT)[:-len(PROMPT)].decode(errors='replace').strip()
        if end != f'END {zlib.crc32(data):08x}':
            raise ShellError(f'CRC mismatch: {end}, data {zlib.crc32(data):08x}')
        return data

    def get(self, remote):
        self.send(f'get "{remote}"\n'.encode())
        return self._blob()

    def shot(self):
        self.send(b'shot\n')
        return self._blob()

    def follow(self, command='log -f'):
        """Print what a following command sends until Ctrl-C."""
        self.send(command.encode() + b'\n')
        self.sock.settimeout(0.5)
        try:
            while True:
                try:
                    chunk = self.sock.recv(4096)
                except socket.timeout:
                    continue
                if not chunk:
                    return
                sys.stdout.write(chunk.decode(errors='replace'))
                sys.stdout.flush()
        except KeyboardInterrupt:
            self.sock.settimeout(30)
            self.send(b'\n')                    # any byte stops it
            self.buf = b''
            self.read_until(PROMPT)


def png(rgb565, width=128, height=160):
    rows = []
    for y in range(height):
        row = bytearray(b'\0')
        for x in range(width):
            v = struct.unpack_from('<H', rgb565, (y * width + x) * 2)[0]
            row += bytes(((v >> 11) * 255 // 31, (v >> 5 & 63) * 255 // 63, (v & 31) * 255 // 31))
        rows.append(bytes(row))

    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(b''.join(rows))) + chunk(b'IEND', b''))


def interactive(sh):
    """Lines typed here go to the phone; what the phone sends is printed
    as it comes (log -f and top run until the next line)."""
    sys.stdout.write(PROMPT.decode())
    sys.stdout.flush()
    sh.sock.settimeout(None)
    done = threading.Event()

    def reader():
        try:
            while True:
                chunk = sh.sock.recv(4096)
                if not chunk:
                    break
                sys.stdout.write(chunk.decode(errors='replace'))
                sys.stdout.flush()
        except OSError:
            pass
        done.set()

    threading.Thread(target=reader, daemon=True).start()
    try:
        for line in sys.stdin:
            if done.is_set():
                break
            sh.send(line.rstrip('\r\n').encode() + b'\n')
            if line.strip() in ('exit', 'quit'):
                done.wait(2)
                break
    except KeyboardInterrupt:
        pass


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--host', default=os.environ.get('B310E_HOST'),
                        help='default: 192.168.7.1 (the PC took our DHCP address), '
                             'then 169.254.7.1 (the PC gave itself a link-local one)')
    parser.add_argument('--port', type=int, default=2323)
    sub = parser.add_subparsers(dest='what')
    sub.add_parser('shell')
    p = sub.add_parser('cmd')
    p.add_argument('words', nargs='+')
    p = sub.add_parser('put')
    p.add_argument('local', type=Path)
    p.add_argument('remote', nargs='?')
    p = sub.add_parser('get')
    p.add_argument('remote')
    p.add_argument('local', type=Path, nargs='?')
    p = sub.add_parser('run')
    p.add_argument('app', type=Path)
    p.add_argument('--follow', action='store_true', help='then show the log until Ctrl-C')
    p.add_argument('--arg', help="the app's argument (app_arg)")
    p = sub.add_parser('shot')
    p.add_argument('png', type=Path, nargs='?', default=Path('screen.png'))
    sub.add_parser('log')
    sub.add_parser('time', help="set the phone's clock from this PC's")
    args = parser.parse_args()

    sh, err = None, None
    for host in [args.host] if args.host else ['192.168.7.1', '169.254.7.1']:
        try:
            sh = Shell(host, args.port, timeout=5 if not args.host else 30)
            sh.sock.settimeout(30)
            break
        except (OSError, ShellError) as e:
            err = f'{host}:{args.port} ({e})'
    if not sh:
        sys.exit(f'cannot reach {err}. On the phone: Tools -> USB -> Network, cable in; the PC '
                 'needs 192.168.7.2 (DHCP) or any 169.254.x.x address.')
    try:
        if args.what in (None, 'shell'):
            interactive(sh)
        elif args.what == 'cmd':
            sys.stdout.write(sh.cmd(' '.join(args.words)))
        elif args.what == 'put':
            data = args.local.read_bytes()
            remote = args.remote or '/' + args.local.name
            sh.put(remote, data)
            print(f'{remote}: {len(data)} bytes, CRC32 {zlib.crc32(data):08x}')
        elif args.what == 'get':
            data = sh.get(args.remote)
            local = args.local or Path(args.remote.rstrip('/').split('/')[-1])
            local.write_bytes(data)
            print(f'{local}: {len(data)} bytes')
        elif args.what == 'run':
            data = args.app.read_bytes()
            remote = '/b310e-os/apps/' + args.app.name
            sh.put(remote, data)
            out = sh.cmd(f'run "{remote}"' + (f' "{args.arg}"' if args.arg else ''))
            sys.stdout.write(out)
            if not out.startswith('OK'):
                sys.exit(1)
            if args.follow:
                sh.follow()
        elif args.what == 'shot':
            args.png.write_bytes(png(sh.shot()))
            print(f'{args.png}: 128 x 160')
        elif args.what == 'log':
            sh.follow()
        elif args.what == 'time':
            import time
            sys.stdout.write(sh.cmd(f'date {int(time.time() + 0.5)}'))
    except ShellError as e:
        sys.exit(f'error: {e}')
    finally:
        try:
            sh.send(b'exit\n')
        except OSError:
            pass
        sh.sock.close()


if __name__ == '__main__':
    main()
