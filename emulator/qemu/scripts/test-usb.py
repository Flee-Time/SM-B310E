#!/usr/bin/env python3
"""Boot Rockbox with a host on the emulated USB port and test mass storage.

The script is the USB host: it talks to the sc6530_usb model's chardev
(protocol in hw/misc/sc6530_usb.c), enumerates the device the way a PC does,
then drives Bulk-Only Transport SCSI commands against the SD card Rockbox
exports. A sector written over USB is checked in the card image afterwards.
"""
import argparse
import importlib.util
import json
import os
import runpy
import shutil
import socket
import struct
import subprocess
import time
from pathlib import Path

spec = importlib.util.spec_from_file_location('stock_capture', Path(__file__).with_name('capture-stock.py'))
capture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(capture)
qtest_api = runpy.run_path(str(Path(__file__).with_name('test-audio.py')))


class UsbError(Exception):
    pass


class Stall(UsbError):
    pass


class Host:
    """Packet-level host over the sc6530_usb chardev protocol."""

    def __init__(self, port, timeout=30.0):
        self.sock = socket.create_connection(('127.0.0.1', port), timeout=timeout)
        self.timeout = timeout
        self.address = 0

    def _request(self, op, ep=0, payload=b''):
        self.sock.sendall(struct.pack('<BBH', ord(op), ep, len(payload)) + payload)
        head = self._recv(3)
        code, size = chr(head[0]), head[1] | head[2] << 8
        return code, self._recv(size) if size else b''

    def _recv(self, n):
        data = b''
        while len(data) < n:
            chunk = self.sock.recv(n - len(data))
            if not chunk:
                raise UsbError('USB host link closed')
            data += chunk
        return data

    def _retry(self, what, fn):
        deadline = time.monotonic() + self.timeout
        while True:
            code, data = fn()
            if code in 'AD':
                return data
            if code == 'T':
                raise Stall(what)
            if time.monotonic() > deadline:
                raise UsbError(f'{what}: NAK timeout')
            time.sleep(0.0005)

    def query(self):
        _, data = self._request('Q')
        return {'attached': bool(data[0]), 'address': data[1],
                'ctrl': struct.unpack_from('<I', data, 2)[0]}

    def wait_attached(self, seconds):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            if self.query()['attached']:
                return
            time.sleep(0.1)
        raise UsbError('device never connected (CTRL enable + pull-up)')

    def bus_reset(self):
        self._request('R')

    def setup(self, req):
        self._retry('SETUP', lambda: self._request('S', 0, req))

    def packet_in(self, ep, maxlen=64):
        return self._retry(f'IN ep{ep}', lambda: self._request('I', ep, struct.pack('<H', maxlen)))

    def packet_out(self, ep, data):
        self._retry(f'OUT ep{ep}', lambda: self._request('O', ep, data))

    def control(self, bm, req, value=0, index=0, data_or_len=0):
        out = isinstance(data_or_len, (bytes, bytearray))
        length = len(data_or_len) if out else data_or_len
        self.setup(struct.pack('<BBHHH', bm, req, value, index, length))
        if bm & 0x80:
            data = b''
            while len(data) < length:
                packet = self.packet_in(0)
                data += packet
                if len(packet) < 64:
                    break
            self.packet_out(0, b'')          # status stage
            return data
        for i in range(0, length, 64):
            self.packet_out(0, bytes(data_or_len[i:i + 64]))
        self.packet_in(0)                   # status stage
        return b''

    def bulk_out(self, ep, data):
        for i in range(0, len(data), 64):
            self.packet_out(ep, data[i:i + 64])

    def bulk_in(self, ep, length):
        data = b''
        while len(data) < length:
            packet = self.packet_in(ep)
            data += packet
            if len(packet) < 64:
                break
        return data


def get_descriptor(host, kind, index=0, length=255, lang=0):
    return host.control(0x80, 6, kind << 8 | index, lang, length)


