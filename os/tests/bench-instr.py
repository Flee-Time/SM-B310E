#!/usr/bin/env python3
"""Exact instruction counts of the OS's everyday work, per scenario and per
function, in QEMU: "the same pixels for fewer instructions" gets a number
before and after every change.

QEMU runs with -icount shift=2,sleep=off (one instruction 4 ns of guest
time; idle, the clock jumps to the next timer at once) and the b310e-prof
TCG plugin (emulator/qemu/plugins/b310e-prof.c, built on demand by
emulator/qemu/scripts/build-prof-plugin.py). The plugin counts every
instruction at its address and holds the CPU before a WFI when asked; held,
the guest's clock stands still, so the keys are sent and the counters read
at the same instruction every run: two runs of the same image count the
same. Not the phone's time (no caches, no PSRAM or card latency): the
instructions the code runs, which is what an optimisation changes.

The card (built as test-boot.py builds its own) holds /bench with 150
files, three old note files (Notes reads them into the store the first time
it opens), notes.app and stopwatch.app; settings.txt turns word completion
off (typing would otherwise teach words, and the store would grow run by
run). The RTC starts at 00:00:00 each run.

Until 2026-11-18 QEMU had no END-key (EIC) interrupt, so the OS polled END
there (every 20 ms lit, 100 ms dark; see its card log's "end key:" line), as
the phone does not. QEMU models it now (sc6530_adi eic-irq, on by default):
home-lit and dark count what the phone runs. But with no 20 ms poll a hold
(at a WFI once the ticks are reached) comes only at the guest's next wake,
so the key phases' windows stretch; `--qemu-global sc6530_adi.eic-irq=off`
gives the old polling and timing, comparable with a baseline made before.

Phases (each from Home, lit, back to Home; what the counters cover):
  boot          power-on to the first idle point after the UI logged "boot:
                home drawn" (Home's first frame sent); boot-1s: power-on to
                the first idle point at 1 s (times: the kernel's ticks, which
                start after the card's power-up wait)
  home-lit      10 s idle on Home, lit (per second)
  dark          60 s with the screen dark (per second; dark, the UI's work
                runs every 30 s, so 10 s would catch it or not)
  menu-nav      26 keys: through the menu, Settings and back
  list-scroll   30 down and 30 up in /bench, 150 entries (per key)
  dialog        a message (Center on a file) and the Delete confirmation from
                Options, Yes/No moved 5 times, No (per LCD transfer)
  picker        Settings -> Display -> Brightness: the picker, 3 down, 3 up,
                picked (the same value)
  quick-panel   * + #, 3 down, 3 up, closed
  text-edit     Notes, a new note, 20 multi-tap keys, the symbol picker
                (* held) opened and closed, saved (deleted again after)
  app-open-close  Apps -> Stopwatch opened and closed 3 times (per cycle)
  settings-set  Settings -> Text input -> Completion toggled 20 times (per set)
Each window ends 2 s after its last key, so the card log's and the store's
deferred writes are counted with what caused them. Per function: the
instructions and the calls (how often its first instruction ran); heap
code goes to the app loaded there ("app:Notes"), IRAM code to its own
bucket. A phase run alone can differ from the same phase in a full run by
a few thousand instructions (the status clock's digits, the store's state).

  python os/tests/bench-instr.py [--phase NAME ...] [--json out.json]
  python os/tests/bench-instr.py --compare before.json after.json

Results: build/os/bench-instr/ (per phase a CSV of every function, a
screenshot at the window's end, the raw dumps), the JSON where --json says.

SPDX-License-Identifier: GPL-2.0-or-later
"""
import argparse
import bisect
import contextlib
import hashlib
import json
import os
from pathlib import Path
import runpy
import shutil
import subprocess
import sys
import time

REPO = Path(__file__).resolve().parents[2]
BOOT = runpy.run_path(str(Path(__file__).with_name('test-boot.py')))
QT, capture, card_list = BOOT['QT'], BOOT['capture'], BOOT['card_list']
free_port, string, FIELDS, MAGIC = BOOT['free_port'], BOOT['string'], BOOT['FIELDS'], BOOT['MAGIC']
MENU, SETTINGS = BOOT['MENU'], BOOT['SETTINGS']
PLUGIN = runpy.run_path(str(REPO / 'emulator/qemu/scripts/build-prof-plugin.py'))
NM = shutil.which('arm-none-eabi-nm') or 'D:/Toolchains/arm-none-eabi/bin/arm-none-eabi-nm.exe'

