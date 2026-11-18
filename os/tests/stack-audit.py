#!/usr/bin/env python3
"""Stack audit: every thread's worst-case stack depth from the compiler's own
frame sizes, next to the stack it is given. A report, not a pass/fail test.

The OS's C files (the Makefile's SRCS_C, plus FatFs) are compiled again into
a scratch folder (not build/os) with the real ARM flags (os/Makefile,
per-file additions such as -marm -O2 included) and -fstack-usage
-fcallgraph-info=su: GCC writes each function's frame (.su) and its direct
calls (.ci). Then:

  - the largest frames, overall and per file;
  - for each thread (found by grepping thread_create / thread_create_owned
    calls: name, stack size, entry function) the deepest chain of direct
    calls from its entry, with its bytes: the frames add up along the chain
    (GCC's frame is everything the function pushes, its inlined callees
    included);
  - the same for each interrupt handler (irq_register calls) on top of
    irq_dispatch, next to the IRQ stack's size (link/os.ld).

What it cannot see is flagged on each line: an indirect call (a function
pointer: the callee and its stack are unknown; such threads' real worst case
can be deeper), recursion (a cycle, counted once), a frame GCC calls
dynamic (alloca, a variable-length array), and callees without a frame
(assembler, libgcc: counted as 0). The context-switch frame the kernel saves
on a thread's stack (arch/sc6530c/cpu.S) is not counted either.

Files that do not compile at the moment are listed and left out (another
change may be half done); their functions then show as unknown callees.

  python os/tests/stack-audit.py               # the report
  python os/tests/stack-audit.py --top 30 --path   # more frames, each chain in full
  python os/tests/stack-audit.py --function ui_run # the deepest chain from one function

Needs arm-none-eabi-gcc 10 or later (-fcallgraph-info); B310E_TOOLCHAIN or
PATH, else D:/Toolchains/arm-none-eabi/bin.

SPDX-License-Identifier: GPL-2.0-or-later
"""
import argparse
import concurrent.futures
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

REPO = Path(__file__).resolve().parents[2]
OS = REPO / 'os'
FATFS = REPO / 'build' / 'fatfs' / 'source'


def find_gcc():
    for d in (os.environ.get('B310E_TOOLCHAIN'), None, r'D:\Toolchains\arm-none-eabi\bin'):
        if d is None:
            p = shutil.which('arm-none-eabi-gcc')
            if p:
                return Path(p)
            continue
        if d:
            for name in ('arm-none-eabi-gcc.exe', 'arm-none-eabi-gcc'):
                if (Path(d) / name).exists():
                    return Path(d) / name
    sys.exit('stack-audit: no arm-none-eabi-gcc (set B310E_TOOLCHAIN)')


def makefile():
    """The Makefile's flags, sources and per-object additions."""
    text = (OS / 'Makefile').read_text(encoding='utf-8')
    text = re.sub(r'\\\n', ' ', text)
    var = {}
    for m in re.finditer(r'^([A-Z_]+)\s*:?=\s*(.*)$', text, re.M):
        var.setdefault(m.group(1), m.group(2).strip())

    def expand(s):
        for _ in range(5):
            s = re.sub(r'\$\(([A-Z_]+)\)', lambda m: var.get(m.group(1), ''), s)
        return s
    arch = expand(var.get('ARCH_FLAGS', '-march=armv5te -mtune=arm926ej-s -mthumb-interwork')).split()
    cflags = expand(var.get('CFLAGS', '-mthumb -Os -std=gnu11 -ffreestanding')).split()
    srcs = []
    m = re.search(r'^SRCS_C\s*:=\s*(.*)$', text, re.M)
    words = m.group(1).replace('$(wildcard', ' ').replace(')', ' ').split() if m else []
    if not words:
        words = ('arch/sc6530c/*.c kernel/*.c drivers/*.c fs/*.c lib/*.c loader/*.c ui/*.c '
                 'apps/*.c net/*.c media/*.c main.c selftest.c').split()
    for w in words:
        srcs += sorted(p.relative_to(OS).as_posix() for p in OS.glob(w))
    extra = {}
    for m in re.finditer(r'^((?:\$\(BUILD\)/\S+\.o\s*)+):\s*CFLAGS\s*\+=\s*(.*)$', text, re.M):
        for obj in m.group(1).split():
            extra[obj.replace('$(BUILD)/', '').replace('.o', '.c')] = m.group(2).split()
    # the include folders are the build's own; nothing is written there
    keep = [f for f in cflags if not f.startswith('-I') and f not in ('-g', '-Wall', '-Wextra')
            and not f.startswith('-W')]
    return keep, srcs, extra


