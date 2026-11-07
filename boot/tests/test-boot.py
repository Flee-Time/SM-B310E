#!/usr/bin/env python3
"""Boot the B310E boot loader from an emulator NOR in QEMU.

The NOR is your own dump with the loader at 0x3fc000 and the boot vector
word 0x20 = 0x3fc000 (boot/mkboot.py nor); the machine starts at the
vector (boot-mode=stock). A FAT32 card holds payloads/b310e-os.bin and
payloads/rockbox.bin. Checked, each from a cold start or a watchdog-style
reset (QMP system_reset, which keeps the reset-mode byte as the phone's
watchdog reset does):

  cold, END held         the menu (the default): B310E OS, a long name on
                         two rows, Rockbox, Stock firmware
  cold, key held         the menu, the key seen
  menu -> B310E OS       the OS starts (its status block)
  reset, request 0xb1    the menu
  Auto on B310E OS       payloads/autoboot.txt written; after a reset the
                         countdown starts B310E OS; a key stops it; Auto off
  menu -> Stock firmware "Hold END"; with END held a jump into the stock
                         PBL (the LCM's power-on registers back): it runs
  reset, request 0xb2    B310E OS directly
  reset, request 0xb3    the payload named in PSRAM (Rockbox)
  reset, 0xb3 no block   the menu, "Request lost"
  reset, request 0xb0    the stock firmware
  reset, a stock value   left alone (0x7a), the menu
  B310E OS Power ->      the OS sets 0xb1 and resets by the watchdog;
  Restart to menu        the menu comes up
  default stock, no key  the stock firmware (a loader built --default stock)
  default program        payloads/rockbox.bin starts (--default program:...)
  an old card (progs/)   the menu lists progs/ and starts B310E OS from it
  no END, a charger      the stock firmware (its off-charging), no menu
  no END, no charger     switched off (a battery put in): the next END
                         is a real power-on
  END held 2 s in menu   the phone powers off (QEMU exits)
  no card                "No card" within 3 s, only Stock firmware
  autoboot.txt on a card Rockbox after a 1 s countdown
  a bad upper NOR read   (the marker at the image's end corrupted) stock

SPDX-License-Identifier: GPL-2.0-or-later
"""
import argparse
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
QMP = runpy.run_path(str(REPO / 'emulator/qemu/scripts/capture-stock.py'))['QMP']

BOOT_FIELDS = ('magic stage early request sd_code fs_code items sel pinmap '
               'loaded keys panel countdown').split()
BOOT_MAGIC = 0x53423342
STAGES = {1: 'init', 2: 'card', 3: 'menu', 4: 'load', 5: 'enter', 6: 'stock'}
OS_MAGIC = 0x534f3342
HWRST = 0x8b000228
REQ_BLOCK, REQ_MAGIC = 0x043fff00, 0x51523342
NOR_LOADER = (0x3fc000, 0x40c000)
WINDOW = (0x303fc000, 0x3040c000)          # the loader through the 0x30000000 window
LONG_NAME = 'a-very-long-payload-name-for-two-rows.bin'
CARD = runpy.run_path(str(Path(__file__).resolve().parents[2] / 'os/tests/test-boot.py'))['card_file']


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