ICOUNT_SHIFT = 2
HOME = 'B310E OS'
SETTLE_MS = 2000                        # after a window's last key
BENCH_FILES = 150
NOTES = ['Shopping: milk, bread', 'Call the bank', 'Meeting at 10']
TEXT_INPUT = ['Language', 'Languages', 'Completion']
# 20 multi-tap keys: "hello wopa" (os/ui/textlang.c, English)
MULTITAP = ['44', '33', '555', '555', '666', '0', '9', '666', '7', '2']
HEAP_END = 0x043f1f00                   # os/link/os.ld: the fault record
IRAM = (0x40000000, 0x40010000)
STOP_IRAM, CHAIN_IRAM = 0x40003000, 0x4000f800
HIGH_VECTORS = 0xffff0000
PHASES = ['boot', 'home-lit', 'dark', 'menu-nav', 'list-scroll', 'dialog', 'picker',
          'quick-panel', 'text-edit', 'app-open-close', 'settings-set']


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


# ---- symbols ------------------------------------------------------------------

class Symbols:
    """Code addresses -> function (and source file) from the ELF; the heap,
    IRAM and the high vector page by their own rules."""

    def __init__(self, elf):
        out = subprocess.run([NM, '-n', '-S', '-l', '--defined-only', str(elf)],
                             capture_output=True, text=True, check=True).stdout
        funcs, self.addr = [], {}
        for line in out.splitlines():
            where = ''
            if '\t' in line:
                line, where = line.split('\t', 1)
            parts = line.split()
            if len(parts) == 4:
                at, size, kind, name = int(parts[0], 16), int(parts[1], 16), parts[2], parts[3]
            elif len(parts) == 3:
                at, size, kind, name = int(parts[0], 16), 0, parts[1], parts[2]
            else:
                continue
            self.addr.setdefault(name, at)
            if kind in 'TtWw' and not name.startswith('$'):
                src = where.rsplit(':', 1)[0].replace('\\', '/')
                if '/os/' in src:
                    src = src.split('/os/', 1)[1]
                funcs.append((at & ~1, size, name, src or '?'))
        funcs.sort()
        names = {}
        for _, _, name, src in funcs:
            names.setdefault(name, set()).add(src)
        self.starts = [f[0] for f in funcs]
        self.funcs = [(at, size, f'{name} ({src.rsplit("/", 1)[-1]})' if len(names[name]) > 1
                       else name, src if src != '?' else '(no line info: libgcc, asm)')
                      for at, size, name, src in funcs]
        self.entry = {name: at for at, _, name, _ in self.funcs}
        self.heap = (self.addr['__heap_start'], HEAP_END)
        self.vectors = self.addr['_vectors']
        self.iram = [(STOP_IRAM, STOP_IRAM + self.addr['sleep_stop_end'] - self.addr['sleep_stop_code'],
                      'iram:sleep_stop_code'),
                     (CHAIN_IRAM, CHAIN_IRAM + self.addr['chain_tramp_end'] - self.addr['chain_tramp'],
                      'iram:chain_tramp')]

    def lookup(self, va, apps=()):
        """(function, source file) of an instruction address"""
        if va >= HIGH_VECTORS:
            va = va - HIGH_VECTORS + self.vectors
        if self.heap[0] <= va < self.heap[1]:
            for start, end, name in apps:
                if start <= va < end:
                    return f'app:{name}', f'app:{name}'
            return 'heap:other', 'heap'
        if IRAM[0] <= va < IRAM[1]:
            for start, end, name in self.iram:
                if start <= va < end:
                    return name, 'iram'
            return 'iram:other', 'iram'
        i = bisect.bisect_right(self.starts, va) - 1
        if i < 0:
            return f'?{va:#x}', '?'
        at, size, name, src = self.funcs[i]
        if size and va >= at + size:            # past a sized function: a gap
            # an unsized label (assembly) before it still owns it
            for j in range(i - 1, max(i - 8, -1), -1):
                a2, s2, n2, f2 = self.funcs[j]
                if not s2 and a2 <= va:
                    return n2, f2
            return f'?{at:#x}+', src
        return name, src


# ---- the machine ----------------------------------------------------------------

