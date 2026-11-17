#!/usr/bin/env python3
"""The data store (os/fs/db.c, /b310e-os/data) in QEMU, through the NET shell's
"db" command (os/net/shell.c) and a test app on the app API (os/tests/dbtest.c).

The card starts with the files the store replaces: settings.txt (the USB
network at the plug, brightness 40), alarms.txt with a Turkish label and
volumes.txt. Three boots, the USB network up in the background each time and
this script as the PC (test-net.py's RNDIS host and TCP):

  1. the old files are in the store, once (import.* set); settings round trip
     with Turkish (ğüşıöç İ), German (äöüß) and Cyrillic text, read back
     byte-exact; 50 sets in a row make at most 3 card writes; the test app
     fills a table of 300 records and queries it (newest first, the index
     field, a string and a bool outside the index, update, remove: 270
     left), keys of its own only, bad UTF-8 refused; a compaction makes the
     file smaller and keeps everything; the table opened at version 2 (a
     field added) runs its upgrade hook once; export to text, the text
     changed on the PC, imported again (the UI's brightness follows the
     store at once), a wrong import changes nothing; power-off;
  2. everything is still there after the restart; power-off;
  3. the file cut in the middle of its last record: the boot logs it, keeps
     the damaged file as data-bad, writes a clean one, and everything before
     the last record is intact.

  make PYTHON=python ../build/os/b310e-os.bin ../build/os/tests/dbtest.app   (from os/)
  python os/tests/test-db.py

SPDX-License-Identifier: GPL-2.0-or-later
"""
import argparse
import json
import os
from pathlib import Path
import re
import runpy
import struct
import subprocess
import sys
import time
import zlib

REPO = Path(__file__).resolve().parents[2]
BOOT = runpy.run_path(str(REPO / 'os/tests/test-boot.py'))
NET = runpy.run_path(str(REPO / 'os/tests/test-net.py'))
DB = runpy.run_path(str(REPO / 'os/tools/b310e-db.py'))
symbol, status, string, wait_for, card_file = (
    BOOT[n] for n in ('symbol', 'status', 'string', 'wait_for', 'card_file'))
Rndis, Tcp, Host = NET['Rndis'], NET['Tcp'], NET['Host']
PROMPT, PHONE_IP, RAW_PORT = NET['PROMPT'], NET['PHONE_IP'], NET['RAW_PORT']
MENU = BOOT['MENU']
TEXTS = {'tr': 'ğüşıöç İ Çağrı', 'de': 'äöüß Grüße', 'ru': 'Привет, мир ёЁ'}
APP = '"app/DB test/'


def dir_entry(image, path):
    """The image offset of a file's 32-byte FAT32 directory entry (8.3 or long
    name), or None: to cut the file as a power cut would."""
    part = 0
    if image[510:512] == b'\x55\xaa' and image[0x1be + 4] in (0x0b, 0x0c):
        part = int.from_bytes(image[0x1be + 8:0x1be + 12], 'little') * 512
    bs = image[part:part + 512]
    bps, spc = int.from_bytes(bs[11:13], 'little'), bs[13]
    fat = part + int.from_bytes(bs[14:16], 'little') * bps
    data = fat + bs[16] * int.from_bytes(bs[36:40], 'little') * bps
    csize = spc * bps

    def clusters(c):
        while 2 <= c < 0x0ffffff8:
            yield data + (c - 2) * csize
            c = int.from_bytes(image[fat + 4 * c:fat + 4 * c + 4], 'little') & 0x0fffffff

    c, parts = int.from_bytes(bs[44:48], 'little'), [p for p in path.split('/') if p]
    for i, want in enumerate(parts):
        hit, lfn = None, {}
        for at in clusters(c):
            for off in range(at, at + csize, 32):
                e = image[off:off + 32]
                if e[0] == 0:
                    break
                if e[0] == 0xe5:
                    lfn = {}
                    continue
                if e[11] == 0x0f:
                    lfn[e[0] & 0x1f] = (e[1:11] + e[14:26] + e[28:32]).decode('utf-16le', 'replace')
                    continue
                short = e[0:8].decode('latin-1').rstrip()
                ext = e[8:11].decode('latin-1').rstrip()
                names = {short + ('.' + ext if ext else '')}
                if lfn:
                    names.add(''.join(lfn[k] for k in sorted(lfn)).split('\0')[0])
                lfn = {}
                if want.lower() in (n.lower() for n in names):
                    hit = off
                    break
            if hit:
                break
        if hit is None:
            return None
        if i == len(parts) - 1:
            return hit
        c = int.from_bytes(image[hit + 20:hit + 22], 'little') << 16 | \
            int.from_bytes(image[hit + 26:hit + 28], 'little')
    return None


