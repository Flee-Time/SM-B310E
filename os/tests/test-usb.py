#!/usr/bin/env python3
"""USB mass storage of the B310E OS in QEMU, with this script as the PC.

Boots the OS with VBUS present and a host on the emulated USB port (the
sc6530_usb model's chardev; protocol in emulator/qemu/machine/hw/misc/
sc6530_usb.c), opens the "USB storage" screen through the test hook in
apps/usb.c (QTest points the home screen's tick at usb_test_hook), then:
  - enumerates in Windows 11's order (device id, strings, the device
    qualifier stalled, GET_MAX_LUN);
  - INQUIRY, TEST UNIT READY, READ CAPACITY(10), READ FORMAT CAPACITIES,
    MODE SENSE(6) and (10), REQUEST SENSE after failures;
  - unsupported commands with data in, data out and none, an unsupported
    mode page, a read past the end: each fails with the right sense and,
    where data was expected, a zero-length packet (in) or taken data (out)
    before the CSW, without a halt (the Rockbox port's way; halts after
    failed commands lost the device on the phone);
  - reads compared with the card image, timed; writes read back, timed;
  - eject and load (START STOP UNIT), mass storage reset, an invalid CBW
    with reset recovery, a host bus reset of the configured device;
  - unplug: the screen closes, the controller leaves the bus, the card is
    mounted again; re-plug and Back (RSoft) do the same;
then checks the written sectors and the card log in the image.

The image must keep usb_test_hook, i.e. reference app_usb (a menu entry,
or link with -Wl,--undefined=app_usb). --irq-stuck holds line 25 high to
exercise the polled fallback.

SPDX-License-Identifier: GPL-2.0-or-later
"""
import argparse
import json
import os
from pathlib import Path
import runpy
import struct
import subprocess
import sys
import time

REPO = Path(__file__).resolve().parents[2]
BOOT = runpy.run_path(str(REPO / 'os/tests/test-boot.py'))
USBHOST = runpy.run_path(str(REPO / 'emulator/qemu/scripts/test-usb.py'))
Host, Stall, UsbError = USBHOST['Host'], USBHOST['Stall'], USBHOST['UsbError']
enumerate_device, get_descriptor = USBHOST['enumerate_device'], USBHOST['get_descriptor']
symbol, status, string, wait_for = BOOT['symbol'], BOOT['status'], BOOT['string'], BOOT['wait_for']

USB_FIELDS = ('state activity last_data_ms bytes_read bytes_written commands failed setups '
              'bus_resets irqs sessions ejected polled sd_errors').split()
USB_OFF, USB_CONNECTED = 0, 4
SCREEN_TICK = 32                        # offsetof(screen_t, tick)
BLOCK = 512


