#!/usr/bin/env python3
"""Boot the B310E OS in QEMU and check milestones 1 and 2 and the UI.

Reads the OS's `os_status` block (address from the ELF symbol table)
through QTest while the guest runs, and drives it with key presses:
  - the card (a FAT32 image whose FSINFO has no free count) mounts, the
    self-test passes (kernel objects plus a 32 KB card write and read),
    the free space is counted after boot and written back to FSINFO;
  - the CPU is mostly idle on the home screen;
  - Menu -> Apps lists the card's apps and opens the Stopwatch app, which
    is freed again when closed; Files lists the card, System and Settings
    open, Settings -> Display changes the brightness (60% -> 80%) and the
    screen timeout (30 s -> 15 s) through the option picker, END returns
    home;
  - the screen dims and then goes dark (panel asleep), a key wakes it;
  - the clock governor runs 104 MHz at rest and 26 MHz with the panel
    asleep, and the idle share stays high while dark (the accounting);
  - Power -> Power off -> Yes powers the machine off;
then checks the card image for the self-test file, the saved setting and
the card log, and saves a screenshot of every screen.

SPDX-License-Identifier: GPL-2.0-or-later
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import runpy
import shutil
import socket
import subprocess
import sys
import time

REPO = Path(__file__).resolve().parents[2]
QT = runpy.run_path(str(REPO / 'emulator/qemu/scripts/test-audio.py'))['QTest']
spec = importlib.util.spec_from_file_location(
    'capture', REPO / 'emulator/qemu/scripts/capture-stock.py')
capture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(capture)

FIELDS = ('magic ticks idle_permille selftest_done selftest_pass pi_wait_ms '
          'sem_rounds_per_s keys last_key worker_loops heap_free stray_irqs cycles idle_cycles '
          'sd_code fs_mounted fs_free_kib sd_retries sd_failures fs_write_kib_per_s '
          'fs_read_kib_per_s ui_depth ui_title ui_list_count battery_mv fs_count_ms ui_light apps_loaded cpu_mhz').split()
MAGIC = 0x534f3342
MENU = ['Apps', 'Files', 'Tools', 'System', 'Settings', 'Power']
SETTINGS = ['Display', 'Sound & touch', 'Power', 'Date & time', 'USB', 'Storage', 'About']
DISPLAY = ['Brightness', 'Screen off', 'Keypad light']
ST_ALL = 0x3f
FS_PATTERN = bytes((i * 7 + (i >> 8)) & 0xff for i in range(512))


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


def fsinfo_free(image):
    """(offset, free count) of the FAT32 FSINFO sector's free count."""
    for at in range(0, min(len(image), 64 << 20), 512):
        if image[at:at + 4] == b'RRaA' and image[at + 484:at + 488] == b'rrAa':
            return at + 488, int.from_bytes(image[at + 488:at + 492], 'little')
    raise AssertionError('no FSINFO sector in the card image')


def card_list(image, path):
    """[(name, is_dir, hidden)] of a folder on the FAT32 card image, in
    directory order (as the OS's Files lists them, folders first), or None."""
    return card_file(image, path, listing=True)


