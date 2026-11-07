#!/usr/bin/env python3
"""The USB network (RNDIS) and the shell of the B310E OS in QEMU, with
this script as the PC.

Boots the OS with VBUS present and a host on the emulated USB port (the
sc6530_usb model's chardev; protocol in emulator/qemu/machine/hw/misc/
sc6530_usb.c), opens the "USB network" screen through the test hook in
apps/usb.c, then, with a small IPv4 stack of its own:
  - enumerates as Windows does, MS OS 1.0 descriptors included (string
    0xee, the extended compat ID "RNDIS" / "5162001"), and checks the
    interface association, classes and endpoints;
  - RNDIS: INITIALIZE, QUERY (supported list, MAC, frame size, speed,
    link, an unsupported OID), SET (packet filter, a two-packet multicast
    list), KEEPALIVE, RESET, the empty GET_ENCAPSULATED_RESPONSE; every
    answer after a RESPONSE_AVAILABLE notification on the interrupt
    endpoint;
  - DHCP (DISCOVER, OFFER, REQUEST, ACK: 192.168.7.2), ARP, ping (56 and
    1400 bytes);
  - TCP to the raw shell port: help, info, ps, mem, log, peek and poke
    (safe range, refused range, a forced read of unmapped space), files
    (mkdir, put of 96 KB with its CRC, ls, crc, get compared, cat, mv,
    rm), shot, run and close of an .app uploaded over the network, key,
    log -f while a second (telnet) session starts the app;
  - the telnet port: negotiation, echo, CR LF;
  - frames whose transfer ends on a packet boundary (no zero-length
    packet), retransmission after a dropped segment;
  - unplug: the screen closes, the stack stops, the controller leaves;
then checks the uploaded files in the card image.

The image must keep usb_test_hook_net (it references app_usb_net).

SPDX-License-Identifier: GPL-2.0-or-later
"""
import argparse
import json
import os
from pathlib import Path
import random
import runpy
import socket
import struct
import subprocess
import sys
import time
import zlib

REPO = Path(__file__).resolve().parents[2]
BOOT = runpy.run_path(str(REPO / 'os/tests/test-boot.py'))
USBHOST = runpy.run_path(str(REPO / 'emulator/qemu/scripts/test-usb.py'))
HELPER = runpy.run_path(str(REPO / 'os/tools/b310e-shell.py'))
Host, Stall, UsbError = USBHOST['Host'], USBHOST['Stall'], USBHOST['UsbError']
get_descriptor, parse_config = USBHOST['get_descriptor'], USBHOST['parse_config']
symbol, status, string, wait_for = BOOT['symbol'], BOOT['status'], BOOT['string'], BOOT['wait_for']

NET_FIELDS = ('up link rx_frames tx_frames rx_bytes tx_bytes rx_dropped tx_dropped tx_timeouts '
              'rndis_msgs rndis_inits dhcp_acks pings tcp_accepted tcp_active tcp_retransmits '
              'tcp_resets shell_sessions shell_commands files_in files_out apps_run shell_open').split()
SCREEN_TICK = 32                        # offsetof(screen_t, tick)
PHONE_IP, HOST_IP = '192.168.7.1', '192.168.7.2'
PHONE_LL, HOST_LL = '169.254.7.1', '169.254.1.2'   # no DHCP: link-local
RAW_PORT, TELNET_PORT = 2323, 23
PROMPT = b'b310e> '


def ip2b(s):
    return socket.inet_aton(s)


def csum(data):
    if len(data) % 2:
        data += b'\0'
    s = sum(struct.unpack(f'!{len(data) // 2}H', data))
    while s >> 16:
        s = (s & 0xffff) + (s >> 16)
    return ~s & 0xffff