class Machine:
    def __init__(self, args, out, nor, card, end=True, charger=False):
        self.started = time.monotonic()
        self.out = out
        qmp_port, qtest_port = free_port(), free_port()
        # boot-mode=stock holds END for 2 s after each reset (hold-end=on):
        # a power-on with END; hold-end=off is one by the charger
        command = [str(args.qemu.resolve()), '-M',
                   'b310e,boot-mode=stock' + ('' if end else ',hold-end=off'), '-S',
                   '-display', 'none', '-serial', 'none', '-monitor', 'none',
                   '-drive', f'file={nor.as_posix()},format=raw,if=none,id=nor,readonly=on',
                   *(['-drive', f'file={card.as_posix()},format=raw,if=none,id=sdcard'] if card else []),
                   '-qmp', f'tcp:127.0.0.1:{qmp_port},server=on,wait=off',
                   '-qtest', f'tcp:127.0.0.1:{qtest_port},server=on,wait=off',
                   '-global', 'sc6530_adi.watchdog=on',
                   '-D', str(out / 'qemu.log')]
        if charger:
            command[-2:-2] = ['-global', 'sc6530_adi.charger-present=on']
        env = os.environ.copy()
        if os.name == 'nt':
            env['PATH'] = r'D:\Toolchains\msys64\mingw64\bin;C:\msys64\mingw64\bin;' + env.get('PATH', '')
        self.log = (out / 'stderr.log').open('wb')
        self.process = subprocess.Popen(command, env=env, stdout=self.log, stderr=self.log)
        self.qmp = self.qt = None
        for _ in range(100):
            try:
                self.qmp = QMP(qmp_port)
                self.qt = QT(qtest_port)
                break
            except OSError:
                time.sleep(0.1)
        assert self.qmp and self.qt, 'QEMU did not start'

    def close(self):
        for c in (self.qt, self.qmp):
            try:
                c and c.close()
            except OSError:
                pass
        self.process.kill()
        self.process.wait()
        self.log.close()

    def shot(self, name):
        self.qmp.command('screendump', {'filename': str(self.out / f'{name}.png'), 'format': 'png'})

    def pc(self):
        text = self.qmp.hmp('info registers')
        return int(re.search(r'R15=([0-9a-f]{8})', text).group(1), 16)


def boot_status(m, base):
    s = {name: m.qt.read(base + 4 * i) for i, name in enumerate(BOOT_FIELDS)}
    raw = b''.join(m.qt.read(base + 4 * len(BOOT_FIELDS) + 4 * i).to_bytes(4, 'little')
                   for i in range(8))
    s['message'] = raw.split(b'\0')[0].decode(errors='replace')
    s['stage'] = STAGES.get(s['stage'], s['stage'])
    return s