class Phone:
    def __init__(self, args, card, out, name):
        build = args.build.resolve()
        self.elf, image = build / 'b310e-os.elf', build / 'b310e-os.bin'
        self.base = symbol(self.elf, 'os_status')
        self.settings = symbol(self.elf, 'ui_settings')
        qmp_port, qtest_port, usb_port = (BOOT['free_port']() for _ in range(3))
        cmd = [str(args.qemu.resolve()), '-M', 'b310e,boot-mode=rockbox',
               '-display', os.environ.get('B310E_QEMU_DISPLAY', 'none'), '-serial', 'none',
               '-monitor', 'none', '-audiodev', 'none,id=audio0', '-global', 'sc6530_adi.audiodev=audio0',
               '-drive', f'file={image.as_posix()},format=raw,if=none,id=os,readonly=on',
               '-drive', f'file={card.as_posix()},format=raw,if=none,id=sdcard',
               '-qmp', f'tcp:127.0.0.1:{qmp_port},server=on,wait=off',
               '-qtest', f'tcp:127.0.0.1:{qtest_port},server=on,wait=off',
               '-chardev', f'socket,id=usbhost,host=127.0.0.1,port={usb_port},server=on,wait=off',
               '-global', 'sc6530_usb.chardev=usbhost',
               '-global', 'sc6530_adi.charger-present=on',
               '-D', str(out / f'qemu-{name}.log')]
        env = os.environ.copy()
        if os.name == 'nt':
            env['PATH'] = r'D:\Toolchains\msys64\mingw64\bin;C:\msys64\mingw64\bin;' + env.get('PATH', '')
        self.log = (out / f'stderr-{name}.log').open('wb')
        self.p = subprocess.Popen(cmd, env=env, stdout=self.log, stderr=self.log)
        self.qmp = self.qt = None
        for _ in range(100):
            try:
                self.qmp, self.qt = BOOT['capture'].QMP(qmp_port), BOOT['QT'](qtest_port)
                break
            except OSError:
                time.sleep(0.1)
        assert self.qmp and self.qt, 'QEMU did not start'
        wait_for(self.qt, self.base, lambda s: s['magic'] == BOOT['MAGIC'], 10, 'OS never started')
        wait_for(self.qt, self.base, lambda s: s['ui_depth'] == 1, 60, 'no UI')
        # the cable at boot: the network by itself (settings: usb_default=2)
        host = Host(usb_port)
        host.wait_attached(30)
        net = Rndis(host)
        net.enumerate()
        net.command(2, struct.pack('<III', 1, 0, 0x4000))
        net.mac = net.query(0x01010101)[1]
        net.set(0x0001010e, struct.pack('<I', 0x0b))
        net.dhcp()
        net.arp(PHONE_IP)
        self.sh = Tcp(net, RAW_PORT)
        self.sh.connect()
        self.sh.read_until(PROMPT)

    def cmd(self, line, seconds=30):
        return self.sh.cmd(line, seconds).strip()

    def writes(self):
        m = re.search(r'(\d+) card writes', self.cmd('db list'))
        return int(m.group(1))

    def stats(self):
        m = re.search(r': (\d+) bytes, (\d+) of them old', self.cmd('db list'))
        return int(m.group(1)), int(m.group(2))

    def app(self, what, seconds=180):
        """the test app, until its result is set"""
        self.cmd(f'run dbtest.app {what}')
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            r = self.cmd(f'db get {APP}result"')
            if not r.startswith('ERR'):
                self.cmd('close')
                return r.split(' str ', 1)[1].strip('"')
            time.sleep(1)
        raise AssertionError(f'the test app gave no result for {what}')

    def put(self, path, data):
        self.sh.send(f'put {path} {len(data)}\n'.encode())
        assert self.sh.read_until(b'\n') == b'READY\n'
        self.sh.send(data)
        ok = self.sh.read_until(PROMPT)[:-len(PROMPT)].decode().strip()
        assert ok == f'OK {len(data)} {zlib.crc32(data):08x}', ok

    def get(self, path):
        self.sh.send(f'get {path}\n'.encode())
        head = self.sh.read_until(b'\n')
        assert head.startswith(b'DATA '), head
        data = self.sh.read_exact(int(head.split()[1]))
        self.sh.read_until(PROMPT)
        return data

    def title(self):
        return string(self.qt, status(self.qt, self.base)['ui_title'])

    def press(self, k, want=None):
        self.qmp.hmp(f'sendkey {k} {200 if k == "esc" else 100}')
        if want:
            wait_for(self.qt, self.base, lambda s: string(self.qt, s['ui_title']) == want, 8,
                     f'not {want}')
        time.sleep(0.4)

    def power_off(self):
        self.press('esc')
        self.press('ret', 'Menu')
        for _ in range(MENU.index('Power')):
            self.press('down')
        self.press('ret', 'Power')
        self.press('ret', 'Power off')
        self.qmp.hmp('sendkey f1 100')
        self.p.wait(timeout=30)

    def close(self):
        for c in (self.qt, self.qmp):
            if c:
                c.close()
        if self.p.poll() is None:
            self.p.terminate()
            self.p.wait()
        self.log.close()