def card_file(image, path, listing=False):
    """The bytes of a file on the FAT32 card image (8.3 or long names; the
    path like '/b310e-os/log.txt', case-insensitive), or None. Follows the
    cluster chain, so a fragmented log reads whole."""
    part = 0
    if image[510:512] == b'\x55\xaa' and image[0x1be + 4] in (0x0b, 0x0c):
        part = int.from_bytes(image[0x1be + 8:0x1be + 12], 'little') * 512
    bs = image[part:part + 512]
    bps = int.from_bytes(bs[11:13], 'little')
    spc = bs[13]
    reserved = int.from_bytes(bs[14:16], 'little')
    nfats = bs[16]
    fatsz = int.from_bytes(bs[36:40], 'little')
    root = int.from_bytes(bs[44:48], 'little')
    fat = part + reserved * bps
    data = fat + nfats * fatsz * bps
    csize = spc * bps

    def chain(c):
        out = bytearray()
        seen = 0
        while 2 <= c < 0x0ffffff8 and seen < 1 << 20:
            at = data + (c - 2) * csize
            out += image[at:at + csize]
            c = int.from_bytes(image[fat + 4 * c:fat + 4 * c + 4], 'little') & 0x0fffffff
            seen += 1
        return bytes(out)

    def find(dirc, name):
        d = chain(dirc)
        lfn = {}
        entries = []
        for i in range(0, len(d), 32):
            e = d[i:i + 32]
            if e[0] == 0:
                break
            if e[0] == 0xe5:
                lfn = {}
                continue
            if e[11] == 0x0f:
                seq = e[0] & 0x1f
                lfn[seq] = (e[1:11] + e[14:26] + e[28:32]).decode('utf-16le', 'replace')
                continue
            short = e[0:8].decode('latin-1').rstrip()
            ext = e[8:11].decode('latin-1').rstrip()
            names = {short + ('.' + ext if ext else '')}
            if lfn:
                names.add(''.join(lfn[k] for k in sorted(lfn)).split('\0')[0].split('￿')[0])
            long_name = ''.join(lfn[k] for k in sorted(lfn)).split('\0')[0].split('￿')[0] if lfn else None
            lfn = {}
            if name is None:                # a listing
                if not (e[11] & 0x08) and short not in ('.', '..'):
                    entries.append((long_name or (short + ('.' + ext if ext else '')),
                                    bool(e[11] & 0x10), bool(e[11] & 0x06)))
                continue
            if name.lower() in (n.lower() for n in names):
                c = int.from_bytes(e[20:22], 'little') << 16 | int.from_bytes(e[26:28], 'little')
                return c, int.from_bytes(e[28:32], 'little'), bool(e[11] & 0x10)
        return entries if name is None else None

    c = root
    parts = [p for p in path.split('/') if p]
    if listing:
        for p in parts:
            hit = find(c, p)
            if hit is None or not hit[2]:
                return None
            c = hit[0]
        return find(c, None)
    for i, p in enumerate(parts):
        hit = find(c, p)
        if hit is None:
            return None
        c, size, is_dir = hit
        if i == len(parts) - 1:
            return None if is_dir else chain(c)[:size]
    return None


def status(qt, base):
    return {name: qt.read(base + 4 * i) for i, name in enumerate(FIELDS)}


def string(qt, addr, limit=64):
    out = b''
    while addr and len(out) < limit:
        word = qt.read(addr & ~3).to_bytes(4, 'little')[addr & 3:]
        if b'\0' in word:
            return (out + word.split(b'\0')[0]).decode(errors='replace')
        out += word
        addr = (addr & ~3) + 4
    return out.decode(errors='replace')