def wait(what, test, seconds):
    end = time.monotonic() + seconds
    last = None
    while time.monotonic() < end:
        last = test()
        if last:
            return last
        time.sleep(0.1)
    raise AssertionError(f'{what} (last: {last})')


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--qemu', type=Path, default=REPO / 'build/qemu/build/qemu-system-arm.exe')
    parser.add_argument('--dump', type=Path, default=REPO / 'tools/spd_dump/full-backup.bin',
                        help='your own 8 MB NOR dump (stays local)')
    parser.add_argument('--loader', type=Path, default=REPO / 'build/boot/b310e-boot.bin')
    parser.add_argument('--elf', type=Path, default=REPO / 'build/boot/b310e-boot.elf')
    parser.add_argument('--os', type=Path, default=REPO / 'build/os/b310e-os.bin')
    parser.add_argument('--os-elf', type=Path, default=REPO / 'build/os/b310e-os.elf')
    parser.add_argument('--rockbox', type=Path, default=REPO / 'sdcard/payloads/rockbox.bin')
    parser.add_argument('--runtime', type=Path, default=REPO / 'sdcard/.rockbox')
    parser.add_argument('--output', type=Path, default=REPO / 'build/boot/test')
    parser.add_argument('--skip-os-restart', action='store_true',
                        help='skip the OS Power menu case (an OS without it)')
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    base = symbol(args.elf, 'boot_status')
    os_base = symbol(args.os_elf, 'os_status')

    nor = out / 'nor-test.bin'
    subprocess.run([sys.executable, str(REPO / 'boot/mkboot.py'), 'nor', '--dump', str(args.dump),
                    '--loader', str(args.loader), '--output', str(nor)], check=True)
    card = out / 'card.img'
    subprocess.run([sys.executable, str(REPO / 'ports/rockbox/tests/make-sd-image.py'),
                    '--fat32', '--size-mib', '64', '--seconds', '1', '--force',
                    '--runtime', str(args.runtime), '--rockbox', str(args.rockbox),
                    '--add', f'payloads/b310e-os.bin={args.os.resolve()}',
                    '--add', f'payloads/{LONG_NAME}={args.os.resolve()}',
                    '--output', str(card)], check=True, capture_output=True)
    auto_txt = out / 'autoboot.txt'
    auto_txt.write_text('# test\nentry = rockbox.bin\ntimeout = 1\n')
    auto_card = out / 'card-auto.img'
    subprocess.run([sys.executable, str(REPO / 'ports/rockbox/tests/make-sd-image.py'),
                    '--fat32', '--size-mib', '64', '--seconds', '1', '--force',
                    '--runtime', str(args.runtime), '--rockbox', str(args.rockbox),
                    '--add', f'payloads/b310e-os.bin={args.os.resolve()}',
                    '--add', f'payloads/autoboot.txt={auto_txt}',
                    '--output', str(auto_card)], check=True, capture_output=True)
    old_card = out / 'card-progs.img'
    subprocess.run([sys.executable, str(REPO / 'ports/rockbox/tests/make-sd-image.py'),
                    '--fat32', '--size-mib', '64', '--seconds', '1', '--force',
                    '--runtime', str(args.runtime), '--rockbox', str(args.rockbox),
                    '--payload-dir', 'progs',
                    '--add', f'progs/b310e-os.bin={args.os.resolve()}',
                    '--output', str(old_card)], check=True, capture_output=True)

    results = {}

    def case(name, fn):
        print(f'--- {name}', flush=True)
        r = fn()
        results[name] = r
        print(json.dumps(r), flush=True)

    m = Machine(args, out, nor, card)
    try:
        st = lambda: boot_status(m, base)

        def reset_with(byte, block=None):
            m.qmp.command('stop')
            m.qt.memory(base, bytes(80))             # forget the last status
            m.qt.write(os_base, 0)
            if block is not None:
                m.qt.memory(REQ_BLOCK, block)
            m.qt.write(HWRST, (m.qt.read(HWRST) & ~0xff00) | byte << 8)
            m.qmp.command('system_reset')
            m.qmp.command('cont')

        def in_menu():
            s = st()
            return s if s['magic'] == BOOT_MAGIC and s['stage'] == 'menu' else None

        def os_running():
            return m.qt.read(os_base) == OS_MAGIC

        def stock_running():
            s = st()
            pc = m.pc()
            return {'pc': hex(pc)} if s['magic'] != BOOT_MAGIC and not (
                NOR_LOADER[0] <= pc < NOR_LOADER[1] or WINDOW[0] <= pc < WINDOW[1]) \
                and pc != 0 else None

        # 1. cold start, no key: the menu (the default)
        def cold_no_key():
            m.qmp.command('cont')
            s = wait('no menu', in_menu, 15)
            assert not s['early'] & 1 and s['early'] & 4, s       # END, no key
            assert s['request'] == 0 and s['items'] == 4, s
            assert s['sd_code'] == 0 and s['fs_code'] == 0 and s['pinmap'] > 100, s
            time.sleep(0.5)
            m.shot('1-menu')
            return s
        case('cold, no key: menu', cold_no_key)

        # 2. cold start with a key held: the menu
        def cold_key():
            m.qmp.hmp('sendkey 5 4000')              # held through the reset
            m.qmp.command('stop')
            m.qt.memory(base, bytes(80))
            m.qt.write(HWRST, m.qt.read(HWRST) & ~0xff00)
            m.qmp.command('system_reset')
            m.qmp.command('cont')
            s = wait('no menu', in_menu, 15)
            assert s['early'] & 1, s
            assert s['items'] == 4, s
            assert s['sd_code'] == 0 and s['fs_code'] == 0, s
            assert s['pinmap'] > 100, s
            time.sleep(4.5)                          # the key released
            m.shot('2-menu')
            return s
        case('cold, key held: menu', cold_key)

        # 3. the menu starts B310E OS (first entry)
        def menu_os():
            m.qmp.hmp('sendkey ret 100')
            wait('B310E OS did not start', os_running, 20)
            s = st()
            assert s['stage'] == 'enter' and s['loaded'] == args.os.stat().st_size, s
            time.sleep(3)
            m.shot('3-os')
            return {'loaded': s['loaded']}
        case('menu -> B310E OS', menu_os)

        # 4. request 0xb1 across a reset: the menu, no key
        def req_menu():
            reset_with(0xb1)
            s = wait('no menu after 0xb1', in_menu, 15)
            assert s['request'] == 0xb1 and not s['early'] & 1, s
            assert m.qt.read(HWRST) >> 8 & 0xff == 0, 'request not cleared'
            m.shot('4-menu-request')
            return s
        case('reset 0xb1: menu', req_menu)

        # 4a. Auto on B310E OS (selected): the file; the countdown; a key stops it
        def auto_toggle():
            time.sleep(0.5)
            m.qmp.hmp('sendkey f1 100')              # LSOFT: Auto
            s = wait('no "Autoboot on"', lambda: (lambda s: s if s['message'] == 'Autoboot on' else None)(st()), 5)
            time.sleep(0.5)
            m.shot('4a-auto-on')
            reset_with(0)                            # a power-on with END
            s = wait('no countdown', lambda: (lambda s: s if s['stage'] == 'menu' and 0 < s['countdown'] < 100 else None)(st()), 15)
            m.shot('4a-countdown')
            wait('B310E OS not autobooted', os_running, 20)
            reset_with(0)
            wait('no countdown', lambda: (lambda s: s if s['stage'] == 'menu' and 0 < s['countdown'] < 100 else None)(st()), 15)
            m.qmp.hmp('sendkey down 100')            # stops it
            s = wait('countdown not stopped', lambda: (lambda s: s if s['countdown'] == 0xffffffff else None)(st()), 5)
            assert s['stage'] == 'menu', s
            time.sleep(0.5)                          # still on B310E OS
            m.qmp.hmp('sendkey f1 100')              # Auto off on B310E OS
            wait('no "Autoboot off"', lambda: (lambda s: s if s['message'] == 'Autoboot off' else None)(st()), 5)
            return {'countdown_stopped': True}
        case('Auto on B310E OS, countdown, stop, off', auto_toggle)

        # 4b. the menu's "Stock firmware": hold END, a jump (no reset)
        def menu_stock():
            def resets():
                return (out / 'qemu.log').read_text(errors='replace').count('b310e: reset')
            time.sleep(0.5)
            m.qmp.hmp('sendkey up 100')              # wraps to the last entry
            wait('not on Stock firmware', lambda: st()['sel'] == st()['items'] - 1, 5)
            m.shot('4b-menu-on-stock')
            before = resets()
            m.qmp.hmp('sendkey ret 100')             # "Hold END"
            wait('no stock screen', lambda: st()['stage'] == 'stock', 5)
            time.sleep(0.8)
            m.shot('4b-stock-screen')
            m.qmp.hmp('sendkey end 2500')            # held: the jump
            wait('loader still there', lambda: st()['stage'] == 'stock', 5)
            m.qt.memory(base, bytes(80))             # the loader's status in IRAM
            time.sleep(4)
            r = wait('stock not running after the menu', stock_running, 15)
            assert resets() == before, 'the machine was reset'
            time.sleep(8)
            m.shot('4b-stock-jump')
            r['pc_later'] = hex(m.pc())
            return r
        case('menu -> Stock firmware', menu_stock)
        reset_with(0)

        # 5. 0xb2: B310E OS directly
        def req_os():
            reset_with(0xb2)
            wait('B310E OS did not start after 0xb2', os_running, 20)
            s = st()
            assert s['request'] == 0xb2 and s['stage'] == 'enter', s
            return {'request': hex(s['request']), 'loaded': s['loaded']}
        case('reset 0xb2: B310E OS', req_os)

        # 6. 0xb3 with a PSRAM block naming Rockbox
        def req_path():
            path = b'payloads/rockbox.bin'.ljust(56, b'\0')
            words = [int.from_bytes(path[i:i + 4], 'little') for i in range(0, 56, 4)]
            check = ~(REQ_MAGIC + sum(words)) & 0xffffffff
            block = REQ_MAGIC.to_bytes(4, 'little') + path + check.to_bytes(4, 'little')
            reset_with(0xb3, block)
            s = wait('Rockbox not entered', lambda: (lambda s: s if s['stage'] == 'enter' else None)(st()), 20)
            assert s['request'] == 0xb3 and s['loaded'] == args.rockbox.stat().st_size, s
            time.sleep(4)
            pc = m.pc()
            assert 0x04000000 <= pc < 0x04400000 or pc >= 0xffff0000, hex(pc)
            assert m.qt.read(REQ_BLOCK) != REQ_MAGIC, 'block not consumed'
            m.shot('6-rockbox')
            return {'loaded': s['loaded'], 'pc': hex(pc)}
        case('reset 0xb3: named program (Rockbox)', req_path)

        # 7. 0xb3 without a block: the menu says so
        def req_path_lost():
            reset_with(0xb3, bytes(64))
            s = wait('no menu after a lost 0xb3', in_menu, 15)
            assert s['message'] == 'Request lost', s
            return s
        case('reset 0xb3, no block: menu', req_path_lost)

        # 8. 0xb0: the stock firmware once
        def req_stock():
            reset_with(0xb0)
            time.sleep(3)
            r = wait('stock not running after 0xb0', stock_running, 10)
            r['hwrst_now'] = hex(m.qt.read(HWRST) >> 8 & 0xff)      # 0x8a: the stock's own
            assert r['hwrst_now'] != '0xb0', 'request not cleared'
            return r
        case('reset 0xb0: stock', req_stock)

        # 9. a stock reset reason is left for the stock firmware
        def stock_value():
            reset_with(0x7a)                    # a PBL value the main OS does not write
            s = wait('no menu after 0x7a', in_menu, 15)
            got = m.qt.read(HWRST) >> 8 & 0xff
            assert s['request'] == 0 and got == 0x7a, (s, hex(got))
            return {'hwrst_after_loader': hex(got)}
        case('reset 0x7a: left alone', stock_value)

        # 10. the OS's own request: Power -> Restart to menu
        if not args.skip_os_restart:
            def os_restart():
                reset_with(0xb2)
                t0 = time.monotonic()
                wait('B310E OS did not start', os_running, 20)
                wait('no OS UI', lambda: m.qt.read(os_base + 4 * 21) == 1, 30)   # ui_depth
                # boot-mode=stock holds END for 2 s after a reset (the power
                # key at power-on); the OS takes END as "home"
                time.sleep(max(0.0, 3.5 - (time.monotonic() - t0)))

                def title():
                    addr = m.qt.read(os_base + 4 * 22)                       # ui_title
                    out = b''
                    while addr and len(out) < 32:
                        word = m.qt.read(addr & ~3).to_bytes(4, 'little')[addr & 3:]
                        if b'\0' in word:
                            return (out + word.split(b'\0')[0]).decode(errors='replace')
                        out += word
                        addr = (addr & ~3) + 4
                    return out.decode(errors='replace')

                def press(key, want):
                    m.qmp.hmp(f'sendkey {key} 100')
                    try:
                        wait(f'{key}: not {want!r}', lambda: title() == want, 6)
                    except AssertionError:
                        m.shot('10-lost')
                        raise AssertionError(f'{key}: screen {title()!r}, wanted {want!r}')
                    time.sleep(0.4)

                press('ret', 'Menu')
                for _ in range(5):                                        # Power
                    press('down', 'Menu')
                press('ret', 'Power')
                press('down', 'Power')                                    # Restart to menu
                press('ret', 'Restart')
                m.shot('10-os-confirm')
                m.qt.memory(base, bytes(80))
                m.qmp.hmp('sendkey ret 100')                              # Yes
                s = wait('no menu after the OS restart', in_menu, 30)
                assert s['request'] == 0xb1 and not s['early'] & 1, s
                m.shot('10-menu-from-os')
                return s
            case('OS Power -> Restart to menu', os_restart)
    finally:
        m.close()

    # The written autoboot.txt (the card image, after QEMU closed it).
    text = CARD(card.read_bytes(), '/payloads/autoboot.txt')
    assert text and b'entry = \n' in text and b'timeout = 3' in text, text
    results['autoboot.txt after Auto off'] = text.decode()

    # The header's default with no key and no request: stock, then a
    # program; then the menu with a card from before the rename; a power-on
    # by the charger (no END); autoboot from the card; a bad NOR window.
    tail = symbol(args.elf, 'boot_tail')         # its NOR offset (linked in the 0x0 window)
    for default, check in (('stock', 'stock'), ('program:payloads/rockbox.bin', 'program'),
                           ('menu', 'old card'), ('menu', 'charger'), ('menu', 'battery'),
                           ('menu', 'autoboot'), ('menu', 'bad window'), ('menu', 'power off'),
                           ('menu', 'no card')):
        loader = out / f'loader-{check}.bin'
        raw = args.loader.read_bytes()
        loader.write_bytes(raw)
        subprocess.run([sys.executable, str(REPO / 'boot/mkboot.py'), 'image', str(loader),
                        '--default', default, '--output', str(loader)], check=True)
        nor_d = out / f'nor-{check.replace(" ", "-")}.bin'
        subprocess.run([sys.executable, str(REPO / 'boot/mkboot.py'), 'nor', '--dump', str(args.dump),
                        '--loader', str(loader), '--output', str(nor_d)], check=True)
        if check == 'bad window':               # the marker reads wrong
            data = bytearray(nor_d.read_bytes())
            data[tail] ^= 0xff
            nor_d.write_bytes(data)
        m = Machine(args, out, nor_d,
                    None if check == 'no card' else
                    old_card if check == 'old card' else auto_card if check == 'autoboot' else card,
                    end=check not in ('charger', 'battery'), charger=check == 'charger')
        try:
            st = lambda: boot_status(m, base)

            def cold_default():
                m.qmp.command('cont')
                if check in ('stock', 'charger', 'bad window'):
                    time.sleep(3)
                    pc = m.pc()
                    assert st()['magic'] != BOOT_MAGIC, hex(pc)
                    assert not NOR_LOADER[0] <= pc < NOR_LOADER[1], hex(pc)
                    assert not WINDOW[0] <= pc < WINDOW[1], hex(pc)
                    return {'pc': hex(pc)}
                if check == 'no card':
                    t0 = time.monotonic()
                    s = wait('no menu', lambda: (lambda s: s if s['stage'] == 'menu' else None)(st()), 15)
                    took = time.monotonic() - t0
                    assert s['message'] == 'No card' and s['items'] == 1, s
                    assert took < 6, took
                    m.shot('no-card')
                    return {'menu_after_s': round(took, 1), 'message': s['message']}
                if check == 'battery':
                    try:
                        m.process.wait(timeout=15)
                    except subprocess.TimeoutExpired:
                        raise AssertionError('not switched off without END')
                    return {'powered_off': True}
                if check == 'power off':
                    wait('no menu', lambda: (lambda s: s if s['stage'] == 'menu' else None)(st()), 15)
                    time.sleep(2.5)                  # the power-on END (2 s) let go
                    m.qmp.hmp('sendkey end 3500')
                    try:
                        m.process.wait(timeout=15)
                    except subprocess.TimeoutExpired:
                        raise AssertionError('no power-off after END held')
                    return {'powered_off': True}
                if check == 'autoboot':
                    s = wait('no autoboot', lambda: (lambda s: s if s['stage'] == 'enter' else None)(st()), 20)
                    assert s['loaded'] == args.rockbox.stat().st_size, s
                    return {'loaded': s['loaded']}
                if check == 'old card':
                    s = wait('no menu', lambda: (lambda s: s if s['stage'] == 'menu' else None)(st()), 15)
                    assert s['items'] == 3, s
                    time.sleep(0.5)
                    m.qmp.hmp('sendkey ret 100')
                    wait('B310E OS from progs/', lambda: m.qt.read(os_base) == OS_MAGIC, 20)
                    return s
                s = wait('no program by default', lambda: (lambda s: s if s['stage'] == 'enter' else None)(st()), 20)
                assert s['loaded'] == args.rockbox.stat().st_size, s
                return {'loaded': s['loaded']}
            case(f'cold, no key, default {default}' + ('' if check in ('stock', 'program') else f' ({check})'),
                 cold_default)
        finally:
            m.close()
    (out / 'results.json').write_text(json.dumps(results, indent=1))
    print(f'PASS: {len(results)} cases; screens and logs in {out}')


if __name__ == '__main__':
    main()