class Rndis:
    """The PC's RNDIS host and a tiny IPv4 stack on it."""

    def __init__(self, host):
        self.host = host
        self.req_id = 0
        self.mac = None                 # the PC adapter's, from the device
        self.phone_mac = None
        self.ip = b'\0\0\0\0'
        self.ip_id = 1
        self.rx_queue = []
        self.conns = {}
        self.notifications = 0
        self.drop_next_from_phone = 0   # test: lose segments the phone sends
        self.model_races = 0            # EP0 replies the old model cut short

    # -- USB ------------------------------------------------------------------
    def enumerate(self):
        h = self.host
        h.bus_reset()
        get_descriptor(h, 1, length=64)
        h.bus_reset()
        h.control(0x00, 5, 9)                       # SET_ADDRESS 9
        dev = get_descriptor(h, 1, length=18)
        head = get_descriptor(h, 2, length=9)
        total = struct.unpack_from('<H', head, 2)[0]
        config = self.ep0_read(lambda: get_descriptor(h, 2, length=total), total)
        get_descriptor(h, 3, 0, 255)
        product = get_descriptor(h, 3, dev[15], 255, 0x0409)
        serial = get_descriptor(h, 3, dev[16], 255, 0x0409)
        msos = get_descriptor(h, 3, 0xee, 0x12)     # Windows: once per VID/PID/rev
        vendor = msos[16]
        compat_head = h.control(0xc0, vendor, 0, 4, 16)
        compat = h.control(0xc0, vendor, 0, 4, struct.unpack_from('<I', compat_head)[0])
        try:
            h.control(0xc1, vendor, 0, 5, 10)       # extended properties: none
            props = 'answered'
        except Stall:
            props = 'stalled'
        h.control(0x00, 9, config[5])               # SET_CONFIGURATION
        return {'dev': dev, 'config': config, 'interfaces': parse_config(config),
                'product': product[2:].decode('utf-16-le'),
                'serial': serial[2:].decode('utf-16-le'),
                'msos': msos, 'compat': compat, 'props': props}

    def ep0_read(self, read, want):
        """A control read of more than one packet. The sc6530_usb model
        before 2026-11-07 could hand the host a packet the driver was still
        loading (a short packet that ends the stage early); fixed in the
        model's source, this retries until build/qemu has it."""
        for _ in range(5):
            data = read()
            if len(data) >= want:
                return data
            self.model_races += 1
        raise UsbError(f'control read short: {len(data)} of {want}')

    def wait_notification(self, seconds=5):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            code, data = self.host._request('I', 3, struct.pack('<H', 64))
            if code == 'D' and data:
                assert data == struct.pack('<II', 1, 0), data.hex()
                self.notifications += 1
                return
            if code == 'T':
                raise UsbError('interrupt endpoint stalled')
            time.sleep(0.001)
        raise UsbError('no RESPONSE_AVAILABLE notification')

    def command(self, msg_type, body=b'', expect=True):
        self.req_id += 1
        msg = struct.pack('<III', msg_type, 12 + len(body), self.req_id) + body
        self.host.control(0x21, 0, 0, 0, msg)
        if not expect:
            return None
        self.wait_notification()
        resp = self.host.control(0xa1, 1, 0, 0, 4096)
        rtype, rlen = struct.unpack_from('<II', resp)
        assert rtype == msg_type | 0x80000000, (hex(rtype), resp.hex())
        if rlen > len(resp) and msg_type == 4:      # see ep0_read: ask again
            self.model_races += 1
            return None
        if msg_type != 6:                           # RESET_CMPLT has no id
            assert struct.unpack_from('<I', resp, 8)[0] == self.req_id
        return resp[:rlen]

    def query(self, oid, want_ok=True):
        for _ in range(5):
            r = self.command(4, struct.pack('<IIII', oid, 0, 20, 0))
            if r:
                break
        st, n, off = struct.unpack_from('<III', r, 12)
        if want_ok:
            assert st == 0, f'QUERY {oid:#x}: status {st:#x}'
        return st, r[8 + off:8 + off + n] if n else b''

    def set(self, oid, data):
        r = self.command(5, struct.pack('<IIII', oid, len(data), 20, 0) + data)
        return struct.unpack_from('<I', r, 12)[0]

    # -- frames ---------------------------------------------------------------
    def send_frame(self, frame):
        msg = struct.pack('<IIIIIIIIIII', 1, 44 + len(frame), 36, len(frame), 0, 0, 0, 0, 0, 0, 0) + frame
        self.host.bulk_out(2, msg)                  # no zero-length packet, ever

    def read_message(self, first):
        data = first
        end = time.monotonic() + 5
        while len(first) == 64 and len(data) % 64 == 0:
            code, packet = self.host._request('I', 1, struct.pack('<H', 64))
            if code == 'D':
                data += packet
                if len(packet) < 64:
                    break
            elif time.monotonic() > end:
                raise UsbError(f'bulk IN stopped mid-message at {len(data)}')
            else:
                time.sleep(0.0003)
        return data

    def pump(self, seconds=0.0):
        """Take every frame the phone has; dispatch; True if any came."""
        end = time.monotonic() + seconds
        got = False
        while True:
            code, packet = self.host._request('I', 1, struct.pack('<H', 64))
            if code == 'D':
                msg = self.read_message(packet)
                mtype, mlen, off, dlen = struct.unpack_from('<IIII', msg)
                assert mtype == 1 and mlen <= len(msg) and 8 + off + dlen <= mlen, msg[:16].hex()
                assert mlen % 64 != 0, 'a transfer that ends on a packet boundary'
                self.dispatch(msg[8 + off:8 + off + dlen])
                got = True
                continue
            if time.monotonic() >= end:
                return got
            time.sleep(0.0005)

    def dispatch(self, f):
        dst, src, etype = f[:6], f[6:12], struct.unpack_from('!H', f, 12)[0]
        if etype == 0x0806:
            op = struct.unpack_from('!H', f, 20)[0]
            sip, tip = f[28:32], f[38:42]
            if op == 2:
                self.phone_mac = f[22:28]
            elif op == 1 and tip == self.ip:
                self.send_eth(src, 0x0806, struct.pack('!HHBBH', 1, 0x800, 6, 4, 2) + self.mac + self.ip
                              + f[22:28] + sip)
            return
        if etype != 0x0800:
            return
        ip = f[14:]
        hl = (ip[0] & 15) * 4
        tl = struct.unpack_from('!H', ip, 2)[0]
        assert csum(ip[:hl]) == 0, 'bad IP header checksum from the phone'
        proto, payload = ip[9], ip[hl:tl]
        self.rx_queue.append((proto, ip[12:16], payload, src))
        if proto == 6:
            sport, dport = struct.unpack_from('!HH', payload)
            c = self.conns.get(dport)
            if c:
                if self.drop_next_from_phone and len(payload) > (payload[12] >> 4) * 4:
                    self.drop_next_from_phone -= 1      # a data segment lost
                    return
                c.segment(payload)

    def send_eth(self, dst, etype, payload):
        self.send_frame(dst + self.mac + struct.pack('!H', etype) + payload)

    def send_ip(self, proto, dst, payload, src=None, dst_mac=None):
        src = self.ip if src is None else src
        self.ip_id += 1
        h = struct.pack('!BBHHHBBH4s4s', 0x45, 0, 20 + len(payload), self.ip_id, 0, 64, proto, 0, src, dst)
        h = h[:10] + struct.pack('!H', csum(h)) + h[12:]
        self.send_eth(dst_mac or self.phone_mac, 0x0800, h + payload)

    def udp(self, sport, dport, dst, data, dst_mac=None, src=None):
        u = struct.pack('!HHHH', sport, dport, 8 + len(data), 0) + data
        src_ip = self.ip if src is None else src
        pseudo = src_ip + dst + struct.pack('!BBH', 0, 17, len(u))
        c = csum(pseudo + u) or 0xffff
        u = u[:6] + struct.pack('!H', c) + u[8:]
        self.send_ip(17, dst, u, src=src, dst_mac=dst_mac)

    def wait_ip(self, test, seconds=5):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            for item in self.rx_queue:
                if test(*item):
                    self.rx_queue.remove(item)
                    return item
            self.pump(0.01)
        raise AssertionError('no matching IP packet from the phone')

    # -- DHCP, ARP, ping --------------------------------------------------------
    def dhcp(self):
        xid = random.getrandbits(32)

        def packet(msg_type, extra=b''):
            d = struct.pack('!BBBBIHH4s4s4s4s16s64s128sI', 1, 1, 6, 0, xid, 0, 0x8000, b'\0' * 4,
                            b'\0' * 4, b'\0' * 4, b'\0' * 4, self.mac, b'', b'', 0x63825363)
            d += bytes([53, 1, msg_type]) + extra + bytes([55, 3, 1, 3, 6, 255])
            return d

        def reply(want_type):
            def test(proto, src, payload, mac):
                if proto != 17 or struct.unpack_from('!HH', payload) != (67, 68):
                    return False
                d = payload[8:]
                return struct.unpack_from('!I', d, 4)[0] == xid and options(d).get(53) == bytes([want_type])
            _, _, payload, mac = self.wait_ip(test)
            self.phone_mac = mac
            return payload[8:]

        def options(d):
            out, i = {}, 240
            while i < len(d) and d[i] != 255:
                if d[i] == 0:
                    i += 1
                    continue
                out[d[i]] = d[i + 2:i + 2 + d[i + 1]]
                i += 2 + d[i + 1]
            return out

        bcast = b'\xff' * 6
        self.udp(68, 67, b'\xff' * 4, packet(1), dst_mac=bcast, src=b'\0' * 4)
        offer = reply(2)
        yiaddr, server = offer[16:20], options(offer)[54]
        self.udp(68, 67, b'\xff' * 4, packet(3, bytes([50, 4]) + yiaddr + bytes([54, 4]) + server),
                 dst_mac=bcast, src=b'\0' * 4)
        ack = reply(5)
        opts = options(ack)
        self.ip = ack[16:20]
        # a request for an address the phone does not hand out: NAK
        self.udp(68, 67, b'\xff' * 4, packet(3, bytes([50, 4]) + ip2b('10.1.2.3')),
                 dst_mac=bcast, src=b'\0' * 4)
        reply(6)
        return {'offer': socket.inet_ntoa(yiaddr), 'server': socket.inet_ntoa(server),
                'ack': socket.inet_ntoa(self.ip), 'mask': socket.inet_ntoa(opts[1]),
                'lease_s': struct.unpack('!I', opts[51])[0], 'router': 3 in opts}

    def arp(self, ip):
        self.phone_mac = None
        self.send_eth(b'\xff' * 6, 0x0806, struct.pack('!HHBBH', 1, 0x800, 6, 4, 1) + self.mac + self.ip
                      + b'\0' * 6 + ip2b(ip))
        end = time.monotonic() + 5
        while self.phone_mac is None:
            assert time.monotonic() < end, 'no ARP reply'
            self.pump(0.01)
        return self.phone_mac

    def ping(self, size, seq, dst=PHONE_IP):
        data = bytes((i * 7 + seq) & 0xff for i in range(size))
        icmp = struct.pack('!BBHHH', 8, 0, 0, 0x4242, seq) + data
        icmp = icmp[:2] + struct.pack('!H', csum(icmp)) + icmp[4:]
        t0 = time.monotonic()
        self.send_ip(1, ip2b(dst), icmp)
        _, _, payload, _ = self.wait_ip(lambda p, s, pl, m: p == 1 and pl[0] == 0 and
                                        struct.unpack_from('!HH', pl, 4) == (0x4242, seq))
        assert csum(payload) == 0 and payload[8:] == data, 'echo reply differs'
        return round((time.monotonic() - t0) * 1000, 1)