class Bench:
    def __init__(self, args, out):
        self.args, self.out = args, out
        self.syms = Symbols(args.elf)
        self.base = self.syms.addr['os_status']
        self.ctl = out / 'ctl'
        shutil.rmtree(self.ctl, ignore_errors=True)
        self.ctl.mkdir(parents=True)
        self.seq = 0
        self.now = 0                    # guest ms at the hold
        self.apps_seen = {}
        self.results = {}
        self.window_on = False

    # -- start, stop
    def make_card(self):
        files = self.out / 'files'
        files.mkdir(exist_ok=True)
        adds = []
        for i in range(1, BENCH_FILES + 1):
            p = files / f'file{i:03d}.dat'
            p.write_bytes(bytes((i * 7 + k) & 0xff for k in range(64 + i)))
            adds += ['--add', f'bench/{p.name}={p}']
        for i, text in enumerate(NOTES, 1):
            p = files / f'NOTE{i:04d}.TXT'
            p.write_bytes(text.encode() + b'\n')
            adds += ['--add', f'b310e-os/notes/{p.name}={p}']
        settings = files / 'settings.txt'
        settings.write_bytes(b'completion=0\n')
        card = self.out / 'card.img'
        subprocess.run([sys.executable, str(REPO / 'ports/rockbox/tests/make-sd-image.py'),
                        '--fat32', '--size-mib', '64', '--seconds', '1', '--force',
                        '--runtime', str(REPO / 'sdcard/.rockbox'),
                        '--rockbox', str(REPO / 'sdcard/payloads/rockbox.bin'),
                        '--add', f'b310e-os/settings.txt={settings}',
                        '--add', f'b310e-os/apps/stopwatch.app={(self.args.modules / "stopwatch.app").resolve()}',
                        '--add', f'b310e-os/apps/notes.app={(self.args.modules / "notes.app").resolve()}',
                        *adds, '--output', str(card)], check=True, capture_output=True)
        image = card.read_bytes()
        self.card_sha = hashlib.sha256(image).hexdigest()
        root = [n for n, d, h in card_list(image, '/') if d and not h]
        self.bench_row = root.index('bench')
        return card

    def start(self):
        plugin = PLUGIN['build_plugin']()
        card = self.make_card()
        qmp_port, qtest_port = free_port(), free_port()
        cmd = [str(self.args.qemu.resolve()), '-M', 'b310e,boot-mode=rockbox',
               '-display', 'none', '-serial', 'none', '-monitor', 'none',
               '-icount', f'shift={ICOUNT_SHIFT},sleep=off,align=off',
               '-audiodev', 'none,id=noaudio', '-global', 'sc6530_adi.audiodev=noaudio',
               '-drive', f'file={self.args.image.resolve().as_posix()},format=raw,if=none,id=os,readonly=on',
               '-drive', f'file={card.as_posix()},format=raw,if=none,id=sdcard',
               '-qmp', f'tcp:127.0.0.1:{qmp_port},server=on,wait=off',
               '-qtest', f'tcp:127.0.0.1:{qtest_port},server=on,wait=off',
               '-plugin', f'file={plugin.as_posix()},ctl={self.ctl.as_posix()},'
                          f'ticks={self.syms.addr["ticks"]:#x}',
               '-D', str(self.out / 'qemu.log')]
        for g in getattr(self.args, 'qemu_global', None) or ():   # sc6530_adi.eic-irq=off
            cmd += ['-global', g]
        env = os.environ.copy()
        if os.name == 'nt':
            env['PATH'] = r'D:\Toolchains\msys64\mingw64\bin;C:\msys64\mingw64\bin;' + env.get('PATH', '')
        self.log = (self.out / 'stderr.log').open('wb')
        self.proc = subprocess.Popen(cmd, env=env, stdout=self.log, stderr=self.log)
        self.qmp = self.qt = None
        for _ in range(200):
            try:
                self.qmp = capture.QMP(qmp_port)
                self.qt = QT(qtest_port)
                break
            except OSError:
                time.sleep(0.1)
        assert self.qmp and self.qt, 'QEMU did not start'
        self.ack = self.wait_ack(0)     # the first WFI

    def close(self):
        for c in (self.qt, self.qmp):
            if c:
                with contextlib.suppress(Exception):
                    c.close()
        if self.proc.poll() is None:
            self.proc.terminate()
            self.proc.wait(timeout=10)
        self.log.close()

    # -- the plugin's commands
    def wait_ack(self, seq, seconds=900):
        path = self.ctl / f'ack-{seq}'
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            try:
                text = path.read_text()
            except OSError:
                if self.proc.poll() is not None:
                    raise AssertionError(f'QEMU exited ({self.proc.returncode}), see {self.out}')
                time.sleep(0.0005)
                continue
            path.unlink()
            _, ticks, total, wfi = (int(v) for v in text.split())
            self.now = ticks
            return {'ticks': ticks, 'total': total, 'wfi': wfi}
        raise AssertionError(f'no ack {seq} from the plugin in {seconds} s (guest busy?)')

    def send(self, text):
        self.seq += 1
        tmp = self.ctl / f'cmd-{self.seq}.tmp'
        tmp.write_text(text + '\n')
        os.replace(tmp, self.ctl / f'cmd-{self.seq}')
        self.ack = self.wait_ack(self.seq)
        return self.ack

    def until(self, ticks):
        self.send(f'until {ticks}')
        if self.window_on:
            self.note_apps()

    def wait(self, ms):
        self.until(self.now + ms)

    def dump(self, name):
        path = self.out / 'dumps' / f'{name}.txt'
        path.parent.mkdir(exist_ok=True)
        self.send(f'dump {path.as_posix()}')
        counts = {}
        with path.open() as f:
            header = f.readline().split()
            for line in f:
                va, _, n = line.split()
                counts[int(va, 16)] = int(n)
        assert sum(counts.values()) == int(header[2]), header
        return counts

    # -- the guest
    def field(self, name):
        return self.qt.read(self.base + 4 * FIELDS.index(name))

    def title(self):
        return string(self.qt, self.field('ui_title'))

    def klog_text(self):
        """what klog has written (the last 8 KB: fs/fs.c log_tail)"""
        n = min(self.qt.read(self.syms.addr['tail_pos']), 8192)
        if not n:
            return ''
        data = bytes.fromhex(self.qt.command(f'read {self.syms.addr["log_tail"]:#x} {n:#x}')[0][2:])
        return data.decode(errors='replace')

    def at_home(self):
        return self.field('ui_depth') == 1 and self.title() == HOME

    def note_apps(self):
        """the loaded apps' code ranges, for the heap's attribution"""
        a = self.qt.read(self.syms.addr['apps'])
        while a:
            image, size = self.qt.read(a + 8), self.qt.read(a + 12)
            self.apps_seen[(image, image + size)] = string(self.qt, a + 28, 20)
            a = self.qt.read(a)

    def key(self, k, want=None, hold=100, gap=400):
        """a key held hold ms, then gap ms of guest time from its press;
        want: the title it must lead to (waited for up to 5 s more)"""
        if k == 'esc':
            hold, gap = max(hold, 200), max(gap, 600)
        self.qmp.hmp(f'sendkey {k} {hold}')
        self.wait(gap)
        if want is not None:
            for _ in range(50):
                if self.title() == want:
                    return
                self.wait(100)
            self.shot('failed')
            raise AssertionError(f'after {k}: {self.title()!r}, wanted {want!r} (t={self.now} ms)')

    def shot(self, name):
        self.qmp.command('screendump', {'filename': str(self.out / f'{name}.png'), 'format': 'png'})

    def home(self):
        if self.field('ui_light') != 0:
            self.key('esc')             # the first key only wakes
        if not self.at_home():
            self.key('esc', HOME)
        assert self.at_home(), self.title()
        self.wait(1000)

    def menu(self, item, want=None):
        self.key('ret', 'Menu')
        for _ in range(MENU.index(item)):
            self.key('down', 'Menu')
        self.key('ret', want or item)

    def settings(self, item):
        self.menu('Settings')
        for _ in range(SETTINGS.index(item)):
            self.key('down', 'Settings')
        self.key('ret', item)

    # -- windows
    @contextlib.contextmanager
    def window(self, name, unit=None, units=None):
        self.apps_seen = {}
        self.note_apps()
        start, t0 = self.dump(f'{name}-start'), self.now
        lcd = ('lcd_kicks', 'lcd_rows_sent', 'lcd_skipped')
        lcd0 = [self.qt.read(self.syms.addr[v]) for v in lcd]
        self.window_on = True
        info = {}
        yield info
        self.wait(SETTLE_MS)
        self.window_on = False
        end = self.dump(f'{name}-end')
        self.shot(name)
        kicks, rows, skipped = (self.qt.read(self.syms.addr[v]) - v0 for v, v0 in zip(lcd, lcd0))
        delta = {va: n - start.get(va, 0) for va, n in end.items() if n != start.get(va, 0)}
        self.record(name, delta, self.now - t0, unit, info.get('units', units), kicks, rows,
                    skipped)

    def record(self, name, delta, guest_ms, unit=None, units=None, kicks=None, rows=None,
               skipped=None):
        apps = [(s, e, n) for (s, e), n in self.apps_seen.items()]
        funcs, files, buckets = {}, {}, {}
        for va, n in delta.items():
            fn, src = self.syms.lookup(va, apps)
            funcs[fn] = funcs.get(fn, 0) + n
            files[src] = files.get(src, 0) + n
        total = sum(delta.values())
        # calls: how often its first instruction ran (a loop back to the very
        # entry would count too; compilers rarely do that)
        calls = {fn: delta.get(self.syms.entry[fn], 0) for fn in funcs if fn in self.syms.entry}
        r = {'total': total, 'guest_ms': guest_ms, 'unit': unit, 'units': units,
             'per_unit': round(total / units) if units else None,
             'lcd_kicks': kicks, 'lcd_rows': rows, 'lcd_skipped': skipped,
             'functions': dict(sorted(funcs.items(), key=lambda kv: -kv[1])),
             'calls': calls,
             'files': dict(sorted(files.items(), key=lambda kv: -kv[1]))}
        if kicks and unit == 'redraw':
            r['units'], r['per_unit'] = kicks, round(total / kicks)
        self.results[name] = r
        with (self.out / f'{name}.csv').open('w', newline='') as f:
            f.write('function,instructions,share,calls\n')
            for fn, n in r['functions'].items():
                f.write(f'"{fn}",{n},{n / total:.6f},{calls.get(fn, "")}\n')
        report(name, r, self.args.top)


