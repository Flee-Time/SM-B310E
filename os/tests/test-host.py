#!/usr/bin/env python3
"""Host tests: OS source files compiled for this PC with the undefined-
behaviour checks on, against small stand-ins (os/tests/host), no QEMU.

Each case is a host program built from the OS files as they are on disk
now plus a test in os/tests/host:

  db-power-cut  os/fs/db.c: a script of store operations with the card's
                power cut at every unit of it (each byte written, each rename,
                unlink, truncate, creation), the torn tail clean and junk;
                after the reboot each item is as before or as after the
                operation in flight, data-bad only for a damaged file
  db-oom        os/fs/db.c: each allocation of each operation, and of the
                load, failing in turn: RAM and card stay consistent, a load
                short of memory never damages the file
  crc32         os/lib/crc32.c against a bit-at-a-time reference
  string        os/lib/string.c (mem*, str*) against byte loops, every
                alignment, guard bytes and inaccessible pages around
  printf        os/lib/printf.c against the C library's snprintf, every
                buffer size, the cut on a whole UTF-8 character
  utf8-case     os/lib/utf8.c, utf8_valid (os/fs/db.c), the case tables of
                os/ui/textlang.c (Turkish i/I)
  gsm           os/net/lapdm.c, rr.c, mm.c, gsm_reg.c: self-tests, known
                frames, random and cut frames and messages
  art-fuzz      os/media/art.c's JPEG/PNG decoders: covers made with PIL,
                decoded at three sizes, then mutated (a fixed seed)

The stand-ins (os/tests/host/host.c): a guard allocator for kmalloc/kfree
(overruns, double frees, writes after free, leaks; "fail the n-th
allocation"), FatFs on files in RAM with a power-cut budget, a one-thread
kernel (mutexes count, a simulated clock), klog into a buffer.

The host's gcc (msys64 mingw64) has no 32-bit runtime and no UBSan or ASan
runtime: the programs are 64-bit (Windows is LLP64, so long stays 32 bits
as on the phone; pointers and size_t are 64) and the UBSan checks are built
in trap mode; a trap, an access violation or a heap error prints where
(addr2line) and what the case was doing, and saves the input that did it.

  python os/tests/test-host.py                 # every case, about 20 s
  python os/tests/test-host.py --case printf --case crc32
  python os/tests/test-host.py --long          # many more cuts and mutations
  python os/tests/test-host.py --prove         # each case built with a broken
                                               # reference: each must FAIL

One line a case: PASS/FAIL and its time. The exit status is 1 when one
fails. A file that does not compile (another change in progress) is tried
again once after a few seconds before the case counts as failed.

SPDX-License-Identifier: GPL-2.0-or-later
"""
import argparse
import concurrent.futures
import os
from pathlib import Path
import random
import re
import shutil
import subprocess
import sys
import tempfile
import time

REPO = Path(__file__).resolve().parents[2]
OS = REPO / 'os'
HOST = OS / 'tests' / 'host'
TOOLCHAIN = Path(r'D:\Toolchains\msys64\mingw64\bin')

# the OS's sources: their ARM-only attribute (crc32.c) means nothing here
OS_FLAGS = ['-Dtarget(x)=']
STRING_NAMES = ['memcpy', 'memmove', 'memset', 'memcmp', 'strlen', 'strcmp', 'strncpy',
                'strchr', 'strrchr']


def osrc(rel, *flags):
    return (OS / rel, OS_FLAGS + list(flags))


def tsrc(name, *flags):
    return (HOST / name, list(flags) if ANSI in flags else [NO_ANSI] + list(flags))


# the C library's own printf (C99, %lld %zu) where no OS header is included:
# os/lib.h declares vsnprintf, which mingw's C99 stdio defines static inline
ANSI = '-D__USE_MINGW_ANSI_STDIO=1'
NO_ANSI = '-D__USE_MINGW_ANSI_STDIO=0'


# program name -> its sources (path, extra flags)
PROGRAMS = {
    'test_db': [tsrc('test_db.c'),
                tsrc('test_db_glue.c', *OS_FLAGS),
                osrc('lib/crc32.c'), osrc('lib/utf8.c')],
    'test_crc32': [tsrc('test_crc32.c'), osrc('lib/crc32.c')],
    'test_string': [tsrc('test_string.c'),
                    osrc('lib/string.c', '-fno-builtin', '-fno-tree-loop-distribute-patterns',
                         *[f'-D{n}=os_{n}' for n in STRING_NAMES])],
    'test_printf': [tsrc('test_printf.c', ANSI),
                    osrc('lib/printf.c', '-fno-builtin', '-Dvsnprintf=os_vsnprintf',
                         '-Dsnprintf=os_snprintf'),
                    osrc('lib/utf8.c')],
    'test_utf8': [tsrc('test_utf8.c'), osrc('lib/utf8.c'), osrc('fs/db.c'), osrc('lib/crc32.c'),
                  osrc('ui/textlang.c')],
    'test_gsm': [tsrc('test_gsm.c'), osrc('net/lapdm.c'), osrc('net/rr.c'), osrc('net/mm.c'),
                 osrc('net/gsm_reg.c')],
    'test_art': [tsrc('test_art.c'), tsrc('test_art_glue.c', *OS_FLAGS)],
}