def string_descriptor(host, index):
    if not index:
        return None
    raw = get_descriptor(host, 3, index, 255, 0x0409)
    return raw[2:raw[0]].decode('utf-16-le', 'replace')


def parse_config(config):
    interfaces = []
    pos = 0
    while pos < len(config):
        size, kind = config[pos], config[pos + 1]
        if kind == 4:
            interfaces.append({'number': config[pos + 2], 'class': config[pos + 5],
                               'subclass': config[pos + 6], 'protocol': config[pos + 7],
                               'endpoints': []})
        elif kind == 0x21 and interfaces:   # HID class descriptor
            interfaces[-1]['report_length'] = struct.unpack_from('<H', config, pos + 7)[0]
        elif kind == 5:
            address, attrs, mps = config[pos + 2], config[pos + 3], struct.unpack_from('<H', config, pos + 4)[0]
            interfaces[-1]['endpoints'].append({'address': address, 'type': attrs & 3, 'mps': mps})
        pos += size
    return interfaces


def enumerate_device(host):
    """Enumerate in the order Windows 11 does (the phone's usb-log.txt,
    2026-11-02), up to the mass storage driver's GET_MAX_LUN."""
    host.bus_reset()
    get_descriptor(host, 1, length=64)
    host.bus_reset()
    host.control(0x00, 5, 7)               # SET_ADDRESS 7
    dev = get_descriptor(host, 1, length=18)
    assert len(dev) == 18 and dev[1] == 1, dev.hex()
    vid, pid = struct.unpack_from('<HH', dev, 8)
    get_descriptor(host, 2, length=255)
    get_descriptor(host, 3, 0, 255)
    get_descriptor(host, 3, dev[15], 255, 0x0409)
    try:
        get_descriptor(host, 6, length=10)  # device qualifier: full speed only
        qualifier = 'answered'
    except Stall:
        qualifier = 'stalled'
    get_descriptor(host, 1, length=18)
    head = get_descriptor(host, 2, length=9)
    total = struct.unpack_from('<H', head, 2)[0]
    config = get_descriptor(host, 2, length=total)
    assert len(config) == total, (len(config), total)
    for _ in range(2):
        size = get_descriptor(host, 3, dev[15], 4, 0x0409)[0]
        get_descriptor(host, 3, dev[15], size, 0x0409)
    get_descriptor(host, 3, 0, 255)
    info = {'vid': f'{vid:04x}', 'pid': f'{pid:04x}', 'max_packet0': dev[7],
            'device_qualifier': qualifier, 'config_value': config[5],
            'interfaces': parse_config(config)}
    host.control(0x00, 9, info['config_value'])  # SET_CONFIGURATION
    msc_if = next(i for i in info['interfaces'] if i['class'] == 8)
    try:
        info['max_lun'] = host.control(0xa1, 0xfe, 0, msc_if['number'], 1)[0]
    except Stall:
        info['max_lun'] = 0
    info['manufacturer'] = string_descriptor(host, dev[14])
    info['product'] = string_descriptor(host, dev[15])
    info['serial'] = string_descriptor(host, dev[16])
    return info


def start_hid(host, info):
    """Windows' HID driver, after mass storage is up: SET_IDLE, then the
    report descriptor with 64 bytes more than its length (the request that
    failed on the phone)."""
    for interface in info['interfaces']:
        if interface['class'] == 3 and 'report_length' in interface:
            length = interface['report_length']
            host.control(0x21, 0x0a, 0, interface['number'])
            report = host.control(0x81, 6, 0x22 << 8, interface['number'], length + 64)
            assert len(report) == length, f'HID report descriptor {len(report)} of {length} bytes'
            interface['report_received'] = len(report)