def wait_for(qt, base, test, seconds, what):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        s = status(qt, base)
        if test(s):
            return s
        time.sleep(0.1)
    raise AssertionError(f'{what}: {json.dumps(status(qt, base))}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qemu', type=Path, default=REPO / 'build/qemu/build/qemu-system-arm.exe')
    parser.add_argument('--elf', type=Path, default=REPO / 'build/os/b310e-os.elf')
    parser.add_argument('--image', type=Path, default=REPO / 'build/os/b310e-os.bin')
    parser.add_argument('--app', type=Path, default=REPO / 'build/os/modules/stopwatch.app')
    parser.add_argument('--output', type=Path, default=REPO / 'build/os/test-boot')
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    base = symbol(args.elf, 'os_status')
    card = out / 'card.img'
    subprocess.run([sys.executable, str(REPO / 'ports/rockbox/tests/make-sd-image.py'),
                    '--fat32', '--size-mib', '64', '--seconds', '1', '--force',
                    '--runtime', str(REPO / 'sdcard/.rockbox'),
                    '--rockbox', str(REPO / 'sdcard/payloads/rockbox.bin'),
                    '--add', f'b310e-os/apps/stopwatch.app={args.app.resolve()}',
                    '--add', f'payloads/b310e-os.bin={args.image.resolve()}',
                    '--output', str(card)], check=True, capture_output=True)
    # No free count in FSINFO (as on the phone's card, whose first boot took
    # 15 s counting): the OS must count in the background, not at boot.
    image = bytearray(card.read_bytes())
    fsinfo_at, _ = fsinfo_free(image)
    image[fsinfo_at:fsinfo_at + 4] = bytes([0xff] * 4)
    card.write_bytes(image)

    qmp_port, qtest_port = free_port(), free_port()
    command = [str(args.qemu.resolve()), '-M', 'b310e,boot-mode=rockbox',
               '-display', os.environ.get('B310E_QEMU_DISPLAY', 'none'), *os.environ.get('B310E_QEMU_AUDIO', '').split(), '-serial', 'none', '-monitor', 'none',
               '-drive', f'file={args.image.resolve().as_posix()},format=raw,if=none,id=os,readonly=on',
               '-drive', f'file={card.as_posix()},format=raw,if=none,id=sdcard',
               '-qmp', f'tcp:127.0.0.1:{qmp_port},server=on,wait=off',
               '-qtest', f'tcp:127.0.0.1:{qtest_port},server=on,wait=off',
               '-global', 'sc6530_adi.watchdog=on',
               '-D', str(out / 'qemu.log')]
    env = os.environ.copy()
    if os.name == 'nt':
        env['PATH'] = r'D:\Toolchains\msys64\mingw64\bin;C:\msys64\mingw64\bin;' + env.get('PATH', '')
    result = {}
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

            def screen():
                s = status(qt, base)
                return string(qt, s['ui_title']), s

            def shot(name):
                qmp.command('screendump', {'filename': str(out / f'{name}.png'), 'format': 'png'})

            def press(key, title, hold=100, seconds=6):
                qmp.hmp(f'sendkey {key} {hold}')
                end = time.monotonic() + seconds
                while time.monotonic() < end:
                    t, s = screen()
                    if t == title:
                        time.sleep(0.4)         # let it draw (its list's count with it)
                        return screen()[1]
                    time.sleep(0.1)
                raise AssertionError(f'after {key}: screen {screen()[0]!r}, wanted {title!r}')

            wait_for(qt, base, lambda s: s['magic'] == MAGIC, 10, 'OS never started')
            s = wait_for(qt, base, lambda s: s['selftest_done'] == ST_ALL, 60, 'self-test incomplete')
            result.update({k: s[k] for k in ('pi_wait_ms', 'sd_code', 'fs_mounted',
                                             'fs_free_kib', 'fs_write_kib_per_s',
                                             'fs_read_kib_per_s')})
            result['selftest_pass'] = hex(s['selftest_pass'])
            assert s['fs_mounted'] == 1, f'card not mounted: {s}'
            assert s['selftest_pass'] == ST_ALL, f'self-test failed: {s}'

            s = wait_for(qt, base, lambda s: s['ui_depth'] == 1, 10, 'no UI')
            assert screen()[0] == 'B310E OS'
            s = wait_for(qt, base, lambda s: s['fs_count_ms'], 20, 'free space never counted')
            result['fs_count_ms'] = s['fs_count_ms'] - 1
            result['fs_free_kib'] = s['fs_free_kib']
            assert s['fs_free_kib'] > 0, s
            time.sleep(3)
            s = status(qt, base)
            result['idle_permille'] = s['idle_permille']
            result['battery_mv'] = s['battery_mv']
            assert s['idle_permille'] > 800, f'not idle: {s}'
            s = wait_for(qt, base, lambda s: s['cpu_mhz'] == 104, 5, 'not 104 MHz at rest')
            result['mhz_at_rest'] = s['cpu_mhz']
            shot('home')

            s = press('ret', 'Menu')
            assert s['ui_list_count'] == len(MENU), s
            shot('menu')
            s = press('ret', 'Apps')
            assert s['ui_list_count'] == 1, f'apps listed: {s}'
            shot('apps')
            time.sleep(1.2)                  # heap_free is published each second
            heap_before = status(qt, base)['heap_free']
            s = press('ret', 'Stopwatch')
            assert s['apps_loaded'] == 1, s
            press('ret', 'Stopwatch')        # start
            time.sleep(1.5)
            press('f1', 'Stopwatch')         # lap
            time.sleep(0.5)
            shot('stopwatch')
            press('f2', 'Apps')              # close: the app is freed
            wait_for(qt, base, lambda s: s['apps_loaded'] == 0, 5, 'app not freed')
            time.sleep(1.2)
            result['app_heap_leak'] = heap_before - status(qt, base)['heap_free']
            assert result['app_heap_leak'] == 0, result
            press('f2', 'Menu')
            press('down', 'Menu')
            s = press('ret', '/')
            result['files_root'] = s['ui_list_count']
            assert s['ui_list_count'] > 0, 'empty card listing'
            shot('files')
            press('f2', 'Menu')
            for _ in range(MENU.index('System') - MENU.index('Files')):
                press('down', 'Menu')
            press('ret', 'System')
            time.sleep(1.2)
            shot('system')
            press('f2', 'Menu')
            press('down', 'Menu')
            s = press('ret', 'Settings')
            assert s['ui_list_count'] == len(SETTINGS), s
            shot('settings')
            s = press('ret', 'Display')      # the first category
            assert s['ui_list_count'] == len(DISPLAY), s
            shot('settings-display')
            s = press('ret', 'Brightness')   # the picker, on the current 60%
            assert s['ui_list_count'] == 10, s
            press('down', 'Brightness')
            press('down', 'Brightness')
            shot('settings-brightness')
            press('ret', 'Display')          # 80%: applied and saved
            press('down', 'Display')
            s = press('ret', 'Screen off')    # on the current 30 s
            assert s['ui_list_count'] == 7, s
            press('up', 'Screen off')
            press('ret', 'Display')          # 15 s
            shot('settings-display-set')
            press('f2', 'Settings')
            s = press('esc', 'B310E OS', hold=200)   # END: home (debounced: longer)
            assert s['ui_depth'] == 1, s
            # 15 s to dim, 10 s more to dark (QEMU runs ~0.7x real time)
            wait_for(qt, base, lambda s: s['ui_light'] == 1, 40, 'never dimmed')
            wait_for(qt, base, lambda s: s['ui_light'] == 2, 30, 'never went dark')
            time.sleep(0.5)
            led = qt.read(0x82001220)        # stock: bit 5 off, bit 6 on
            result['led_dark'] = hex(led)
            assert led & 0x60 == 0x20, f'backlight not switched off: {led:#x}'
            result['went_dark'] = True
            s = wait_for(qt, base, lambda s: s['cpu_mhz'] == 26, 5, 'not 26 MHz while dark')
            result['mhz_dark'] = s['cpu_mhz']
            time.sleep(2.5)                  # a full second counted while dark
            result['idle_dark'] = status(qt, base)['idle_permille']
            assert result['idle_dark'] > 900, f'not idle while dark: {result}'
            qmp.hmp('sendkey down 100')      # only wakes
            wait_for(qt, base, lambda s: s['ui_light'] == 0, 5, 'no wake')
            time.sleep(0.5)
            led = qt.read(0x82001220)
            result['led_woken'] = hex(led)
            result['mhz_woken'] = status(qt, base)['cpu_mhz']
            assert result['mhz_woken'] >= 104, result
            assert led & 0x60 == 0x40, f'backlight not back on: {led:#x}'
            assert screen()[0] == 'B310E OS' and status(qt, base)['ui_depth'] == 1
            shot('woken')
            press('ret', 'Menu')
            for _ in range(MENU.index('Power')):
                press('down', 'Menu')
            press('ret', 'Power')
            # Restart B310E OS: the image from the card, no reset (the boot
            # menu needs a key at power-on); the OS starts again from zero
            press('down', 'Power')
            press('down', 'Power')
            press('ret', 'Restart')
            t_before = status(qt, base)['ticks']
            qmp.hmp('sendkey ret 100')       # Yes
            wait_for(qt, base, lambda s: s['ticks'] < t_before and s['ui_depth'] == 1 and
                     string(qt, s['ui_title']) == 'B310E OS', 30, 'no restart')
            result['restarted_at_ticks'] = status(qt, base)['ticks']
            time.sleep(1.5)
            shot('restarted')
            press('ret', 'Menu')
            for _ in range(MENU.index('Power')):
                press('down', 'Menu')
            press('ret', 'Power')
            press('ret', 'Power off')        # the confirmation
            press('right', 'Power off')      # No selected: Center keeps it on
            shot('confirm-no')
            press('ret', 'Power')
            press('ret', 'Power off')
            press('down', 'Power off')       # any direction moves: No,
            press('up', 'Power off')         # then Yes again
            shot('confirm')
            result['stray_irqs'] = status(qt, base)['stray_irqs']
            qmp.hmp('sendkey ret 100')       # Center on Yes
            try:
                process.wait(timeout=15)
                result['powered_off'] = True
            except subprocess.TimeoutExpired:
                result['powered_off'] = False
            assert result['powered_off'], 'no power-off after Power off -> Yes'
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
    result['selftest_file'] = FS_PATTERN in image
    result['fsinfo_free_clusters'] = fsinfo_free(image)[1]
    settings = (card_file(image, '/b310e-os/settings.txt') or b'').decode(errors='replace')
    result['settings_file'] = settings.splitlines()
    result['settings_saved'] = ('brightness=80' in result['settings_file'] and
                                'screen_timeout=15' in result['settings_file'])
    log_text = (card_file(image, '/b310e-os/log.txt') or b'').decode(errors='replace')
    result['card_log'] = log_text.splitlines()[:10]
    (out / 'card-log.txt').write_text(log_text)
    assert result['selftest_file'], 'self-test file not on the card image'
    assert result['fsinfo_free_clusters'] != 0xffffffff, 'free count not written back to FSINFO'
    assert result['settings_saved'], ('brightness and timeout not saved', result['settings_file'])
    assert 'self-test card ok' in log_text and 'power off' in log_text, log_text
    assert 'boot: B310E OS built' in log_text and 'counted in' in log_text, log_text
    assert 'app: Stopwatch loaded' in log_text and 'stopwatch: started' in log_text, log_text
    assert 'app: Stopwatch closed' in log_text, log_text
    (out / 'result.json').write_text(json.dumps(result, indent=2))
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