# objects whose .data and .bss are renamed after compiling (objcopy): db.c's
# statics between two markers, so a reboot can put them all back
# (test_db_glue.c)
RENAME_DATA = {'test_db_glue.c': ['--rename-section', '.data=.data$dbt1',
                                  '--rename-section', '.bss=.data$dbt1z,alloc,load,contents,data']}

# case -> (program, arguments), in the order they are reported
CASES = {
    'db-power-cut': ('test_db', ['power-cut']),
    'db-oom': ('test_db', ['oom']),
    'crc32': ('test_crc32', []),
    'string': ('test_string', []),
    'printf': ('test_printf', []),
    'utf8-case': ('test_utf8', []),
    'gsm': ('test_gsm', []),
    'art-fuzz': ('test_art', None),     # arguments made by art_args
}

CFLAGS = ['-std=gnu11', '-O1', '-g', '-fno-omit-frame-pointer', '-Wall', '-Wno-unused-function',
          '-Wno-format-truncation', '-fno-strict-aliasing',
          '-fsanitize=undefined', '-fsanitize=bounds-strict', '-fsanitize-trap=all',
          '-fstack-protector-strong']


def find_cc():
    cc = os.environ.get('B310E_HOST_CC')
    if cc:
        return Path(cc)
    if (TOOLCHAIN / 'gcc.exe').exists():
        return TOOLCHAIN / 'gcc.exe'
    found = shutil.which('gcc')
    if not found:
        sys.exit('test-host: no host gcc (set B310E_HOST_CC)')
    return Path(found)


def tool_env(cc):
    env = os.environ.copy()
    env['PATH'] = str(cc.parent) + os.pathsep + env.get('PATH', '')
    return env


def build(cc, name, out, prove):
    """Compile and link one program; (exe or None, the compiler's messages)."""
    objdir = out / ('prove' if prove else 'obj') / name
    objdir.mkdir(parents=True, exist_ok=True)
    exe = objdir / (name + '.exe' if os.name == 'nt' else name)
    inc = ['-I', str(HOST), '-I', str(OS / 'include'), '-I', str(OS)]
    extra = ['-DHOST_PROVE'] if prove else []
    env = tool_env(cc)
    objs, log = [], []
    sources = PROGRAMS[name] + [(HOST / 'host.c', [ANSI])]
    for src, flags in sources:
        obj = objdir / (src.parent.name + '_' + src.stem + '.o')
        r = subprocess.run([str(cc), *CFLAGS, *inc, *extra, *flags, '-c', str(src), '-o', str(obj)],
                           capture_output=True, text=True, env=env)
        if r.returncode:
            return None, f'{src.relative_to(REPO)} does not compile:\n{r.stderr.strip()}'
        if r.stderr.strip():
            log.append(r.stderr.strip())
        if src.name in RENAME_DATA:
            objcopy = cc.parent / ('objcopy.exe' if os.name == 'nt' else 'objcopy')
            r = subprocess.run([str(objcopy if objcopy.exists() else 'objcopy'),
                                *RENAME_DATA[src.name], str(obj)],
                               capture_output=True, text=True, env=env)
            if r.returncode:
                return None, f'objcopy on {obj.name} failed:\n{r.stderr.strip()}'
        objs.append(str(obj))
    link = [str(cc), *CFLAGS, *objs, '-o', str(exe)]
    if os.name == 'nt':
        link.append('-Wl,--disable-dynamicbase')    # fixed addresses for addr2line
    r = subprocess.run(link, capture_output=True, text=True, env=env)
    if r.returncode:
        return None, f'{name} does not link:\n{r.stderr.strip()}'
    return exe, '\n'.join(log)


def build_retry(cc, name, out, prove):
    exe, log = build(cc, name, out, prove)
    if exe is None:
        time.sleep(5)                   # another change to that file may be half done
        exe, log2 = build(cc, name, out, prove)
        log = log2 if exe is None else log2 + '\n(compiled on the second try)'
    return exe, log


ART_KINDS_OK = {'base420', 'base444', 'base422', 'prog', 'q100', 'q5', 'gray', 'rst', 'rgb',
                'rgba', 'pal', 'pal4', 'la', 'l', 'stored', 'trans', 'unsync', 'ogg', 'ogg2'}