def compile_one(gcc, flags, src, out, cwd):
    obj = out / (src.replace('/', '__').replace('..', 'up') + '.o')
    inc = ['-I.', '-Iinclude', '-Ifs', f'-I{FATFS.as_posix()}', f'-I{(REPO / "build" / "os").as_posix()}']
    cmd = [str(gcc), *flags, *inc, '-w', '-fstack-usage', '-fcallgraph-info=su', '-c', src,
           '-o', str(obj)]
    r = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
    if r.returncode:
        return src, None, r.stderr.strip().splitlines()[:3]
    return src, obj.with_suffix('.ci'), None


NODE = re.compile(r'node: \{ title: "([^"]+)" label: "([^"]*)"')
EDGE = re.compile(r'edge: \{ sourcename: "([^"]+)" targetname: "([^"]+)"')


def parse(ci_files):
    frames, kind, where, calls = {}, {}, {}, {}
    for ci in ci_files:
        try:
            text = ci.read_text(encoding='utf-8', errors='replace')
        except OSError:
            continue
        for m in NODE.finditer(text):
            title, label = m.group(1), m.group(2).replace('\\n', '\n')
            lines = label.split('\n')
            sm = re.search(r'(\d+) bytes \(([^)]*)\)', label)
            if sm:
                frames[title] = int(sm.group(1))
                kind[title] = sm.group(2)
                where[title] = lines[1] if len(lines) > 1 else ''
        for m in EDGE.finditer(text):
            calls.setdefault(m.group(1), []).append(m.group(2))
    return frames, kind, where, calls


def resolver(frames):
    """A callee's node: the static one of the caller's file first, then the global one."""
    def resolve(caller, name):
        if ':' in name or name == '__indirect_call':
            return name
        if ':' in caller:
            local = caller.rsplit(':', 1)[0] + ':' + name
            if local in frames:
                return local
        return name
    return resolve


class Depth:
    def __init__(self, frames, kind, calls):
        self.frames, self.kind, self.calls = frames, kind, calls
        self.memo = {}
        self.resolve = resolver(frames)

    def worst(self, fn, stack=()):
        """(bytes, chain, flags) of the deepest direct-call chain from fn"""
        if fn in self.memo:
            return self.memo[fn]
        if fn in stack:
            return 0, [fn + ' (recursion)'], {'recursion:' + fn}
        own = self.frames.get(fn)
        flags = set()
        if own is None:
            flags.add('unknown:' + fn)
            own = 0
        elif 'dynamic' in self.kind.get(fn, ''):
            flags.add('dynamic:' + fn)
        best, chain = 0, []
        for callee in self.calls.get(fn, []):
            if callee == '__indirect_call':
                flags.add('indirect:' + fn)
                continue
            callee = self.resolve(fn, callee)
            b, c, f = self.worst(callee, stack + (fn,))
            flags |= f
            if b > best or not chain:
                best, chain = b, c
        result = (own + best, [fn] + chain, flags)
        if not any(f.startswith('recursion:') for f in flags):
            self.memo[fn] = result
        return result


CREATE = re.compile(r'thread_create(?:_owned)?\s*\(\s*("(?:[^"\\]|\\.)*"|[^,]+),\s*([^,]+),\s*([^,]+),\s*'
                    r'([A-Za-z_]\w*)\s*,')
IRQ = re.compile(r'irq_register\s*\(\s*([^,]+),\s*([A-Za-z_]\w*)\s*\)')
DEFINE = re.compile(r'^\s*#\s*define\s+(\w+)[ \t]+([^\n]*?)\s*(?:/\*.*)?$', re.M)