def report(name, r, top):
    per = f', {r["per_unit"]:,} per {r["unit"]} ({r["units"]})' if r.get('per_unit') else ''
    kicks = (f', {r["lcd_kicks"]} LCD transfers ({r["lcd_rows"]} rows), '
             f'{r["lcd_skipped"]} updates unchanged' if r.get('lcd_kicks') is not None else '')
    print(f'\n== {name}: {r["total"]:,} instructions in {r["guest_ms"]} ms{per}{kicks}')
    for fn, n in list(r['functions'].items())[:top]:
        c = r['calls'].get(fn)
        calls = f'  ({c:,} calls, {n // c:,} each)' if c else ''
        print(f'  {n:>14,} {100 * n / max(r["total"], 1):5.1f}%  {fn}{calls}')
    if top:
        print('  files: ' + ', '.join(f'{f} {100 * n / max(r["total"], 1):.0f}%'
                                      for f, n in list(r['files'].items())[:6]))
    sys.stdout.flush()


# ---- the phases (each from Home, lit, back to Home) ------------------------------

def boot(b):
    got_1s = got_home = False
    for _ in range(100000):
        if not got_1s and b.now >= 1000:
            b.record('boot-1s', b.dump('boot-1s'), b.now)
            got_1s = True
        if not got_home and b.field('magic') == MAGIC and 'boot: home drawn' in b.klog_text():
            b.record('boot', b.dump('boot'), b.now)
            b.shot('boot')
            got_home = True
        if got_1s and got_home:
            break
        if got_home:
            b.until(1000)
        else:
            b.send('run')               # every idle point: home's first frame
    # the rest of the start (free space, the log, the store) and Notes once
    # (it reads the old note files into the store): not in any window
    b.until(max(b.now, 5000))
    b.home()
    b.menu('Apps')
    b.key('ret', 'Notes', gap=1500)
    b.key('f2', 'Apps')
    b.key('esc', HOME)
    b.wait(3000)