class MassStorage:
    def __init__(self, host, interface):
        self.host = host
        self.interface = interface['number']
        eps = [e for e in interface['endpoints'] if e['type'] == 2]
        self.ep_in = next(e['address'] & 0xf for e in eps if e['address'] & 0x80)
        self.ep_out = next(e['address'] & 0xf for e in eps if not e['address'] & 0x80)
        self.tag = 1

    def max_lun(self):
        try:
            return self.host.control(0xa1, 0xfe, 0, self.interface, 1)[0]
        except Stall:
            return 0

    def command(self, cdb, data_in=0, data_out=b''):
        length = data_in or len(data_out)
        flags = 0x80 if data_in else 0
        cbw = struct.pack('<IIIBBB', 0x43425355, self.tag, length, flags, 0, len(cdb)) + cdb.ljust(16, b'\0')
        self.tag += 1
        self.host.bulk_out(self.ep_out, cbw)
        data = b''
        if data_in:
            data = self.host.bulk_in(self.ep_in, data_in)
        elif data_out:
            self.host.bulk_out(self.ep_out, data_out)
        csw = self.host.bulk_in(self.ep_in, 13)
        sig, tag, residue, status = struct.unpack('<IIIB', csw)
        assert sig == 0x53425355 and tag == self.tag - 1, csw.hex()
        return status, residue, data

    def wait_ready(self, seconds):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            status, _, _ = self.command(b'\x00' * 6)    # TEST UNIT READY
            if status == 0:
                return
            self.command(b'\x03\0\0\0\x12\0', data_in=18)  # REQUEST SENSE
            time.sleep(0.5)
        raise UsbError('medium never became ready')

    def read10(self, lba, count, block):
        status, _, data = self.command(struct.pack('>BBIBHB', 0x28, 0, lba, 0, count, 0), data_in=count * block)
        assert status == 0, status
        return data

    def write10(self, lba, data, block):
        status, _, _ = self.command(struct.pack('>BBIBHB', 0x2a, 0, lba, 0, len(data) // block, 0), data_out=data)
        assert status == 0, status


def watchdog_resets(output):
    trace = output / 'trace.log'
    text = trace.read_text(errors='replace') if trace.exists() else ''
    return text.count('watchdog expired')


def freeze_test(args, host, qtest_port, block, original, result):
    """Hang the phone in a USB session: every interrupt masked, so the tick
    stops and nothing feeds the watchdog. It must reset the machine, Rockbox
    must come back and connect again, and that session's card log must list
    the frozen one's breadcrumbs."""
    # a write first, so the breadcrumbs show the OUT DMA steps
    scratch = len(original) // block - 600
    msc = MassStorage(host, next(i for i in enumerate_device(host)['interfaces']
                                 if i['class'] == 8))
    msc.wait_ready(60)
    msc.write10(scratch, bytes(range(256)) * 256, block)
    qt = qtest_api['QTest'](qtest_port)
    qt.command('writel 0x8000000c 0xffffffff')       # INTC: disable all
    t0 = time.monotonic()
    while not watchdog_resets(args.output):
        if time.monotonic() - t0 > 30:
            raise AssertionError('no watchdog reset 30 s after the freeze')
        time.sleep(0.2)
    result['freeze_reset_seconds'] = round(time.monotonic() - t0, 2)
    qt.close()
    deadline = time.monotonic() + 30
    while host.query()['attached']:
        if time.monotonic() > deadline:
            raise UsbError('still attached 30 s after the watchdog reset')
        time.sleep(0.2)
    host.wait_attached(args.attach_timeout)
    info = enumerate_device(host)
    msc = MassStorage(host, next(i for i in info['interfaces'] if i['class'] == 8))
    msc.wait_ready(60)
    assert msc.read10(0, 1, block) == original[:block], 'sector 0 after the reset'
    result['reconnected_after_reset'] = True
    result['watchdog_resets'] = watchdog_resets(args.output)


def finish_freeze(args, card, result):
    image = card.read_bytes()
    marker = b'previous USB session never ended'
    at = image.find(marker)
    assert at >= 0, 'card log has no breadcrumbs of the frozen session'
    start = image.rfind(b'---', 0, at)
    excerpt = image[at:at + 4000].split(b'\0')[0].decode(errors='replace')
    lines = excerpt.splitlines()
    result['crumb_lines'] = [l for l in lines[:70] if 'step' in l or 'never' in l]
    assert any(' S ' in l for l in result['crumb_lines']), 'no session start crumb'
    (args.output / 'result.json').write_text(json.dumps(result, indent=2))
    print(json.dumps(result, indent=2), flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qemu', type=Path, required=True)
    parser.add_argument('--rockbox', type=Path, required=True)
    parser.add_argument('--sdcard', type=Path, required=True, help='card image (a copy is used)')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--attach-timeout', type=float, default=60)
    parser.add_argument('--read-kib', type=int, default=256, help='throughput read size')
    parser.add_argument('--irq-stuck', action='store_true',
                        help='hold IRQ 25 high: exercises the polled fallback')
    parser.add_argument('--no-dma-out', action='store_true',
                        help='raise no OUT DMA requests: exercises the OUT salvage')
    parser.add_argument('--watchdog', action='store_true',
                        help='model the hardware watchdog; the run must not reset')
    parser.add_argument('--freeze', action='store_true',
                        help='mask every interrupt mid-session: the watchdog must '
                             'reset the phone and the next session log the breadcrumbs')
    parser.add_argument('--no-dma', action='store_true',
                        help='raise no USB DMA requests: exercises the PIO fallback')
    args = parser.parse_args()
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    card = args.output / 'sdcard.img'
    shutil.copyfile(args.sdcard, card)
    original = card.read_bytes()

    ports = []
    for _ in range(3):
        with socket.socket() as probe:
            probe.bind(('127.0.0.1', 0))
            ports.append(probe.getsockname()[1])
    qmp_port, usb_port, qtest_port = ports
    command = [str(args.qemu.resolve()), '-M', 'b310e,boot-mode=rockbox',
               '-display', 'none', '-serial', 'none', '-monitor', 'none',
               '-drive', f'file={args.rockbox.resolve().as_posix()},format=raw,if=none,id=os,readonly=on',
               '-drive', f'file={card.as_posix()},format=raw,if=none,id=sdcard',
               '-qmp', f'tcp:127.0.0.1:{qmp_port},server=on,wait=off',
               '-qtest', f'tcp:127.0.0.1:{qtest_port},server=on,wait=off',
               '-chardev', f'socket,id=usbhost,host=127.0.0.1,port={usb_port},server=on,wait=off',
               '-global', 'sc6530_usb.chardev=usbhost',
               '-global', 'sc6530_adi.charger-present=on',
               '-D', str(args.output / 'trace.log'), '--trace', 'sc6530_usb_setup']
    if args.irq_stuck:
        command += ['-global', 'sc6530_usb.irq-stuck=on']
    if args.no_dma:
        command += ['-global', 'sc6530_usb.dma-requests=off']
    if args.no_dma_out:
        command += ['-global', 'sc6530_usb.dma-out-requests=off']
    if args.watchdog or args.freeze:
        command += ['-global', 'sc6530_adi.watchdog=on']
    (args.output / 'command.json').write_text(json.dumps(command, indent=2))
    env = os.environ.copy()
    if os.name == 'nt':
        env['PATH'] = r'D:\Toolchains\msys64\mingw64\bin;C:\msys64\mingw64\bin;' + env.get('PATH', '')
    result = {}
    qmp = None
    with (args.output / 'stderr.log').open('wb') as log:
        process = subprocess.Popen(command, env=env, stdout=log, stderr=log)
        try:
            for _ in range(100):
                if process.poll() is not None:
                    raise RuntimeError('QEMU exited; see stderr.log')
                try:
                    qmp = capture.QMP(qmp_port)
                    break
                except OSError:
                    time.sleep(.1)
            if qmp is None:
                raise RuntimeError('QMP startup timeout')
            host = Host(usb_port)
            started = time.monotonic()
            host.wait_attached(args.attach_timeout)
            result['attach_seconds'] = round(time.monotonic() - started, 2)
            info = enumerate_device(host)
            result['device'] = info
            result['address'] = host.query()['address']
            msc_if = next(i for i in info['interfaces'] if i['class'] == 8)
            msc = MassStorage(host, msc_if)
            result['max_lun'] = info['max_lun']
            _, _, inquiry = msc.command(b'\x12\0\0\0\x24\0', data_in=36)
            result['inquiry'] = {'vendor': inquiry[8:16].decode(errors='replace').strip(),
                                 'product': inquiry[16:32].decode(errors='replace').strip()}
            start_hid(host, info)
            msc.wait_ready(60)
            _, _, cap = msc.command(b'\x25' + b'\0' * 9, data_in=8)
            last, block = struct.unpack('>II', cap)
            result['capacity'] = {'blocks': last + 1, 'block_size': block}
            assert (last + 1) * block == len(original), (last + 1, block, len(original))

            mbr = msc.read10(0, 1, block)
            assert mbr == original[:block], 'sector 0 differs from the card image'
            if args.freeze:
                freeze_test(args, host, qtest_port, block, original, result)
                qmp.command('quit')
                process.wait(timeout=20)
                return finish_freeze(args, card, result)
            t0 = time.monotonic()
            bulk = msc.read10(64, args.read_kib * 1024 // block, block)
            elapsed = time.monotonic() - t0
            assert len(bulk) == args.read_kib * 1024, f'bulk read: {len(bulk)} bytes'
            # Rockbox writes the card before handing it over (usb-log.txt,
            # FSInfo), so only the area ahead of the first partition must
            # still match the image; all of it must read back the same.
            first_partition = struct.unpack_from('<I', original, 0x1c6)[0]
            fixed = max(0, min(len(bulk), (first_partition - 64) * block))
            expect = original[64 * block:64 * block + fixed]
            if bulk[:fixed] != expect:
                bad = next(i for i, (x, y) in enumerate(zip(bulk, expect)) if x != y)
                raise AssertionError(f'bulk read differs from the image at byte {bad}')
            again = msc.read10(64, args.read_kib * 1024 // block, block)
            if again != bulk:
                bad = next((i for i, (x, y) in enumerate(zip(bulk, again)) if x != y), len(again))
                raise AssertionError(f'second bulk read differs at byte {bad}')
            result['read_kib_per_s'] = round(len(bulk) / 1024 / elapsed, 1)

            # With --no-dma(-out) the OUT DMA never progresses: the driver must
            # FAIL that receive (Rockbox then writes nothing), and the host
            # resets the device and retries, as Windows does. The retried
            # write goes by PIO and must land intact.
            result['write_retries'] = 0

            def write(lba, data):
                nonlocal msc
                try:
                    msc.write10(lba, data, block)
                except (UsbError, AssertionError) as error:
                    if not (args.no_dma or args.no_dma_out) or result['write_retries']:
                        raise
                    result['write_retries'] += 1
                    result['first_write_error'] = str(error)
                    host.bus_reset()
                    info_retry = enumerate_device(host)
                    msc = MassStorage(host, next(i for i in info_retry['interfaces']
                                                 if i['class'] == 8))
                    msc.wait_ready(60)
                    msc.write10(lba, data, block)

            pattern = bytes((i * 7 + 3) & 0xff for i in range(block * 2))
            target = last - 1
            write(target, pattern)
            assert msc.read10(target, 2, block) == pattern, 'read-back after write differs'
            result['write_lba'] = target

            # A large write: bulk OUT runs by DMA in transfers of up to
            # 16 KiB (the SD side splits them into 32-block commands).
            big = bytes(((i * 131) ^ (i >> 9)) & 0xff for i in range(128 * 1024))
            big_lba = target - 8 - len(big) // block
            t0 = time.monotonic()
            write(big_lba, big)
            elapsed = time.monotonic() - t0
            assert msc.read10(big_lba, len(big) // block, block) == big,                 'read-back after the 128 KiB write differs'
            result['big_write_lba'] = big_lba
            result['write_kib_per_s'] = round(len(big) / 1024 / elapsed, 1)
            qmp.command('screendump', {'filename': str(args.output / 'usb-screen.png'), 'format': 'png'})

            # Host reset of a configured device (Windows does this when a
            # command times out; the phone then never recovered): the
            # controller loses its endpoint setup and the host enumerates
            # again without SET_ADDRESS/SET_CONFIGURATION reaching software.
            info_reset = enumerate_device(host)
            msc = MassStorage(host, next(i for i in info_reset['interfaces'] if i['class'] == 8))
            msc.wait_ready(60)
            start_hid(host, info_reset)
            assert msc.read10(0, 1, block) == original[:block], 'sector 0 after host reset'
            assert msc.read10(64, 8, block) == original[64 * block:72 * block], 'read after host reset'
            result['host_reset_ok'] = True

            # Unplug: VBUS goes away (the charger EIC line) and the host
            # with it. Rockbox must shut the controller down and get back to
            # its own UI; then a re-plug must enumerate and serve again.
            qt = qtest_api['QTest'](qtest_port)
            vbus = 'set_irq_in /machine/peripheral/sc6530-adi charger-input 0 {}'
            qt.command(vbus.format(0))
            deadline = time.monotonic() + 30
            while host.query()['attached']:
                if time.monotonic() > deadline:
                    raise UsbError('controller still connected 30 s after unplug')
                time.sleep(0.2)
            result['unplug_detach_seconds'] = round(30 - (deadline - time.monotonic()), 2)
            time.sleep(3)
            qmp.command('screendump', {'filename': str(args.output / 'unplugged.png'), 'format': 'png'})
            qt.command(vbus.format(1))
            host.wait_attached(args.attach_timeout)
            info2 = enumerate_device(host)
            msc = MassStorage(host, next(i for i in info2['interfaces'] if i['class'] == 8))
            msc.wait_ready(60)
            start_hid(host, info2)
            assert msc.read10(0, 1, block) == original[:block], 'sector 0 after re-plug'
            result['replug_ok'] = True
            qt.close()
            qmp.command('quit')
            process.wait(timeout=20)
        except Exception:
            if qmp and process.poll() is None:
                state = {name: qmp.hmp(f'xp /{count}wx {addr:#x}') for name, addr, count in [
                    ('usb', 0x20300000, 8), ('ep0', 0x20300040, 12), ('ep1', 0x203000c0, 6),
                    ('ep2', 0x20300100, 6), ('intc', 0x80000000, 4)]}
                state['registers'] = qmp.hmp('info registers')
                (args.output / 'failure-state.json').write_text(json.dumps(state, indent=2))
                qmp.command('screendump', {'filename': str(args.output / 'failure.png'), 'format': 'png'})
            raise
        finally:
            if qmp:
                qmp.close()
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=10)
    if args.watchdog:
        result['watchdog_resets'] = watchdog_resets(args.output)
        assert result['watchdog_resets'] == 0, 'the watchdog reset a healthy session'
    image = card.read_bytes()
    lba = result['write_lba']
    block = result['capacity']['block_size']
    assert image[lba * block:(lba + 2) * block] == pattern, 'write did not reach the card image'
    big_lba = result['big_write_lba']
    assert image[big_lba * block:big_lba * block + len(big)] == big,         '128 KiB write did not reach the card image'
    result['image_write_verified'] = True
    (args.output / 'result.json').write_text(json.dumps(result, indent=2))
    print(json.dumps(result, indent=2), flush=True)


if __name__ == '__main__':
    main()