def value(expr, text):
    """A stack size: a number, or a #define of numbers and + - * / ( ) in the same file."""
    defs = {m.group(1): m.group(2) for m in DEFINE.finditer(text)}

    def subst(s, depth=0):
        s = re.sub(r'\b(\d+)[uUlL]+\b', r'\1', s)
        if depth > 5:
            return s
        return re.sub(r'\b([A-Za-z_]\w*)\b',
                      lambda m: '(' + subst(defs[m.group(1)], depth + 1) + ')' if m.group(1) in defs
                      else m.group(1), s)
    s = subst(expr.strip())
    if not re.fullmatch(r'[\d\s+\-*/()]+', s):
        return None
    try:
        return int(eval(s.replace('/', '//'), {'__builtins__': {}}))
    except Exception:
        return None


def threads(srcs):
    found = []
    for src in srcs:
        path = OS / src
        try:
            text = path.read_text(encoding='utf-8', errors='replace')
        except OSError:
            continue
        for m in CREATE.finditer(text):
            if not m.group(1).strip().startswith('"') and m.group(4) in ('fn', 'func'):
                continue                # a wrapper passing its own argument on (app_thread ...)
            line = text.count('\n', 0, m.start()) + 1
            found.append((m.group(1).strip(), m.group(3).strip(), value(m.group(3), text),
                          m.group(4), src, line))
    return found


def irq_handlers(srcs):
    found = []
    for src in srcs:
        text = (OS / src).read_text(encoding='utf-8', errors='replace')
        for m in IRQ.finditer(text):
            if m.group(2) == 'handler':
                continue
            found.append((m.group(1).strip(), m.group(2), src))
    return found


def irq_stack():
    try:
        text = (OS / 'link' / 'os.ld').read_text(encoding='utf-8')
    except OSError:
        return None
    m = re.search(r'\.\s*\+=\s*(\d+)\s*;\s*__irq_stack_top', text)
    return int(m.group(1)) if m else None


ASM = set()                            # global labels of the .S files (their frames unknown)


def asm_symbols():
    for s in OS.glob('arch/**/*.S'):
        text = s.read_text(encoding='utf-8', errors='replace')
        for names in re.findall(r'^\s*\.glob(?:al|l)\s+([\w\s,]+)$', text, re.M):
            ASM.update(n.strip() for n in names.split(',') if n.strip())


def listed(names, n=3):
    names = sorted(short(x) for x in names)
    return ', '.join(names[:n]) + (' ...' if len(names) > n else '')


def flag_text(flags):
    ind = [f.split(':', 1)[1] for f in flags if f.startswith('indirect:')]
    rec = [f.split(':', 1)[1] for f in flags if f.startswith('recursion:')]
    unk = [f.split(':', 1)[1] for f in flags if f.startswith('unknown:')]
    dyn = [f.split(':', 1)[1] for f in flags if f.startswith('dynamic:')]
    lib = [u for u in unk if u.startswith('__')]
    asm = [u for u in unk if u in ASM]
    other = [u for u in unk if u not in lib and u not in asm]
    out = []
    if ind:
        out.append(f'indirect calls in {len(ind)}: {listed(ind)}')
    if rec:
        out.append(f'recursion through {listed(rec)}')
    if dyn:
        out.append(f'dynamic frame: {listed(dyn)}')
    if other:
        out.append(f'no frame known for {listed(other)}')
    if asm or lib:
        out.append(f'{len(asm)} assembler, {len(lib)} libgcc callees counted 0')
    return '; '.join(out)