def home_lit(b):
    b.home()
    with b.window('home-lit', 'second', 10):
        b.wait(10000 - SETTLE_MS)


def dark(b):
    b.home()
    for _ in range(120):                # the timeout: dim, then dark
        if b.field('ui_light') == 2:
            break
        b.wait(1000)
    assert b.field('ui_light') == 2, 'never went dark'
    b.wait(2000)
    with b.window('dark', 'second', 60):
        b.wait(60000 - SETTLE_MS)
    b.home()


def menu_nav(b):
    b.home()
    with b.window('menu-nav', 'key', 26):
        b.key('ret', 'Menu')
        for _ in range(5):
            b.key('down', 'Menu')
        b.key('up', 'Menu')             # Settings
        b.key('ret', 'Settings')
        for _ in range(8):
            b.key('down', 'Settings')
        for _ in range(8):
            b.key('up', 'Settings')
        b.key('f2', 'Menu')
        b.key('esc', HOME)
    b.home()


def open_bench_folder(b):
    b.menu('Files', '/')
    for _ in range(b.bench_row):
        b.key('down', '/')
    b.key('ret', '/bench', gap=1000)
    assert b.field('ui_list_count') == BENCH_FILES, b.field('ui_list_count')


def list_scroll(b):
    b.home()
    open_bench_folder(b)
    with b.window('list-scroll', 'key', 60):
        for _ in range(30):
            b.key('down', '/bench', gap=300)
        for _ in range(30):
            b.key('up', '/bench', gap=300)
    b.home()