class Tcp:
    """A TCP client: in-order receive, a send window, go-back-N."""

    def __init__(self, net, port):
        self.net = net
        self.port = port
        self.lport = random.randint(40000, 60000)
        self.iss = random.getrandbits(32)
        self.snd_una = self.snd_nxt = self.iss
        self.rcv_nxt = None
        self.peer_wnd = 0
        self.peer_mss = 536
        self.rx = bytearray()
        self.fin = False
        self.rst = False
        self.syn_ack = False
        self.unacked = b''                # from snd_una
        net.conns[self.lport] = self

    def send_seg(self, flags, seq, data=b'', opts=b''):
        hl = 20 + len(opts)
        ack = self.rcv_nxt or 0
        t = struct.pack('!HHIIBBHHH', self.lport, self.port, seq & 0xffffffff, ack & 0xffffffff,
                        (hl // 4) << 4, flags, 65535, 0, 0) + opts + data
        pseudo = self.net.ip + ip2b(PHONE_IP) + struct.pack('!BBH', 0, 6, len(t))
        t = t[:16] + struct.pack('!H', csum(pseudo + t)) + t[18:]
        self.net.send_ip(6, ip2b(PHONE_IP), t)

    def segment(self, t):
        sport, dport, seq, ack, off, flags, wnd = struct.unpack_from('!HHIIBBH', t)
        pseudo = ip2b(PHONE_IP) + self.net.ip + struct.pack('!BBH', 0, 6, len(t))
        assert csum(pseudo + t) == 0, 'bad TCP checksum from the phone'
        hl = (off >> 4) * 4
        data = t[hl:]
        if flags & 4:
            self.rst = True
            return
        if flags & 2:                         # SYN-ACK
            self.syn_ack = True
            self.rcv_nxt = seq + 1
            opts = t[20:hl]
            if len(opts) >= 4 and opts[0] == 2:
                self.peer_mss = struct.unpack_from('!H', opts, 2)[0]
        if flags & 0x10:
            if 0 < (ack - self.snd_una) & 0xffffffff <= (self.snd_nxt - self.snd_una) & 0xffffffff:
                n = (ack - self.snd_una) & 0xffffffff
                self.unacked = self.unacked[n:]
                self.snd_una = ack
            self.peer_wnd = wnd
        if self.rcv_nxt is None or flags & 2:
            return
        if data or flags & 1:
            if seq == self.rcv_nxt:
                self.rx += data
                self.rcv_nxt = (self.rcv_nxt + len(data)) & 0xffffffff
                if flags & 1:
                    self.fin = True
                    self.rcv_nxt = (self.rcv_nxt + 1) & 0xffffffff
            self.send_seg(0x10, self.snd_nxt)        # ACK (also a duplicate one)

    def connect(self, seconds=5):
        self.send_seg(0x02, self.iss, opts=struct.pack('!BBH', 2, 4, 1460))
        self.snd_nxt = self.snd_una = (self.iss + 1) & 0xffffffff
        end = time.monotonic() + seconds
        while not self.syn_ack:
            assert time.monotonic() < end and not self.rst, 'no SYN-ACK'
            self.net.pump(0.005)
        self.send_seg(0x10, self.snd_nxt)

    def send(self, data, seconds=60):
        """All of data, acknowledged; go back to snd_una when ACKs stop."""
        self.unacked += data
        end = time.monotonic() + seconds
        sent = (self.snd_nxt - self.snd_una) & 0xffffffff
        last_progress = time.monotonic()
        last_una = self.snd_una
        while self.unacked:
            assert time.monotonic() < end and not self.rst, 'send stalled'
            while sent < len(self.unacked) and sent < self.peer_wnd:
                n = min(self.peer_mss, len(self.unacked) - sent, self.peer_wnd - sent)
                self.send_seg(0x18, self.snd_una + sent, self.unacked[sent:sent + n])
                sent += n
                self.snd_nxt = (self.snd_una + sent) & 0xffffffff
            self.net.pump(0.002)
            sent = (self.snd_nxt - self.snd_una) & 0xffffffff
            if self.snd_una != last_una:
                last_una, last_progress = self.snd_una, time.monotonic()
            elif time.monotonic() - last_progress > 1.0:
                sent = 0                                # retransmit
                self.snd_nxt = self.snd_una
                last_progress = time.monotonic()

    def read_until(self, marker, seconds=20):
        end = time.monotonic() + seconds
        while marker not in self.rx:
            assert time.monotonic() < end and not self.rst and not self.fin, \
                f'{marker!r} never came: {bytes(self.rx[-300:])!r}'
            self.net.pump(0.005)
        i = self.rx.index(marker) + len(marker)
        out = bytes(self.rx[:i])
        del self.rx[:i]
        return out

    def read_exact(self, n, seconds=30):
        end = time.monotonic() + seconds
        while len(self.rx) < n:
            assert time.monotonic() < end and not self.rst, f'{len(self.rx)} of {n} bytes'
            self.net.pump(0.005)
        out = bytes(self.rx[:n])
        del self.rx[:n]
        return out

    def cmd(self, line, seconds=20):
        self.send(line.encode() + b'\n')
        return self.read_until(PROMPT, seconds)[:-len(PROMPT)].decode(errors='replace')

    def close(self, seconds=5):
        self.send_seg(0x11, self.snd_nxt)
        self.snd_nxt = (self.snd_nxt + 1) & 0xffffffff
        end = time.monotonic() + seconds
        while not self.fin:
            assert time.monotonic() < end, 'no FIN from the phone'
            self.net.pump(0.005)
        del self.net.conns[self.lport]


class TcpSock:
    """The socket calls os/tools/b310e-shell.py makes, on a Tcp."""

    def __init__(self, tcp):
        self.tcp = tcp

    def sendall(self, data):
        self.tcp.send(data)

    def recv(self, n):
        end = time.monotonic() + 30
        while not self.tcp.rx:
            if self.tcp.fin or self.tcp.rst:
                return b''
            assert time.monotonic() < end, 'helper: nothing to read'
            self.tcp.net.pump(0.005)
        out = bytes(self.tcp.rx[:n])
        del self.tcp.rx[:n]
        return out

    def settimeout(self, t):
        pass

    def close(self):
        pass


class Done(Exception):
    pass


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--qemu', type=Path, default=REPO / 'build/qemu/build/qemu-system-arm.exe')
    parser.add_argument('--build', type=Path, default=REPO / 'build/os')
    parser.add_argument('--output', type=Path)
    parser.add_argument('--background', action='store_true',
                        help='no screen: Settings -> USB "Network" on the card, the cable '
                             'plugged at boot; enumeration, DHCP, ping and the shell only')
    args = parser.parse_args()
    build = args.build.resolve()
    out = (args.output or build / 'test-net').resolve()
    out.mkdir(parents=True, exist_ok=True)
    elf, image, app = build / 'b310e-os.elf', build / 'b310e-os.bin', build / 'modules/stopwatch.app'
    base = symbol(elf, 'os_status')
    net_base = symbol(elf, 'net_stat')
    usb_base = symbol(elf, 'usb_stat')
    home = symbol(elf, 'home')
    hook = symbol(elf, 'usb_test_hook_net') | 1        # Thumb
    saved = symbol(elf, 'usb_test_saved_tick')
    card = out / 'card.img'
    extra = []
    if args.background:                 # the default USB function: network
        settings = out / 'settings.txt'
        settings.write_text('usb_default=2\n', newline='\n')
        extra = ['--add', f'b310e-os/settings.txt={settings}']
    subprocess.run([sys.executable, str(REPO / 'ports/rockbox/tests/make-sd-image.py'),
                    '--fat32', '--size-mib', '64', '--seconds', '1', '--force',
                    '--runtime', str(REPO / 'sdcard/.rockbox'),
                    '--rockbox', str(REPO / 'sdcard/payloads/rockbox.bin'),
                    '--add', f'b310e-os/apps/stopwatch.app={app}', *extra,
                    '--output', str(card)], check=True, capture_output=True)

    qmp_port, qtest_port, usb_port = BOOT['free_port'](), BOOT['free_port'](), BOOT['free_port']()
    command = [str(args.qemu.resolve()), '-M', 'b310e,boot-mode=rockbox',
               '-display', 'none', '-serial', 'none', '-monitor', 'none',
               '-audiodev', 'none,id=audio0', '-global', 'sc6530_adi.audiodev=audio0',
               '-drive', f'file={image.as_posix()},format=raw,if=none,id=os,readonly=on',
               '-drive', f'file={card.as_posix()},format=raw,if=none,id=sdcard',
               '-qmp', f'tcp:127.0.0.1:{qmp_port},server=on,wait=off',
               '-qtest', f'tcp:127.0.0.1:{qtest_port},server=on,wait=off',
               '-chardev', f'socket,id=usbhost,host=127.0.0.1,port={usb_port},server=on,wait=off',
               '-global', 'sc6530_usb.chardev=usbhost',
               '-global', 'sc6530_adi.charger-present=on',
               '-global', 'sc6530_adi.watchdog=on',     # the reboot
               '-D', str(out / 'qemu.log')]
    (out / 'command.json').write_text(json.dumps(command, indent=2))
    env = os.environ.copy()
    if os.name == 'nt':
        env['PATH'] = r'D:\Toolchains\msys64\mingw64\bin;C:\msys64\mingw64\bin;' + env.get('PATH', '')
    result = {}
    upload = bytes(((i * 131) ^ (i >> 7)) & 0xff for i in range(96 * 1024))
    app_bytes = app.read_bytes()
    with (out / 'stderr.log').open('wb') as log:
        process = subprocess.Popen(command, env=env, stdout=log, stderr=log)
        qmp = qt = None
        try:
            for _ in range(100):
                try:
                    qmp = BOOT['capture'].QMP(qmp_port)
                    qt = BOOT['QT'](qtest_port)
                    break
                except OSError:
                    time.sleep(0.1)
            assert qmp and qt, 'QEMU did not start'
            host = Host(usb_port)

            def netstat():
                return {n: qt.read(net_base + 4 * i) for i, n in enumerate(NET_FIELDS)}

            def title():
                return string(qt, status(qt, base)['ui_title'])

            def wait_title(want, seconds=10):
                end = time.monotonic() + seconds
                while time.monotonic() < end:
                    if title() == want:
                        return
                    time.sleep(0.1)
                raise AssertionError(f'screen {title()!r}, wanted {want!r}')

            def shot(name):
                qmp.command('screendump', {'filename': str(out / f'{name}.png'), 'format': 'png'})

            wait_for(qt, base, lambda s: s['magic'] == BOOT['MAGIC'], 10, 'OS never started')
            wait_for(qt, base, lambda s: s['ui_depth'] == 1, 60, 'no UI')
            wait_title('B310E OS')
            time.sleep(1)
            if args.background:
                # no screen: the cable at boot starts the network by itself
                host.wait_attached(20)
                assert title() == 'B310E OS', title()
            else:
                qt.write(saved, qt.read(home + SCREEN_TICK))
                qt.write(home + SCREEN_TICK, hook)
                wait_title('USB network')
                host.wait_attached(20)
            assert netstat()['up'] == 1

            # enumeration, as Windows does it
            net = Rndis(host)
            e = net.enumerate()
            dev = e['dev']
            assert dev[4:7] == b'\xef\x02\x01', dev.hex()          # interface associations
            assert struct.unpack_from('<HHH', dev, 8) == (0x1209, 0x0001, 0x0200), dev.hex()
            assert e['msos'][2:16].decode('utf-16-le') == 'MSFT100', e['msos'].hex()
            assert e['compat'][18:23] == b'RNDIS' and e['compat'][26:33] == b'5162001', e['compat'].hex()
            assert e['compat'][16] == 0 and struct.unpack_from('<IHH', e['compat']) == (40, 0x100, 4)
            cfg = e['config']
            assert cfg[9:17] == bytes([8, 0x0b, 0, 2, 0xe0, 0x01, 0x03, 0]), cfg[9:17].hex()
            ifs = e['interfaces']
            assert [(i['class'], i['subclass'], i['protocol']) for i in ifs] == [(0xe0, 1, 3), (0x0a, 0, 0)], ifs
            assert ifs[0]['endpoints'] == [{'address': 0x83, 'type': 3, 'mps': 8}], ifs
            assert sorted((p['address'], p['type']) for p in ifs[1]['endpoints']) == [(0x02, 2), (0x81, 2)]
            assert e['props'] == 'stalled'
            result['enumeration'] = {'product': e['product'], 'serial': e['serial'],
                                     'ms_vendor_code': e['msos'][16], 'compat': 'RNDIS/5162001'}

            # RNDIS control
            empty = host.control(0xa1, 1, 0, 0, 4096)
            assert empty == b'\0', empty.hex()
            init = net.command(2, struct.pack('<III', 1, 0, 0x4000))
            st, major, minor, flags, medium, per, maxt = struct.unpack_from('<IIIIIII', init, 12)
            assert (st, major, flags, medium, per) == (0, 1, 1, 0, 1) and maxt >= 1558, init.hex()
            _, oids = net.query(0x00010101)
            oids = struct.unpack(f'<{len(oids) // 4}I', oids)
            for oid in (0x00010101, 0x00010106, 0x00010107, 0x0001010e, 0x00010114,
                        0x01010101, 0x01010102):
                assert oid in oids, hex(oid)
            _, mac = net.query(0x01010101)
            assert len(mac) == 6 and not mac[0] & 1, mac.hex()
            net.mac = mac
            frame_size = struct.unpack('<I', net.query(0x00010106)[1])[0]
            speed = struct.unpack('<I', net.query(0x00010107)[1])[0]
            connected = struct.unpack('<I', net.query(0x00010114)[1])[0]
            assert (frame_size, speed, connected) == (1500, 120000, 0)
            st, _ = net.query(0x00fd0203, want_ok=False)          # unsupported
            assert st == 0xc00000bb, hex(st)
            assert netstat()['link'] == 0, 'link up before the packet filter'
            assert net.set(0x0001010e, struct.pack('<I', 0x0b)) == 0
            assert net.set(0x01010103, bytes(range(60))) == 0      # 88-byte message: 2 packets
            assert netstat()['link'] == 1
            keep = net.command(8)
            assert struct.unpack_from('<I', keep, 12)[0] == 0
            result['rndis'] = {'model_races_retried': net.model_races, 'max_transfer': maxt, 'oids': len(oids), 'mac': mac.hex(':'),
                               'frame_size': frame_size, 'link_kbit': speed // 10}

            # DHCP, ARP, ping
            result['dhcp'] = net.dhcp()
            assert result['dhcp']['ack'] == HOST_IP and result['dhcp']['server'] == PHONE_IP
            assert result['dhcp']['mask'] == '255.255.255.0' and not result['dhcp']['router']
            phone_mac = net.arp(PHONE_IP)
            result['phone_mac'] = phone_mac.hex(':')
            result['ping_ms'] = [net.ping(56, 1), net.ping(1400, 2), net.ping(1472, 3)]
            assert netstat()['pings'] == 3
            # a PC that gave itself a link-local address (Windows, when it did
            # not ask our DHCP server): the phone answers at 169.254.7.1 too
            dhcp_ip = net.ip
            net.ip = ip2b(HOST_LL)
            assert net.arp(PHONE_LL) == phone_mac
            result['ping_ll_ms'] = net.ping(56, 4, dst=PHONE_LL)
            net.ip = dhcp_ip
            net.arp(PHONE_IP)
            net.ping(56, 5)
            assert netstat()['pings'] == 5

            # the clock from the network: the phone asks the PC (SNTP, port
            # 123) once it has seen it; the answer sets the RTC (UTC, zone 0)
            import calendar
            want = calendar.timegm((2026, 11, 7, 12, 34, 56))
            proto, src, req, mac = net.wait_ip(
                lambda proto, src, p, mac: proto == 17 and struct.unpack_from('!H', p, 2)[0] == 123,
                seconds=15)
            assert len(req) >= 8 + 48 and req[8] & 7 == 3, req.hex()        # a client's question
            answer = bytearray(48)
            answer[0], answer[1] = 0x24, 2                                 # version 4, server; stratum 2
            struct.pack_into('!II', answer, 40, want + 2208988800, 0)      # the transmit time
            net.udp(123, 123, src, bytes(answer))
            end = time.monotonic() + 5
            day = (want - calendar.timegm((1980, 1, 1, 0, 0, 0))) // 86400
            while time.monotonic() < end and (qt.read(0x8200160c) & 0xffff) != day:
                net.pump(0.05)
            rtc = [qt.read(0x82001600 + 4 * i) & 0xffff for i in range(4)]    # s m h day
            result['ntp_rtc'] = rtc
            assert rtc[3] == day and rtc[2] == 12 and rtc[1] == 34, (rtc, day)

            # the raw shell port
            sh = Tcp(net, RAW_PORT)
            sh.connect()
            banner = sh.read_until(PROMPT).decode()
            assert 'B310E OS shell' in banner, banner
            if args.background:
                result['background'] = {'screen': title(), 'shell': banner.strip()[:40]}
                shot('background')
                raise Done
            help_text = sh.cmd('help')
            for name in ('ps', 'mem', 'log', 'peek', 'poke', 'ls', 'put', 'get', 'run', 'reboot'):
                assert f'  {name} ' in help_text, name
            info = sh.cmd('info')
            assert 'B310E OS, built' in info and 'card: mounted' in info, info
            ps = sh.cmd('ps')
            for name in ('net', 'netsh', 'shell', 'usb', 'ui', 'idle'):
                assert any(line.split()[:1] == [name] for line in ps.splitlines()), (name, ps)
            result['ps'] = ps.splitlines()
            mem = sh.cmd('mem')
            assert 'heap:' in mem and 'bytes free' in mem, mem
            logs = sh.cmd('log 400')
            assert 'net: up' in logs, logs[-500:]
            peek = sh.cmd('peek 0x04000000 4')
            vec = struct.unpack('<4I', image.read_bytes()[:16])
            assert peek.split()[1:5] == [f'{v:08x}' for v in vec], (peek, vec)
            refused = sh.cmd('peek 0x30000000')
            assert refused.startswith('ERR') and 'safe ranges' in refused, refused
            forced = sh.cmd('peek -f 0x30000000')
            result['peek_unmapped_forced'] = forced.strip()
            # the high-vector section has one page mapped: the rest faults
            aborted = sh.cmd('peek -f 0xfff00000')
            assert 'data abort' in aborted, aborted
            aborted_poke = sh.cmd('poke -f 0xfff00000 1')
            assert aborted_poke.startswith('ERR data abort'), aborted_poke
            assert sh.cmd('peek -f 0xffff0000').startswith('ffff0000: e59ff018')     # the vector page
            result['abort_caught'] = [aborted.strip(), aborted_poke.strip()]
            poke = sh.cmd('poke 0x043f9ffc 0x1234 h')
            assert '= 0x1234' in poke, poke
            intc = sh.cmd('peek 0x80000008')                   # the interrupt enable mask
            assert intc.startswith('80000008:'), intc
            result['peek'] = {'vectors': peek.strip(), 'refused': refused.strip(),
                              'poke': poke.strip(), 'intc_enable': intc.strip()}

            # files
            assert sh.cmd('mkdir /nettest').strip() == 'OK'
            assert sh.cmd('cd /nettest') == '' and sh.cmd('pwd').strip() == '/nettest'
            sh.send(f'put data.bin {len(upload)}\n'.encode())
            assert sh.read_until(b'\n') == b'READY\n'
            t0 = time.monotonic()
            sh.send(upload)
            okline = sh.read_until(PROMPT)[:-len(PROMPT)].decode()
            put_s = time.monotonic() - t0
            assert okline.strip() == f'OK {len(upload)} {zlib.crc32(upload):08x}', okline
            result['put_kib_per_s'] = round(len(upload) / 1024 / put_s, 1)
            ls = sh.cmd('ls')
            assert f'{len(upload)}' in ls and 'data.bin' in ls and '1 entries' in ls, ls
            crc = sh.cmd('crc data.bin').split()
            assert crc[:2] == [f'{zlib.crc32(upload):08x}', str(len(upload))], crc
            sh.send(b'get /nettest/data.bin\n')
            head = sh.read_until(b'\n')
            assert head == f'DATA {len(upload)}\n'.encode(), head
            t0 = time.monotonic()
            back = sh.read_exact(len(upload))
            get_s = time.monotonic() - t0
            end = sh.read_until(PROMPT)
            assert back == upload, 'downloaded file differs'
            assert end.startswith(f'END {zlib.crc32(upload):08x}'.encode()), end
            result['get_kib_per_s'] = round(len(upload) / 1024 / get_s, 1)
            text = b'hello from the PC\nsecond line\n'
            sh.send(f'put note.txt {len(text)}\n'.encode())
            sh.read_until(b'READY\n')
            sh.send(text)
            sh.read_until(PROMPT)
            assert sh.cmd('cat note.txt') == text.decode()
            assert sh.cmd('mv note.txt kept.txt').strip() == 'OK'
            assert sh.cmd('cat kept.txt') == text.decode()
            missing = sh.cmd('cat note.txt')
            assert missing.startswith('ERR') and 'no such file' in missing, missing
            assert sh.cmd('df').strip().endswith('MB')
            sh.send(b'put gone.txt 3\n')
            sh.read_until(b'READY\n')
            sh.send(b'abc')
            sh.read_until(PROMPT)
            assert sh.cmd('rm gone.txt').strip() == 'OK'
            sh.send(b'shot\n')
            assert sh.read_until(b'\n') == b'DATA 40960\n'
            pixels = sh.read_exact(40960)
            sh.read_until(PROMPT)
            write_png(out / 'shot.png', pixels)
            result['shot_nonblank'] = len(set(pixels)) > 4

            # live coding: an app uploaded and run, closed
            sh.send(f'put /b310e-os/apps/net.app {len(app_bytes)}\n'.encode())
            sh.read_until(b'READY\n')
            sh.send(app_bytes)
            assert sh.read_until(PROMPT).decode().startswith('OK'), 'app upload'
            # a second session, on the telnet port, starts it while this one follows the log
            sh.send(b'log -f\n')
            sh.read_until(b'any key stops --')
            tn = Tcp(net, TELNET_PORT)
            tn.connect()
            neg = tn.read_until(PROMPT)
            assert neg.startswith(bytes([255, 251, 1, 255, 251, 3])), neg[:8]
            tn.send(bytes([255, 253, 1, 255, 253, 3]))          # DO ECHO, DO SGA
            tn.send(b'echo hi\r\n')
            echoed = tn.read_until(PROMPT)
            assert echoed == b'echo hi\r\nhi\r\n' + PROMPT, echoed
            tn.send(b'run net\r\n')
            ran = tn.read_until(PROMPT)
            assert b'OK /b310e-os/apps/net.app started' in ran, ran
            wait_title('Stopwatch')
            assert status(qt, base)['apps_loaded'] == 1
            followed = sh.read_until(b'app: Stopwatch loaded')
            sh.send(b'\n')
            sh.read_until(PROMPT)
            shot('net-app')
            keys = status(qt, base)['keys']
            assert sh.cmd('key center').strip() == 'OK'
            assert status(qt, base)['keys'] >= keys + 2
            assert 'OK' in sh.cmd('run stopwatch')              # replaces the open one
            wait_title('Stopwatch')
            time.sleep(0.5)
            assert status(qt, base)['apps_loaded'] == 1, status(qt, base)['apps_loaded']
            assert sh.cmd('close').startswith('OK')
            wait_title('USB network')
            time.sleep(0.5)
            assert status(qt, base)['apps_loaded'] == 0
            tn.send(b'bad\x08\x08\x08exit\r\n')                 # Backspace editing
            tn.read_until(b'bye')
            end = time.monotonic() + 5
            while not tn.fin:
                assert time.monotonic() < end, 'telnet session did not close'
                net.pump(0.01)
            result['live_coding'] = {'log_follow_saw_load': True, 'telnet_echo': True}

            # a lost segment from the phone: it retransmits
            before = netstat()['tcp_retransmits']
            net.drop_next_from_phone = 1
            out_text = sh.cmd('help', seconds=30)
            assert '  peek ' in out_text
            assert netstat()['tcp_retransmits'] > before, netstat()
            # frames that end exactly on a packet boundary: 44 + 1492 = 1536
            pad = b'x' * (1492 - 14 - 20 - 8)
            net.udp(5555, 9, ip2b(PHONE_IP), pad)
            assert net.ping(56, 4) > 0                          # still in step
            result['retransmit_ok'] = True

            # RNDIS RESET and KEEPALIVE while TCP is open
            reset = net.command(6, b'', expect=True)
            assert struct.unpack_from('<I', reset, 8)[0] == 0, reset.hex()
            net.command(8)
            assert 'B310E OS' in sh.cmd('info')

            # the PC helper (os/tools/b310e-shell.py) on a third session
            ht = Tcp(net, RAW_PORT)
            ht.connect()
            helper = HELPER['Shell'](None, None, sock=TcpSock(ht))
            blob = bytes(random.getrandbits(8) for _ in range(20000))
            helper.put('/nettest/helper.bin', blob)
            assert helper.get('/nettest/helper.bin') == blob
            assert HELPER['png'](helper.shot()).startswith(b'\x89PNG')
            helper.put('/b310e-os/apps/helper.app', app_bytes)
            assert helper.cmd('run "/b310e-os/apps/helper.app"').startswith('OK')
            wait_title('Stopwatch')
            assert helper.cmd('close').startswith('OK')
            wait_title('USB network')
            try:
                helper.put('/no/such/folder/x', b'1')
                raise AssertionError('put into a missing folder succeeded')
            except HELPER['ShellError'] as err:
                assert 'no such path' in str(err), err
            helper.send(b'exit\n')
            helper.read_until(b'bye')
            ht.close()
            result['helper_ok'] = True

            sh.send(b'exit\n')
            sh.read_until(b'bye')
            sh.close()
            time.sleep(0.5)
            s = netstat()
            result['net_stat'] = s
            assert s['shell_sessions'] == 3 and s['files_in'] >= 6 and s['apps_run'] == 3, s
            assert s['tx_timeouts'] == 0 and s['shell_open'] == 0, s
            shot('usb-network')

            # unplug: the screen closes, the stack stops
            qt.command('set_irq_in /machine/peripheral/sc6530-adi charger-input 0 0')
            wait_title('B310E OS', 10)
            end = time.monotonic() + 10
            while host.query()['attached']:
                assert time.monotonic() < end, 'still on the bus'
                time.sleep(0.1)
            assert netstat()['up'] == 0 and qt.read(usb_base) == 0
            time.sleep(2.5)                                     # the log thread writes the card
            result['os_status'] = {k: v for k, v in status(qt, base).items()
                                   if k in ('idle_permille', 'heap_free', 'stray_irqs')}

            # plugged in again: a second session; top, adi; reboot from the shell
            qt.command('set_irq_in /machine/peripheral/sc6530-adi charger-input 0 1')
            time.sleep(1.5)
            qt.write(saved, qt.read(home + SCREEN_TICK))
            qt.write(home + SCREEN_TICK, hook)
            wait_title('USB network')
            host.wait_attached(20)
            net = Rndis(host)
            net.enumerate()
            net.command(2, struct.pack('<III', 1, 0, 0x4000))
            net.mac = net.query(0x01010101)[1]
            net.set(0x0001010e, struct.pack('<I', 0x0b))
            assert net.dhcp()['ack'] == HOST_IP
            net.arp(PHONE_IP)
            sh = Tcp(net, RAW_PORT)
            sh.connect()
            sh.read_until(PROMPT)
            sh.send(b'top\n')
            sh.read_until(b'load ')
            sh.read_until(b'load ')                             # a second round
            sh.send(b'\n')
            sh.read_until(PROMPT)
            adi = sh.cmd('adi 0x82001184')
            assert adi.startswith('0x82001184 = 0x'), adi
            assert sh.cmd('adi 0x20000000').startswith('usage')
            ticks = status(qt, base)['ticks']
            sh.send(b'reboot\n')
            sh.read_until(b'rebooting')
            end = time.monotonic() + 30
            while True:                                         # the OS starts over
                assert time.monotonic() < end, 'no restart after reboot'
                time.sleep(0.2)
                try:
                    s = status(qt, base)
                except Exception:
                    continue
                if s['magic'] == BOOT['MAGIC'] and s['ticks'] < ticks and s['ui_depth'] == 1:
                    break
            wait_title('B310E OS', 20)
            result['replug_and_reboot_ok'] = True
            time.sleep(2.5)
            qmp.command('quit')
            process.wait(timeout=20)
        except Done:                    # --background: all it checks passed
            pass
        except Exception:
            if qmp and process.poll() is None:
                try:
                    shot('failure')
                    result['net_stat_failure'] = netstat()
                    result['threads_failure'] = threads(qt, elf)
                except Exception:
                    pass
            print(json.dumps(result, indent=2, default=str))
            raise
        finally:
            if qt:
                qt.close()
            if qmp:
                qmp.close()
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=10)

    if args.background:
        log_text = (BOOT['card_file'](card.read_bytes(), '/b310e-os/log.txt') or b'').decode(errors='replace')
        result['card_log'] = [l for l in log_text.splitlines() if 'usb:' in l and 'cmd' not in l]
        (out / 'result.json').write_text(json.dumps(result, indent=2, default=str))
        print('PASS ' + json.dumps(result, default=str))
        return
    img = card.read_bytes()
    assert BOOT['card_file'](img, '/nettest/data.bin') == upload, 'upload not in the card image'
    assert BOOT['card_file'](img, '/nettest/kept.txt') == text
    assert BOOT['card_file'](img, '/nettest/gone.txt') is None
    assert BOOT['card_file'](img, '/b310e-os/apps/net.app') == app_bytes
    assert BOOT['card_file'](img, '/nettest/helper.bin') == blob
    log_text = (BOOT['card_file'](img, '/b310e-os/log.txt') or b'').decode(errors='replace')
    (out / 'card-log.txt').write_text(log_text)
    result['card_log'] = [l for l in log_text.splitlines() if 'net:' in l or 'usb: network' in l]
    assert 'usb: network session 1 ended (unplugged)' in log_text, result['card_log']
    assert 'reboot to the boot menu' in log_text
    assert 'net: /nettest/data.bin received' in log_text
    result['image_files_verified'] = True
    (out / 'result.json').write_text(json.dumps(result, indent=2, default=str))
    print(json.dumps(result, indent=2, default=str))


def threads(qt, elf):
    """Every thread (kernel/thread.c's list) with its state and the CPU it
    took over half a second: what a failure was stuck in."""
    def walk():
        out = {}
        t = qt.read(symbol(elf, 'all_threads'))
        while t and len(out) < 40:
            word = qt.read(t + 20)
            out[t] = {'name': string(qt, qt.read(t + 40)), 'prio': word & 0xff,
                      'state': word >> 16 & 0xff, 'cycles': qt.read(t + 52),
                      'waitq': hex(qt.read(t + 28)), 'thread': hex(t)}
            t = qt.read(t + 56)
        return out
    a = walk()
    time.sleep(0.5)
    b = walk()
    cur = qt.read(symbol(elf, 'kernel_current'))
    return [dict(v, cpu=v['cycles'] - a.get(k, v)['cycles'], current=k == cur) for k, v in b.items()]


def write_png(path, rgb565):
    rows = []
    for y in range(160):
        row = bytearray(b'\0')
        for x in range(128):
            v = rgb565[(y * 128 + x) * 2] | rgb565[(y * 128 + x) * 2 + 1] << 8
            row += bytes(((v >> 11) * 255 // 31, (v >> 5 & 63) * 255 // 63, (v & 31) * 255 // 31))
        rows.append(bytes(row))

    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 128, 160, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(b''.join(rows))) + chunk(b'IEND', b'')
    path.write_bytes(png)


if __name__ == '__main__':
    main()