def make_covers(out):
    """Covers made with PIL (cached), and their manifest."""
    d = out / 'covers'
    manifest = d / 'manifest-v2.txt'
    if manifest.exists():
        return manifest
    try:
        from PIL import Image
    except ImportError:
        return None
    import base64
    d.mkdir(parents=True, exist_ok=True)
    rng = random.Random(1)
    lines = []

    def add(path, flags=0, pos=0, ok=True, blank=False):
        # ok: 2 decodes and draws, 1 decodes (blank: all transparent), 0 may fail
        lines.append(f'{path.as_posix()} {flags} {pos} {(1 if blank else 2) if ok else 0}')

    for w, h in [(300, 300), (97, 61), (40, 130), (8, 8), (600, 450)]:
        im = Image.new('RGB', (w, h))
        px = im.load()
        for y in range(h):
            for x in range(w):
                px[x, y] = ((x * 255) // w, (y * 255) // h, ((x ^ y) * 7) & 255)
        for _ in range(w * h // 20):
            px[rng.randrange(w), rng.randrange(h)] = tuple(rng.randrange(256) for _ in range(3))
        tag = f'{w}x{h}'
        jpegs = [('base420', dict(subsampling=2)), ('base444', dict(subsampling=0)),
                 ('base422', dict(subsampling=1)), ('prog', dict(progressive=True)),
                 ('q100', dict(quality=100, subsampling=0)), ('q5', dict(quality=5))]
        if w <= 300:
            jpegs = jpegs[:1] + jpegs[3:4] + (jpegs[1:3] if w == 97 else [])
        for kind, kw in jpegs:
            p = d / f'{tag}_{kind}.jpg'
            im.save(p, 'JPEG', **kw)
            add(p)
        p = d / f'{tag}_gray.jpg'
        im.convert('L').save(p, 'JPEG')
        add(p)
        if w == 97:
            p = d / f'{tag}_rst.jpg'
            try:
                im.save(p, 'JPEG', restart_marker_blocks=3)
                add(p)
            except (TypeError, OSError):
                pass
        pngs = [('rgb', im), ('rgba', im.convert('RGBA')), ('pal', im.convert('P')),
                ('pal4', im.convert('P', palette=Image.ADAPTIVE, colors=16)),
                ('la', im.convert('LA')), ('l', im.convert('L')), ('1bit', im.convert('1'))]
        if w > 100:
            pngs = pngs[:2] + pngs[2:3]
        for kind, img in pngs:
            p = d / f'{tag}_{kind}.png'
            if kind == 'pal4':
                img.save(p, 'PNG', bits=4)
            else:
                img.save(p, 'PNG')
            add(p, ok=kind in ART_KINDS_OK)
        if w == 97:
            p = d / f'{tag}_stored.png'
            im.save(p, 'PNG', compress_level=0)
            add(p)
    im = Image.new('RGBA', (50, 50), (255, 0, 0, 0))
    p = d / 'trans.png'
    im.save(p)
    add(p, blank=True)
    # an ID3-style unsynchronised JPEG (FF -> FF 00), after 10 bytes of something
    src = (d / '97x61_base420.jpg').read_bytes()
    unsync = bytearray(b'ID3\x04\x00\x80\x00\x00\x00\x00')
    for b in src:
        unsync.append(b)
        if b == 0xff:
            unsync.append(0)
    p = d / 'cover_unsync.bin'
    p.write_bytes(bytes(unsync))
    add(p, flags=1, pos=10)
    # a FLAC-picture-like base64 value in a Vorbis comment, cut by Ogg page headers
    for name, img in (('ogg', '97x61_base420.jpg'), ('ogg2', '40x130_rgb.png')):
        text = b'METADATA_BLOCK_PICTURE=' + base64.b64encode((d / img).read_bytes())
        pages, pos, data = bytearray(), None, text
        seq = 0
        while data:
            part, data = data[:255 * 4], data[255 * 4:]
            segs = [255] * (len(part) // 255) + ([len(part) % 255] if len(part) % 255 else [])
            hdr = b'OggS\x00' + (b'\x01' if seq else b'\x00') + bytes(8) + b'\x01\x00\x00\x00' + \
                seq.to_bytes(4, 'little') + bytes(4) + bytes([len(segs)]) + bytes(segs)
            if pos is None:
                pos = len(pages) + len(hdr) + len('METADATA_BLOCK_PICTURE=')
            pages += hdr + part
            seq += 1
        p = d / f'cover_{name}.ogg'
        p.write_bytes(bytes(pages))
        add(p, flags=2, pos=pos)
    manifest.write_text('\n'.join(lines) + '\n', encoding='utf-8')
    return manifest


def run_case(case, exe, args, out, env, timeout):
    crash = out / f'{case}-crash.bin'
    if crash.exists():
        crash.unlink()
    env = dict(env, HOST_CRASH_FILE=str(crash))
    t0 = time.time()
    try:
        r = subprocess.run([str(exe), *args], capture_output=True, text=True, env=env,
                           timeout=timeout, errors='replace')
        code, so, se = r.returncode, r.stdout, r.stderr
    except subprocess.TimeoutExpired as e:
        code, so, se = -1, (e.stdout or b'').decode(errors='replace') if isinstance(e.stdout, bytes) \
            else (e.stdout or ''), f'timed out after {timeout} s'
    return code, so, se, time.time() - t0


def where(cc, exe, text):
    """addr2line for the address of a CRASH line"""
    m = re.search(r'CRASH: .* at (0x[0-9a-fA-F]+)', text)
    if not m:
        return ''
    a2l = cc.parent / ('addr2line.exe' if os.name == 'nt' else 'addr2line')
    if not a2l.exists():
        a2l = shutil.which('addr2line')
        if not a2l:
            return ''
    r = subprocess.run([str(a2l), '-f', '-i', '-p', '-e', str(exe), m.group(1)],
                       capture_output=True, text=True, env=tool_env(cc))
    return r.stdout.strip()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--case', action='append', choices=list(CASES), help='only this case (again for more)')
    ap.add_argument('--long', action='store_true', help='every cut point, thousands of mutations')
    ap.add_argument('--prove', action='store_true',
                    help='build each case with a deliberately wrong reference or stand-in: '
                         'each must fail (shows the checks can see a fault)')
    ap.add_argument('--out', type=Path, default=Path(tempfile.gettempdir()) / 'b310e-host-tests',
                    help='where the programs are built (default: %(default)s)')
    ap.add_argument('-v', '--verbose', action='store_true', help="every case's output")
    ap.add_argument('--seed', type=int, default=7, help='the art fuzzing seed (default 7)')
    ap.add_argument('--mutations', type=int, help='art mutations a cover (default 300, --long 4000)')
    args = ap.parse_args()
    cases = args.case or list(CASES)
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    cc = find_cc()
    env = tool_env(cc)
    names = sorted({CASES[c][0] for c in cases})
    print(f'test-host: {cc} ({"prove: each case must fail" if args.prove else "64-bit, UBSan traps"})')
    t0 = time.time()
    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
        built = dict(zip(names, pool.map(lambda n: build_retry(cc, n, out, args.prove), names)))
        manifest = make_covers(out) if 'art-fuzz' in cases else None
        jobs = {}
        for case in cases:
            name, cargs = CASES[case]
            exe, log = built[name]
            if exe is None:
                continue
            if cargs is None:
                if manifest is None:
                    continue
                n = args.mutations or (4000 if args.long else 300)
                cargs = [str(manifest), str(n), str(args.seed)]
            if args.long:
                cargs = cargs + ['--long']
            timeout = 3600 if args.long else 300
            jobs[case] = pool.submit(run_case, case, exe, cargs, out, env, timeout)
        failed = 0
        for case in cases:
            name = CASES[case][0]
            exe, log = built[name]
            if exe is None:
                print(f'FAIL  {case:14s}   --    {log.splitlines()[0]}')
                print('      ' + '\n      '.join(log.splitlines()[1:12]))
                failed += 1
                continue
            if case not in jobs:
                print(f'FAIL  {case:14s}   --    no covers: PIL is missing for this Python')
                failed += 1
                continue
            code, so, se, secs = jobs[case].result()
            ok = code == 0
            if args.prove:
                verdict = 'PASS' if not ok else 'FAIL'
                note = 'failed, as it must with the broken reference' if not ok else \
                    'DID NOT FAIL: its checks cannot see the broken reference'
            else:
                verdict = 'PASS' if ok else 'FAIL'
                note = so.strip().splitlines()[-1] if ok and so.strip() else \
                    (se.strip().splitlines()[0] if se.strip() else f'exit {code}')
            print(f'{verdict}  {case:14s} {secs:5.1f} s  {note}')
            if verdict == 'FAIL':
                failed += 1
            if (verdict == 'FAIL' and not args.prove) or args.verbose:
                text = (so + se).strip()
                lines = text.splitlines()
                show = lines if args.verbose else lines[:40]
                print('      ' + '\n      '.join(show))
                loc = where(cc, exe, se)
                if loc:
                    print(f'      at: {loc}')
                if log and args.verbose:
                    print('      compiler: ' + log.replace('\n', '\n      '))
            elif args.prove and args.verbose is False and ok is False:
                first = next((l for l in se.splitlines() if l.startswith(('FAIL', 'CRASH'))), '')
                if first:
                    print(f'      ({first.strip()})')
    print(f'test-host: {len(cases) - failed} of {len(cases)} passed in {time.time() - t0:.1f} s')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