def dialog(b):
    b.home()
    open_bench_folder(b)
    with b.window('dialog', 'redraw'):
        b.key('ret', 'File')            # a file of no known kind: a message
        b.key('ret', '/bench')          # any key closes it
        b.key('f1', 'Options')
        b.key('down', 'Options')
        b.key('ret', 'Delete file')
        for k in ('right', 'left', 'down', 'up', 'right'):
            b.key(k, 'Delete file')     # Yes/No: No at the end
        b.key('ret', '/bench')          # No: kept
    assert b.field('ui_list_count') == BENCH_FILES, 'a file was deleted'
    b.home()


def picker(b):
    b.home()
    b.settings('Display')
    with b.window('picker', 'key', 8):
        b.key('ret', 'Brightness')
        for k in ('down', 'down', 'down', 'up', 'up', 'up'):
            b.key(k, 'Brightness')
        b.key('ret', 'Display')         # the same value again
    b.home()


def quick_panel(b):
    b.home()
    with b.window('quick-panel', 'key', 8):
        b.key('minus-slash', 'Quick panel', hold=150)
        for k in ('down', 'down', 'down', 'up', 'up', 'up'):
            b.key(k, 'Quick panel')
        b.key('f2', HOME)
    b.home()


def text_edit(b):
    b.home()
    b.menu('Apps')
    b.key('ret', 'Notes', gap=1500)
    with b.window('text-edit', 'key', 24):
        b.key('ret', 'New note')
        for group in MULTITAP:
            for i, k in enumerate(group):
                # taps 300 ms apart; after a letter's last, 1.2 s: chosen
                b.key(k, gap=300 if i < len(group) - 1 else 1200)
        b.key('minus', 'Symbols', hold=1000, gap=1500)   # * held: the symbols
        b.key('down', 'Symbols')
        b.key('right', 'Symbols')
        b.key('f2', 'New note')         # closed: nothing picked
        b.key('ret', 'Notes', gap=1000)  # saved
    # the note again out of the store (newest first: the row under New note)
    b.key('down', 'Notes')
    b.key('f1', 'Delete note')
    b.key('ret', 'Notes', gap=1000)
    b.home()


def app_open_close(b):
    b.home()
    b.menu('Apps')
    b.key('down', 'Apps')               # Notes, Stopwatch
    with b.window('app-open-close', 'cycle', 3):
        for _ in range(3):
            b.key('ret', 'Stopwatch', gap=1000)
            b.key('f2', 'Apps', gap=1000)
    b.home()


def settings_set(b):
    b.home()
    b.settings('Text input')
    for _ in range(TEXT_INPUT.index('Completion')):
        b.key('down', 'Text input')
    with b.window('settings-set', 'set', 20):
        for _ in range(20):
            b.key('ret', 'Text input', gap=300)
    b.home()


RUN = {'home-lit': home_lit, 'dark': dark, 'menu-nav': menu_nav, 'list-scroll': list_scroll,
       'dialog': dialog, 'picker': picker, 'quick-panel': quick_panel, 'text-edit': text_edit,
       'app-open-close': app_open_close, 'settings-set': settings_set}