def get_value(ph, key):
    """the value "db get" shows: (type, text)"""
    r = ph.cmd(f'db get "{key}"')
    assert not r.startswith('ERR'), r
    name, rest = r.split(' ', 1) if not key.startswith('app/') else (key, r[len(key) + 1:])
    t, v = rest.split(' ', 1)
    return t, v


def store_of(card):
    data = card_file(card.read_bytes(), '/b310e-os/data')
    assert data, 'no /b310e-os/data on the card'
    return data, DB['parse'](data)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--qemu', type=Path, default=REPO / 'build/qemu/build/qemu-system-arm.exe')
    parser.add_argument('--build', type=Path, default=REPO / 'build/os')
    args = parser.parse_args()
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')     # the results are UTF-8
    out = (args.build / 'test-db').resolve()
    out.mkdir(parents=True, exist_ok=True)
    app = args.build / 'tests/dbtest.app'
    if not app.is_file():
        sys.exit('test-db: no dbtest.app (make PYTHON=python ../build/os/tests/dbtest.app in os/)')
    (out / 'settings.txt').write_bytes(b'usb_default=2\nbrightness=40\n')
    (out / 'alarms.txt').write_bytes('1 450 62 Uyanış\n'.encode())
    (out / 'volumes.txt').write_bytes(b'5 - Eski oyun\n')
    card = out / 'card.img'
    subprocess.run([sys.executable, str(REPO / 'ports/rockbox/tests/make-sd-image.py'),
                    '--fat32', '--size-mib', '64', '--seconds', '1', '--force',
                    '--runtime', str(REPO / 'sdcard/.rockbox'),
                    '--rockbox', str(REPO / 'sdcard/payloads/rockbox.bin'),
                    '--add', f'b310e-os/settings.txt={out / "settings.txt"}',
                    '--add', f'b310e-os/alarms.txt={out / "alarms.txt"}',
                    '--add', f'b310e-os/volumes.txt={out / "volumes.txt"}',
                    '--add', f'b310e-os/apps/dbtest.app={app.resolve()}',
                    '--output', str(card)], check=True, capture_output=True)
    result = {}

    # ---- boot 1
    ph = Phone(args, card, out, 'first')
    try:
        # the old files, imported once
        assert get_value(ph, 'ui.brightness') == ('int', '40'), get_value(ph, 'ui.brightness')
        assert get_value(ph, 'ui.usb_default') == ('int', '2')
        for k in ('import.settings', 'import.alarms', 'import.volumes', 'import.clock'):
            assert get_value(ph, k) == ('bool', 'true'), k
        alarms = ph.cmd('db dump alarms')
        assert 'minute=450 days=62 label="Uyanış"' in alarms, alarms
        volumes = ph.cmd('db dump volumes')
        assert 'name="Eski oyun" level=5' in volumes, volumes
        result['imported'] = {'alarms': alarms.splitlines()[1:], 'volumes': volumes.splitlines()[1:]}

        # settings: other scripts, byte for byte
        for k, text in TEXTS.items():
            r = ph.cmd(f'db set test.{k} "{text}" str')
            assert r == f'test.{k} str "{text}"', r
        for k, text in TEXTS.items():
            assert get_value(ph, f'test.{k}') == ('str', f'"{text}"'), (k, get_value(ph, f'test.{k}'))
        assert ph.cmd('db set test.n 12') == 'test.n int 12'
        assert ph.cmd('db set test.flag true bool') == 'test.flag bool true'

        # batching: a slider's 50 steps in a row, a few card writes
        time.sleep(3)
        w0 = ph.writes()
        ph.sh.send(b''.join(f'db set test.slider {i}\n'.encode() for i in range(50)))
        for _ in range(50):
            ph.sh.read_until(PROMPT)
        time.sleep(3.5)
        w1 = ph.writes()
        result['writes_for_50_sets'] = w1 - w0
        assert w1 - w0 <= 3, result
        assert get_value(ph, 'test.slider') == ('int', '49')

        # tables through the app API
        result['fill'] = ph.app('fill')
        assert result['fill'] == 'ok fill', result
        listing = ph.cmd('db list')
        assert re.search(r'app/DB test/items\s+270 records', listing), listing
        dump = ph.cmd(f'db dump {APP}items" 3').splitlines()
        # id = n + 1; 300 (n 299, group 9) was removed
        assert dump[1].split()[0] == '299' and dump[2].split()[0] == '298', dump
        assert 'name="Çağrı ğüşıöç İ 297"' in dump[2] and 'group=7' in dump[2], dump
        assert ph.cmd('db get "app/DB test/hello"') == 'app/DB test/hello str "Merhaba dünya"'

        # compaction: smaller, the same contents
        size0, dead0 = ph.stats()
        r = ph.cmd('db compact')
        size1, dead1 = ph.stats()
        result['compaction'] = {'before': [size0, dead0], 'after': [size1, dead1]}
        assert r.startswith(DB_PATH_SHOWN) and dead1 == 0 and size1 < size0, (r, result)
        assert ph.app('verify') == 'ok verify'

        # a field added: version 2, the hook once
        assert ph.app('upgrade') == 'ok upgrade'
        assert ph.app('verify') == 'ok verify'
        layout = ph.cmd(f'db dump {APP}items" 1').splitlines()[0]
        assert 'version 2' in layout and 'note' in layout, layout

        # export, change on the PC, import
        assert ph.cmd('db export').startswith('OK written to /b310e-os/data.txt')
        text = ph.get('/b310e-os/data.txt').decode('utf-8')
        (out / 'export.txt').write_text(text, encoding='utf-8', newline='\n')
        assert f'key "test.tr" str "{TEXTS["tr"]}"' in text
        assert len([l for l in text.splitlines() if l.startswith(f'rec {APP}items"')]) == 270
        assert 'key "ui.brightness" int 40' in text
        changed = text.replace('key "ui.brightness" int 40', 'key "ui.brightness" int 70')
        changed += 'key "test.imported" str "içeri alındı"\n'
        ph.put('/import.txt', changed.encode('utf-8'))
        r = ph.cmd('db import /import.txt')
        assert r == 'OK read from /import.txt', r
        assert get_value(ph, 'ui.brightness') == ('int', '70')
        assert get_value(ph, 'test.imported') == ('str', '"içeri alındı"')
        time.sleep(1)
        result['ui_brightness_after_import'] = ph.qt.read(ph.settings)
        assert result['ui_brightness_after_import'] == 70, 'the UI did not take up the store'
        assert ph.app('verify') == 'ok verify'
        assert re.search(r'app/DB test/items\s+270 records', ph.cmd('db list'))
        ph.put('/bad.txt', (changed + 'key "x" int notanumber\n').encode('utf-8'))
        r = ph.cmd('db import /bad.txt')
        assert r.startswith('ERR') and 'nothing changed' in r, r
        assert get_value(ph, 'test.imported') == ('str', '"içeri alındı"')
        assert ph.cmd('db del test.n') == 'OK'
        assert ph.cmd('db get test.n').startswith('ERR')
        ph.power_off()
    except Exception:
        print(json.dumps(result, indent=2, ensure_ascii=False))
        raise
    finally:
        ph.close()

    data, s = store_of(card)
    assert s.error is None, s.error
    assert s.keys['test.ru'] == TEXTS['ru'] and s.keys['ui.brightness'] == 70, s.keys
    assert 'test.n' not in s.keys
    result['file_after_first'] = {'bytes': len(data), 'keys': len(s.keys),
                                  'tables': {t.name: len(t.records) for t in s.tables.values()}}

    # ---- boot 2: all of it after a restart
    ph = Phone(args, card, out, 'second')
    try:
        for k, text in TEXTS.items():
            assert get_value(ph, f'test.{k}') == ('str', f'"{text}"'), k
        assert get_value(ph, 'ui.brightness') == ('int', '70')
        assert ph.qt.read(ph.settings) == 70
        assert get_value(ph, 'test.slider') == ('int', '49')
        assert ph.app('verify') == 'ok verify'
        ph.cmd('db set test.last "son değer" str')
        ph.power_off()
    except Exception:
        print(json.dumps(result, indent=2, ensure_ascii=False))
        raise
    finally:
        ph.close()

    # ---- the file cut in its last record, as a power cut would leave it
    img = bytearray(card.read_bytes())
    data, s = store_of(card)
    at = dir_entry(img, '/b310e-os/data')
    assert at and int.from_bytes(img[at + 28:at + 32], 'little') == len(data)
    last = DB['last_record'](data)
    cut = last + 6                      # into the header of the last record
    img[at + 28:at + 32] = cut.to_bytes(4, 'little')
    card.write_bytes(img)
    expect = DB['parse'](data[:cut])
    assert expect.error == 'a torn record', expect.error
    result['cut'] = {'file': len(data), 'at': cut}

    ph = Phone(args, card, out, 'third')
    try:
        for k, text in TEXTS.items():
            assert get_value(ph, f'test.{k}') == ('str', f'"{text}"'), k
        assert ph.app('verify') == 'ok verify'
        for k, v in expect.keys.items():
            if k.startswith('test.'):
                got = ph.cmd(f'db get "{k}"')
                assert got.split(' ', 2)[2].strip('"') in (str(v).lower(), str(v)), (k, v, got)
        time.sleep(3)                   # the log
        ph.power_off()
    except Exception:
        print(json.dumps(result, indent=2, ensure_ascii=False))
        raise
    finally:
        ph.close()
    img = card.read_bytes()
    log = (card_file(img, '/b310e-os/log') or b'').decode(errors='replace')
    (out / 'card-log.txt').write_text(log, encoding='utf-8')
    assert 'the rest left out' in log, 'the torn tail was not logged'
    assert card_file(img, '/b310e-os/data-bad'), 'the damaged file was not kept'
    data, s = store_of(card)
    assert s.error is None, s.error
    result['log'] = [l.strip() for l in log.splitlines() if ' db: ' in l][:12]
    (out / 'result.json').write_text(json.dumps(result, indent=2, ensure_ascii=False), encoding='utf-8')
    print('PASS ' + json.dumps(result, ensure_ascii=False))


DB_PATH_SHOWN = '/b310e-os/data:'

if __name__ == '__main__':
    main()