def short(fn):
    return fn.rsplit(':', 1)[1] if ':' in fn else fn


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--out', type=Path, default=Path(tempfile.gettempdir()) / 'b310e-stack-audit',
                    help='scratch folder for the objects (default: %(default)s)')
    ap.add_argument('--top', type=int, default=20, help='largest frames shown (default 20)')
    ap.add_argument('--path', action='store_true', help='each worst chain in full')
    ap.add_argument('--function', action='append', default=[], help='also the worst chain from this function')
    args = ap.parse_args()
    gcc = find_gcc()
    flags, srcs, extra = makefile()
    out = args.out.resolve()
    if out.exists():
        shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True, exist_ok=True)
    units = [(s, flags + extra.get(s, [])) for s in srcs]
    for f in ('ff.c', 'ffunicode.c'):
        if (FATFS / f).exists():
            units.append((os.path.relpath(FATFS / f, OS).replace('\\', '/'), flags))
    cis, broken = [], []
    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
        for src, ci, err in pool.map(lambda u: compile_one(gcc, u[1], u[0], out, OS), units):
            if ci is None:
                broken.append((src, err))
            else:
                cis.append(ci)
    frames, kind, where, calls = parse(cis)
    asm_symbols()
    print(f'stack-audit: {len(cis)} of {len(units)} files compiled with {gcc.name} '
          f'({" ".join(flags[:6])} ...), {len(frames)} functions')
    for src, err in broken:
        print(f'  NOT COMPILED (left out): {src}: {err[0] if err else ""}')

    print(f'\nThe {args.top} largest frames:')
    for fn in sorted(frames, key=lambda f: -frames[f])[:args.top]:
        print(f'  {frames[fn]:6d}  {short(fn):32s} {where.get(fn, "")}  {kind.get(fn, "")}')
    per_file = {}
    for fn, n in frames.items():
        f = where.get(fn, '').split(':')[0]
        if n > per_file.get(f, (0, ''))[0]:
            per_file[f] = (n, fn)
    print('\nThe largest frame of each file (256 bytes or more):')
    for f, (n, fn) in sorted(per_file.items(), key=lambda kv: -kv[1][0]):
        if n >= 256:
            print(f'  {n:6d}  {f:28s} {short(fn)}')

    depth = Depth(frames, kind, calls)
    print('\nThreads (stack: given; worst: the deepest direct-call chain; what it cannot see on the right):')
    print(f'  {"thread":12s} {"entry":22s} {"stack":>6s} {"worst":>6s} {"left":>6s}  where')
    rows = []
    for name, size_text, size, entry, src, line in threads(srcs):
        node = entry if entry in frames else f'{src}:{entry}' if f'{src}:{entry}' in frames else entry
        b, chain, fl = depth.worst(node)
        rows.append((name, entry, size_text, size, b, chain, fl, src, line))
    for name, entry, size_text, size, b, chain, fl, src, line in rows:
        left = f'{size - b:6d}' if size is not None else '     ?'
        warn = '  <-- OVER' if size is not None and b > size else \
            '  <-- under 25% left' if size is not None and b > size * 3 // 4 else ''
        print(f'  {name.strip(chr(34))[:12]:12s} {entry[:22]:22s} {size if size is not None else size_text:>6} '
              f'{b:6d} {left}  {src}:{line}{warn}')
        ft = flag_text(fl)
        if ft:
            print(f'  {"":12s} {"":22s} {"":6s} {"":6s} {"":6s}  ({ft})')
        if args.path:
            print('      ' + ' > '.join(f'{short(c)} {frames.get(c, 0)}' for c in chain))
    isz = irq_stack()
    hs = irq_handlers(srcs)
    if hs:
        base = depth.worst('irq_dispatch')
        print(f'\nInterrupt handlers (on the IRQ stack, {isz if isz else "?"} bytes; irq_dispatch '
              f'{base[0]} bytes on its own, its handler call is indirect):')
        for line_name, handler, src in hs:
            node = handler if handler in frames else f'{src}:{handler}'
            b, chain, fl = depth.worst(node)
            total = frames.get('irq_dispatch', 0) + b
            warn = '  <-- OVER' if isz and total > isz else ''
            print(f'  {handler[:22]:22s} line {line_name[:10]:10s} {total:6d}  {src}{warn}')
            ft = flag_text(fl)
            if ft:
                print(f'  {"":22s} {"":15s} ({ft})')
            if args.path:
                print('      irq_dispatch > ' + ' > '.join(f'{short(c)} {frames.get(c, 0)}' for c in chain))
    for fn in args.function:
        node = fn if fn in frames else next((f for f in frames if short(f) == fn), fn)
        b, chain, fl = depth.worst(node)
        print(f'\n{fn}: {b} bytes: ' + ' > '.join(f'{short(c)} {frames.get(c, 0)}' for c in chain))
        ft = flag_text(fl)
        if ft:
            print(f'  ({ft})')
    print(f'\n(objects and .su/.ci files in {out})')
    return 0


if __name__ == '__main__':
    sys.exit(main())