# ---- compare ----------------------------------------------------------------------

def compare(a_path, b_path, movers):
    a, b = (json.loads(Path(p).read_text()) for p in (a_path, b_path))
    print(f'{"phase":<16}{"before":>16}{"after":>16}{"change":>9}{"per unit before":>18}{"after":>12}')
    for name in [n for n in a['phases'] if n in b['phases']]:
        pa, pb = a['phases'][name], b['phases'][name]
        ch = 100 * (pb['total'] - pa['total']) / pa['total'] if pa['total'] else 0
        pu = lambda p: f'{p["per_unit"]:,}' if p.get('per_unit') else '-'
        print(f'{name:<16}{pa["total"]:>16,}{pb["total"]:>16,}{ch:>8.2f}%{pu(pa):>18}{pu(pb):>12}')
    for name in [n for n in a['phases'] if n in b['phases']]:
        fa, fb = a['phases'][name]['functions'], b['phases'][name]['functions']
        moved = sorted(((fb.get(f, 0) - fa.get(f, 0), f) for f in set(fa) | set(fb)),
                       key=lambda d: -abs(d[0]))
        moved = [m for m in moved if m[0]][:movers]
        if moved:
            print(f'\n{name}: the functions that moved most')
            for d, f in moved:
                print(f'  {d:>+14,}  {fa.get(f, 0):>13,} -> {fb.get(f, 0):<13,} {f}')


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--phase', action='append', choices=PHASES[1:], metavar='NAME',
                        help='run only these (boot always runs); default: all')
    parser.add_argument('--json', type=Path, help='write the results here')
    parser.add_argument('--top', type=int, default=15, help='functions printed per phase')
    parser.add_argument('--compare', nargs=2, type=Path, metavar=('BEFORE', 'AFTER'))
    parser.add_argument('--movers', type=int, default=10, help='--compare: functions per phase')
    parser.add_argument('--qemu', type=Path, default=REPO / 'build/qemu/build/qemu-system-arm.exe')
    parser.add_argument('--elf', type=Path, default=REPO / 'build/os/b310e-os.elf')
    parser.add_argument('--image', type=Path, default=REPO / 'build/os/b310e-os.bin')
    parser.add_argument('--modules', type=Path, default=REPO / 'build/os/modules')
    parser.add_argument('--output', type=Path, default=REPO / 'build/os/bench-instr')
    # Since the END key's interrupt is modelled (2026-11-18) the OS no longer
    # polls END every 20 ms lit, and a hold waits for the next WFI with the
    # ticks reached: windows stretch to the guest's sleeps (list-scroll took
    # 62 s of guest time for its 20 s). sc6530_adi.eic-irq=off polls as the
    # baseline's QEMU did, for the key phases.
    parser.add_argument('--qemu-global', action='append', metavar='DRIVER.PROP=VALUE',
                        help='a -global for QEMU (repeatable), e.g. sc6530_adi.eic-irq=off')
    args = parser.parse_args()
    if args.compare:
        compare(*args.compare, args.movers)
        return
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    stamp = {p.name: (p.stat().st_mtime, sha256(p)) for p in (args.elf, args.image)}
    b = Bench(args, out)
    t0 = time.monotonic()
    try:
        b.start()
        boot(b)
        for name in args.phase or PHASES[1:]:
            RUN[name](b)
    finally:
        b.close()
    after = {p.name: (p.stat().st_mtime, sha256(p)) for p in (args.elf, args.image)}
    assert after == stamp, 'the OS image changed during the run: run again'
    result = {'meta': {'elf': str(args.elf), 'elf_sha256': stamp[args.elf.name][1],
                       'image_sha256': stamp[args.image.name][1],
                       'image_mtime': time.strftime('%Y-%m-%d %H:%M:%S',
                                                    time.localtime(stamp[args.image.name][0])),
                       'card_sha256': b.card_sha, 'icount_shift': ICOUNT_SHIFT,
                       'qemu_global': args.qemu_global or [],
                       'host_seconds': round(time.monotonic() - t0, 1),
                       'qemu': str(args.qemu)},
              'phases': b.results}
    print('\n' + json.dumps({n: (r['total'], r.get('per_unit')) for n, r in b.results.items()}))
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(result, indent=1))


if __name__ == '__main__':
    main()