class Msc:
    """Bulk-Only Transport host side, with halts cleared as a PC does."""

    def __init__(self, host, interface):
        self.host = host
        self.interface = interface['number']
        eps = [e for e in interface['endpoints'] if e['type'] == 2]
        self.ep_in = next(e['address'] for e in eps if e['address'] & 0x80)
        self.ep_out = next(e['address'] for e in eps if not e['address'] & 0x80)
        self.tag = 0x100

    def clear_halt(self, ep):
        """CLEAR_FEATURE(ENDPOINT_HALT). The controller ACKs the status
        stage before the driver has seen the request, so the halt may
        still be set for a moment: GET_STATUS (answered by the driver, in
        order) shows when it has gone. The controller has one SETUP
        register: a request that follows before the driver has read the
        last one replaces it (polled mode services every millisecond), so
        the request is sent again if the halt stays."""
        deadline = time.monotonic() + 3
        while True:
            self.host.control(0x02, 1, 0, ep)
            again = time.monotonic() + 0.3
            while time.monotonic() < again:
                time.sleep(0.005)
                if not self.halted(ep):
                    return
            if time.monotonic() > deadline:
                raise UsbError(f'endpoint {ep:#x} still halted after CLEAR_FEATURE')

    def halted(self, ep):
        return self.host.control(0x82, 0, 0, ep, 2)[0] & 1

    def reset_recovery(self):
        self.host.control(0x21, 0xff, 0, self.interface, b'')
        self.clear_halt(self.ep_in)
        self.clear_halt(self.ep_out)

    def command(self, cdb, data_in=0, data_out=b'', length=None):
        if length is None:
            length = data_in or len(data_out)
        flags = 0x80 if data_in else 0
        self.tag += 1
        cbw = struct.pack('<IIIBBB', 0x43425355, self.tag, length, flags, 0, len(cdb)) + cdb.ljust(16, b'\0')
        self.host.bulk_out(self.ep_out & 0xf, cbw)
        data, stalled = b'', None
        try:
            if data_in:
                data = self.host.bulk_in(self.ep_in & 0xf, data_in)
            elif data_out:
                self.host.bulk_out(self.ep_out & 0xf, data_out)
        except Stall:
            stalled = 'in' if data_in else 'out'
            self.clear_halt(self.ep_in if data_in else self.ep_out)
        try:
            csw = self.host.bulk_in(self.ep_in & 0xf, 13)
        except Stall:
            stalled = stalled or 'in'
            self.clear_halt(self.ep_in)
            csw = self.host.bulk_in(self.ep_in & 0xf, 13)
        sig, tag, residue, st = struct.unpack('<IIIB', csw)
        assert sig == 0x53425355 and tag == self.tag, f'bad CSW {csw.hex()}'
        return st, residue, data, stalled

    def sense(self):
        st, _, data, _ = self.command(b'\x03\0\0\0\x12\0', data_in=18)
        assert st == 0 and len(data) == 18 and data[0] == 0x70, data.hex()
        return data[2] & 0xf, data[12], data[13]

    def wait_ready(self, seconds):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            if self.command(b'\0' * 6)[0] == 0:
                return
            self.sense()
            time.sleep(0.3)
        raise UsbError('medium never became ready')

    def read10(self, lba, count):
        st, residue, data, stalled = self.command(struct.pack('>BBIBHB', 0x28, 0, lba, 0, count, 0),
                                                  data_in=count * BLOCK)
        assert st == 0 and residue == 0 and not stalled, (st, residue, stalled)
        assert len(data) == count * BLOCK, len(data)
        return data

    def write10(self, lba, data):
        st, residue, _, stalled = self.command(struct.pack('>BBIBHB', 0x2a, 0, lba, 0, len(data) // BLOCK, 0),
                                               data_out=data)
        assert st == 0 and residue == 0 and not stalled, (st, residue, stalled)


def mass_storage(host, info):
    return Msc(host, next(i for i in info['interfaces'] if i['class'] == 8))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--qemu', type=Path, default=REPO / 'build/qemu/build/qemu-system-arm.exe')
    parser.add_argument('--elf', type=Path, required=True)
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--app', type=Path, required=True, help='stopwatch.app for the card image')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--read-kib', type=int, default=512)
    parser.add_argument('--irq-stuck', action='store_true',
                        help='hold IRQ 25 high: exercises the polled fallback')
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    base = symbol(args.elf, 'os_status')
    usb_base = symbol(args.elf, 'usb_stat')
    home = symbol(args.elf, 'home')
    hook = symbol(args.elf, 'usb_test_hook') | 1          # Thumb
    saved = symbol(args.elf, 'usb_test_saved_tick')
    resume_result = symbol(args.elf, 'usb_app_resume_result')
    card = out / 'card.img'
    subprocess.run([sys.executable, str(REPO / 'ports/rockbox/tests/make-sd-image.py'),
                    '--fat32', '--size-mib', '64', '--seconds', '1', '--force',
                    '--runtime', str(REPO / 'sdcard/.rockbox'),
                    '--rockbox', str(REPO / 'sdcard/payloads/rockbox.bin'),
                    '--add', f'b310e-os/apps/stopwatch.app={args.app.resolve()}',
                    '--output', str(card)], check=True, capture_output=True)
    size = card.stat().st_size

    qmp_port, qtest_port, usb_port = BOOT['free_port'](), BOOT['free_port'](), BOOT['free_port']()
    command = [str(args.qemu.resolve()), '-M', 'b310e,boot-mode=rockbox',
               '-display', 'none', '-serial', 'none', '-monitor', 'none',
               '-drive', f'file={args.image.resolve().as_posix()},format=raw,if=none,id=os,readonly=on',
               '-drive', f'file={card.as_posix()},format=raw,if=none,id=sdcard',
               '-qmp', f'tcp:127.0.0.1:{qmp_port},server=on,wait=off',
               '-qtest', f'tcp:127.0.0.1:{qtest_port},server=on,wait=off',
               '-chardev', f'socket,id=usbhost,host=127.0.0.1,port={usb_port},server=on,wait=off',
               '-global', 'sc6530_usb.chardev=usbhost',
               '-global', 'sc6530_adi.charger-present=on',
               '-global', 'sc6530_adi.watchdog=on',
               '-D', str(out / 'qemu.log')]
    if args.irq_stuck:
        command += ['-global', 'sc6530_usb.irq-stuck=on']
    (out / 'command.json').write_text(json.dumps(command, indent=2))
    env = os.environ.copy()
    if os.name == 'nt':
        env['PATH'] = r'D:\Toolchains\msys64\mingw64\bin;C:\msys64\mingw64\bin;' + env.get('PATH', '')
    result = {}
    written = {}
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

            def usb():
                return {name: qt.read(usb_base + 4 * i) for i, name in enumerate(USB_FIELDS)}

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

            def open_usb():
                assert title() == 'B310E OS', title()
                qt.write(saved, qt.read(home + SCREEN_TICK))
                qt.write(home + SCREEN_TICK, hook)    # within a second, the UI calls it
                wait_title('USB storage')
                host.wait_attached(20)

            def wait_detached(seconds=15):
                end = time.monotonic() + seconds
                while host.query()['attached']:
                    if time.monotonic() > end:
                        raise UsbError('still on the bus')
                    time.sleep(0.1)

            def vbus(on):
                qt.command(f'set_irq_in /machine/peripheral/sc6530-adi charger-input 0 {int(on)}')

            wait_for(qt, base, lambda s: s['magic'] == BOOT['MAGIC'], 10, 'OS never started')
            wait_for(qt, base, lambda s: s['ui_depth'] == 1, 60, 'no UI')
            wait_title('B310E OS')
            time.sleep(1)
            t0 = time.monotonic()
            open_usb()
            result['attach_seconds'] = round(time.monotonic() - t0, 2)

            # enumeration
            info = enumerate_device(host)
            result['device'] = {k: info[k] for k in ('vid', 'pid', 'max_packet0', 'device_qualifier',
                                                     'config_value', 'max_lun', 'manufacturer',
                                                     'product', 'serial')}
            assert (info['vid'], info['pid']) == ('1209', '0001'), info
            assert info['device_qualifier'] == 'stalled' and info['max_lun'] == 0, info
            msc_if = next(i for i in info['interfaces'] if i['class'] == 8)
            assert (msc_if['subclass'], msc_if['protocol']) == (6, 0x50), msc_if
            assert sorted((e['address'], e['type'], e['mps']) for e in msc_if['endpoints']) == \
                [(0x02, 2, 64), (0x81, 2, 64)], msc_if
            assert info['serial'] and len(info['serial']) >= 12, info
            assert host.control(0x80, 8, 0, 0, 1) == b'\x01'      # GET_CONFIGURATION
            assert host.control(0x80, 0, 0, 0, 2) == b'\0\0'      # GET_STATUS
            assert usb()['state'] == USB_CONNECTED, usb()
            msc = mass_storage(host, info)

            # identification and capacity
            st, _, inquiry, _ = msc.command(b'\x12\0\0\0\x24\0', data_in=36)
            assert st == 0 and len(inquiry) == 36 and inquiry[1] & 0x80, inquiry.hex()
            result['inquiry'] = {'vendor': inquiry[8:16].decode().strip(),
                                 'product': inquiry[16:32].decode().strip()}
            msc.wait_ready(20)
            st, _, cap, _ = msc.command(b'\x25' + b'\0' * 9, data_in=8)
            last, block = struct.unpack('>II', cap)
            assert st == 0 and block == BLOCK and (last + 1) * block == size, (st, last, block, size)
            result['capacity_blocks'] = last + 1
            st, residue, fmt, _ = msc.command(b'\x23' + b'\0' * 6 + b'\x00\xfc\0', data_in=252)
            assert st == 0 and len(fmt) == 12 and residue == 240, (st, residue, fmt.hex())
            assert struct.unpack('>I', fmt[4:8])[0] == last + 1 and fmt[8] == 2, fmt.hex()
            st, residue, mode, _ = msc.command(b'\x1a\0\x3f\0\xc0\0', data_in=192)
            assert st == 0 and mode == b'\x03\0\0\0' and residue == 188, (st, residue, mode.hex())
            st, _, mode10, _ = msc.command(b'\x5a\0\x3f' + b'\0' * 4 + b'\x00\x08\0', data_in=8)
            assert st == 0 and len(mode10) == 8 and mode10[1] == 6, mode10.hex()

            # failures: the right sense; the data stage ends with a zero-length
            # packet (in) or the data is taken (out), no halt, as Rockbox does
            checks = {}
            st, residue, _, stalled = msc.command(b'\x1a\0\x08\0\xc0\0', data_in=192)
            checks['mode_page_8'] = (st, residue, stalled, msc.sense())
            assert checks['mode_page_8'] == (1, 192, None, (5, 0x24, 0)), checks
            st, residue, _, stalled = msc.command(b'\x4a\x01\0\0\x10\0\0\0\x08\0', data_in=8)
            checks['unsupported_in'] = (st, residue, stalled, msc.sense())
            assert checks['unsupported_in'] == (1, 8, None, (5, 0x20, 0)), checks
            st, residue, _, stalled = msc.command(b'\x01' + b'\0' * 5)
            checks['unsupported_none'] = (st, residue, stalled, msc.sense())
            assert checks['unsupported_none'] == (1, 0, None, (5, 0x20, 0)), checks
            st, residue, _, stalled = msc.command(b'\x55\x10' + b'\0' * 5 + b'\x00\x18\0', data_out=bytes(24))
            checks['unsupported_out'] = (st, residue, stalled, msc.sense())
            assert checks['unsupported_out'] == (1, 24, None, (5, 0x20, 0)), checks
            st, residue, _, stalled = msc.command(struct.pack('>BBIBHB', 0x28, 0, last + 1, 0, 1, 0),
                                                  data_in=BLOCK)
            checks['read_past_end'] = (st, residue, stalled, msc.sense())
            assert checks['read_past_end'] == (1, BLOCK, None, (5, 0x21, 0)), checks
            assert msc.command(b'\x1e\0\0\0\x01\0')[0] == 0       # PREVENT
            assert msc.command(b'\x35' + b'\0' * 9)[0] == 0       # SYNCHRONIZE CACHE
            assert msc.sense() == (0, 0, 0)
            result['failure_checks'] = {k: list(v[:3]) + [list(v[3])] for k, v in checks.items()}

            # reads against the image (the OS has the card suspended: stable)
            image = card.read_bytes()
            assert msc.read10(0, 1) == image[:BLOCK], 'sector 0 differs from the image'
            count = args.read_kib * 1024 // BLOCK
            t0 = time.monotonic()
            bulk = msc.read10(64, count)
            elapsed = time.monotonic() - t0
            if bulk != image[64 * BLOCK:(64 + count) * BLOCK]:
                bad = next(i for i, (x, y) in enumerate(zip(bulk, image[64 * BLOCK:])) if x != y)
                raise AssertionError(f'bulk read differs from the image at byte {bad}')
            result['read_kib_per_s'] = round(len(bulk) / 1024 / elapsed, 1)
            odd = msc.read10(1000, 37)                     # not a whole chunk
            assert odd == image[1000 * BLOCK:1037 * BLOCK], 'odd-sized read differs'

            # writes, read back
            pattern = bytes((i * 7 + 3) & 0xff for i in range(BLOCK * 2))
            msc.write10(last - 1, pattern)
            written[last - 1] = pattern
            assert msc.read10(last - 1, 2) == pattern, 'read-back after write differs'
            big = bytes(((i * 131) ^ (i >> 9)) & 0xff for i in range(200 * 1024))
            big_lba = last - 8 - len(big) // BLOCK
            t0 = time.monotonic()
            msc.write10(big_lba, big)
            elapsed = time.monotonic() - t0
            written[big_lba] = big
            result['write_kib_per_s'] = round(len(big) / 1024 / elapsed, 1)
            assert msc.read10(big_lba, len(big) // BLOCK) == big, 'read-back after the big write differs'
            s = usb()
            assert s['bytes_written'] == len(pattern) + len(big), s
            shot('usb-connected')

            # eject and load
            assert msc.command(b'\x1b\0\0\0\x02\0')[0] == 0
            assert msc.command(b'\0' * 6)[0] == 1 and msc.sense() == (2, 0x3a, 0)
            assert usb()['ejected'] == 1
            time.sleep(0.6)
            shot('usb-ejected')
            assert msc.command(b'\x1b\0\0\0\x03\0')[0] == 0
            msc.wait_ready(5)
            result['eject_load_ok'] = True

            # mass storage reset; an invalid CBW and reset recovery
            msc.reset_recovery()
            msc.wait_ready(5)
            host.bulk_out(msc.ep_out & 0xf, b'\xde\xad' * 15 + b'\0')
            try:
                host.packet_in(msc.ep_in & 0xf)
                raise AssertionError('no halt after an invalid CBW')
            except Stall:
                pass
            assert msc.halted(msc.ep_in) and msc.halted(msc.ep_out)
            msc.reset_recovery()
            msc.wait_ready(5)
            assert msc.read10(0, 1) == image[:BLOCK]
            result['reset_recovery_ok'] = True

            # host reset of the configured device: enumeration again
            info = enumerate_device(host)
            msc = mass_storage(host, info)
            msc.wait_ready(10)
            assert msc.read10(64, 8) == image[64 * BLOCK:72 * BLOCK], 'read after host reset'
            result['host_reset_ok'] = True
            s = usb()
            result['stat_connected'] = s
            assert s['bus_resets'] >= 1 and s['sd_errors'] == 0, s
            assert bool(s['polled']) == args.irq_stuck, s

            # unplug: the screen closes, the controller leaves the bus, the
            # card is mounted again
            vbus(False)
            t0 = time.monotonic()
            wait_title('B310E OS', 10)
            wait_detached()
            result['unplug_seconds'] = round(time.monotonic() - t0, 2)
            assert usb()['state'] == USB_OFF, usb()
            assert qt.read(resume_result) == 0, 'card not mounted again after unplug'

            # re-plug, then Back (RSoft)
            vbus(True)
            time.sleep(1.5)
            open_usb()
            info = enumerate_device(host)
            msc = mass_storage(host, info)
            msc.wait_ready(10)
            assert msc.read10(0, 1) == image[:BLOCK]
            assert usb()['sessions'] == 2, usb()
            qmp.hmp('sendkey f2 100')
            wait_title('B310E OS', 10)
            wait_detached()
            assert usb()['state'] == USB_OFF and qt.read(resume_result) == 0
            result['back_ok'] = True
            time.sleep(2.5)                     # the log thread writes the card
            shot('home-after')
            result['os_status'] = {k: v for k, v in status(qt, base).items()
                                   if k in ('idle_permille', 'heap_free', 'stray_irqs', 'cpu_mhz')}
            qmp.command('quit')
            process.wait(timeout=20)
        except Exception:
            if qmp and process.poll() is None:
                try:
                    shot('failure')
                    state = {name: qmp.hmp(f'xp /{n}wx {addr:#x}') for name, addr, n in [
                        ('usb', 0x20300000, 8), ('ep0', 0x20300040, 12), ('ep1', 0x203000c0, 6),
                        ('ep2', 0x20300100, 6), ('intc', 0x80000000, 4)]}
                    state['usb_stat'] = usb()
                    (out / 'failure-state.json').write_text(json.dumps(state, indent=2))
                except Exception:
                    pass
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
    for lba, data in written.items():
        assert image[lba * BLOCK:lba * BLOCK + len(data)] == data, f'write at {lba} not in the image'
    result['image_writes_verified'] = True
    log_at = image.find(b'lcd: transfer spacing')
    log_at = log_at // 512 * 512 if log_at >= 0 else -1
    end = image.find(b'\0', log_at) if log_at >= 0 else -1
    log_text = image[log_at:end].decode(errors='replace') if log_at >= 0 else ''
    (out / 'card-log.txt').write_text(log_text)
    result['card_log'] = [l for l in log_text.splitlines() if 'usb:' in l]
    for want in ('usb: session 1 ended (unplugged)', 'usb: session 2 ended (back)'):
        assert want in log_text, f'{want!r} not in the card log'
    assert log_text.count('usb: card mounted again') >= 2, result['card_log']
    (out / 'result.json').write_text(json.dumps(result, indent=2))
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
