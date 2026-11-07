#!/usr/bin/env python3
"""B310E boot loader images.

  mkboot.py image RAW --default stock|menu|program[:PATH] --output BIN
      set the header's default (no key, no request) and its CRC-32
  mkboot.py nor --dump NOR8M --loader BIN --output NOR
      an emulator NOR: a copy of your own NOR dump with the loader at
      0x3fc000 and the boot vector word 0x20 = 0x3fc000 (never for the
      phone, and never to be shared: it holds the stock firmware)
  mkboot.py verify --flash READBACK --loader BIN
      check a NOR read-back after the install (word 0x20, the loader)
  mkboot.py backup-check --backup READ [--reference DUMP] [--slice OUT]
      check a full NOR read before any NOR write: 8 MB (or the 16 MB
      read that mirrors it), not one block repeated, the vectors, what is
      at 0x3fc000; optionally against an older dump; --slice writes the
      8 MB image a full restore takes
  mkboot.py info BIN

Header (start.S): +0x04 stock entry, +0x08 "B310BOOT", +0x10 size,
+0x14 version, +0x18 default mode, +0x20 default program (48 bytes),
+0x50 CRC-32 of the image with this word zero.

SPDX-License-Identifier: GPL-2.0-or-later
"""
import argparse
from pathlib import Path
import struct
import sys
import zlib

NOR_AT = 0x3fc000
NOR_MAX = 0x10000              # boot.ld
NOR_SIZE = 0x800000
VECTOR_WORD = 0x20
STOCK_ENTRY = 0x46e4
MODES = {'stock': 0, 'menu': 1, 'program': 2}
PATH_AT, PATH_LEN, CRC_AT = 0x20, 48, 0x50
SECTOR = 0x1000


def crc(image):
    data = bytearray(image)
    data[CRC_AT:CRC_AT + 4] = bytes(4)
    return zlib.crc32(data) & 0xffffffff


def check(image, name):
    if len(image) < 0x60 or image[8:16] != b'B310BOOT':
        sys.exit(f'{name}: not a B310E boot loader image')
    size = struct.unpack_from('<I', image, 0x10)[0]
    if size != len(image):
        sys.exit(f'{name}: header size {size:#x}, file {len(image):#x}')
    if len(image) > NOR_MAX:
        sys.exit(f'{name}: {len(image)} bytes, more than {NOR_MAX}')
    if struct.unpack_from('<I', image, 0)[0] >> 24 != 0xea:
        sys.exit(f'{name}: word 0 is not an ARM branch')


def describe(image):
    entry, = struct.unpack_from('<I', image, 4)
    version, mode = struct.unpack_from('<II', image, 0x14)
    path = image[PATH_AT:PATH_AT + PATH_LEN].split(b'\0')[0].decode()
    stored, = struct.unpack_from('<I', image, CRC_AT)
    names = {v: k for k, v in MODES.items()}
    return (f'{len(image)} bytes ({(len(image) + SECTOR - 1) // SECTOR} NOR sectors '
            f'from {NOR_AT:#x}), version {version >> 16}.{version & 0xffff}, '
            f'stock entry {entry:#x}, default {names.get(mode, mode)}'
            + (f' ({path})' if mode == MODES['program'] else '')
            + f', CRC-32 {stored:08x}' + ('' if stored == crc(image) else ' (WRONG)'))


def cmd_image(args):
    image = bytearray(args.raw.read_bytes())
    check(image, args.raw)
    mode, _, path = args.default.partition(':')
    if mode not in MODES:
        sys.exit(f'--default: {mode}? (stock, menu, program[:path])')
    struct.pack_into('<I', image, 0x18, MODES[mode])
    if path:
        raw = path.encode('ascii')
        if len(raw) >= PATH_LEN:
            sys.exit(f'--default: path longer than {PATH_LEN - 1} bytes')
        image[PATH_AT:PATH_AT + PATH_LEN] = raw.ljust(PATH_LEN, b'\0')
    if args.stock_entry is not None:
        struct.pack_into('<I', image, 4, args.stock_entry)
    struct.pack_into('<I', image, CRC_AT, crc(image))
    args.output.write_bytes(image)
    print(f'{args.output}: {describe(image)}')


def cmd_nor(args):
    nor = bytearray(args.dump.read_bytes()[:NOR_SIZE])
    image = args.loader.read_bytes()
    check(image, args.loader)
    if len(nor) != NOR_SIZE:
        sys.exit(f'{args.dump}: {len(nor)} bytes, not an 8 MB NOR dump')
    vector, = struct.unpack_from('<I', nor, VECTOR_WORD)
    if vector not in (STOCK_ENTRY, NOR_AT):
        sys.exit(f'{args.dump}: boot vector {vector:#x}, expected {STOCK_ENTRY:#x} or {NOR_AT:#x}')
    end = NOR_AT + (len(image) + SECTOR - 1) // SECTOR * SECTOR
    old = nor[NOR_AT:end]
    if vector == STOCK_ENTRY and old != b'\xff' * len(old):
        sys.exit(f'{args.dump}: {NOR_AT:#x}..{end:#x} is not erased')
    nor[NOR_AT:end] = image.ljust(end - NOR_AT, b'\xff')
    struct.pack_into('<I', nor, VECTOR_WORD, NOR_AT)
    args.output.write_bytes(nor)
    print(f'{args.output}: emulator NOR, loader at {NOR_AT:#x} ({len(image)} bytes)')


def cmd_verify(args):
    flash = args.flash.read_bytes()
    image = args.loader.read_bytes()
    check(image, args.loader)
    ok = True
    vector, = struct.unpack_from('<I', flash, VECTOR_WORD)
    print(f'boot vector (word {VECTOR_WORD:#x}) = {vector:#x}', '(ok)' if vector == NOR_AT else '(NOT the loader)')
    ok &= vector == NOR_AT
    got = flash[NOR_AT:NOR_AT + len(image)]
    same = got == image
    print(f'loader at {NOR_AT:#x}: {"matches" if same else "DIFFERS"} ({len(image)} bytes)')
    ok &= same
    end = NOR_AT + (len(image) + SECTOR - 1) // SECTOR * SECTOR
    tail = flash[NOR_AT + len(image):end]
    print(f'rest of the last sector: {"erased" if tail == b"\xff" * len(tail) else "NOT erased"}')
    sys.exit(0 if ok else 1)


FIRMWARE_END = 0x690000        # code, resources, DSP, NV; the FS follows


def loader_at(nor):
    block = nor[NOR_AT:NOR_AT + 0x10]
    if block == b'\xff' * 0x10:
        return 'erased'
    if block[8:16] == b'SDLOADER':
        size, = struct.unpack_from('<I', nor, NOR_AT + 0x10)
        return f'fpdoom sdboot ({size} bytes)'
    if block[8:16] == b'B310BOOT':
        return 'the B310E boot loader (' + describe(bytes(nor[NOR_AT:NOR_AT + struct.unpack_from('<I', nor, NOR_AT + 0x10)[0]])) + ')'
    return 'unknown data'


def cmd_backup_check(args):
    data = args.backup.read_bytes()
    bad = []
    if len(data) == 2 * NOR_SIZE:
        lo, hi = data[:NOR_SIZE], data[NOR_SIZE:]
        diff = [a for a in range(0, NOR_SIZE, SECTOR) if lo[a:a + SECTOR] != hi[a:a + SECTOR]]
        print(f'16 MB read: the halves differ in {len(diff)} sectors'
              + (f' (first {diff[0]:#x}, last {diff[-1]:#x})' if diff else ''))
        if any(a < FIRMWARE_END for a in diff):
            bad.append('the halves differ below the file system: an unstable read')
        data = lo
    elif len(data) != NOR_SIZE:
        sys.exit(f'{args.backup}: {len(data)} bytes; expected 8 or 16 MB')
    blocks = {data[a:a + SECTOR] for a in range(0, NOR_SIZE, SECTOR)}
    print(f'{len(blocks)} different 4 KB blocks')
    if len(blocks) < 64:
        bad.append('a few blocks repeated: the FDL read one block for every address')
    v0, = struct.unpack_from('<I', data, 0)
    vector, = struct.unpack_from('<I', data, VECTOR_WORD)
    print(f'word 0 = {v0:#010x}' + ('' if v0 == 0xe59ff018 else ' (NOT the stock vector table)'))
    if v0 != 0xe59ff018:
        bad.append('no vector table at 0')
    print(f'boot vector word {VECTOR_WORD:#x} = {vector:#x} ('
          + {STOCK_ENTRY: 'stock', NOR_AT: 'a loader at 0x3fc000'}.get(vector, 'UNKNOWN') + ')')
    if vector not in (STOCK_ENTRY, NOR_AT):
        bad.append('unknown boot vector')
    print(f'at {NOR_AT:#x}: {loader_at(data)}')
    tail = data[NOR_AT + 0x10000:NOR_AT + 0xa4000]
    print(f'{NOR_AT + 0x10000:#x}..{NOR_AT + 0xa4000:#x}: '
          + ('erased' if tail == b'\xff' * len(tail) else 'NOT erased'))
    if args.reference:
        ref = args.reference.read_bytes()[:NOR_SIZE]
        skip = range(NOR_AT, NOR_AT + 0x10000)
        diff = [a for a in range(0, FIRMWARE_END, SECTOR)
                if a not in skip and (data[a:a + SECTOR] != ref[a:a + SECTOR])]
        # the vector word 0x20 changes with the loader install
        if diff and diff[0] == 0:
            a, b = bytearray(data[:SECTOR]), bytearray(ref[:SECTOR])
            a[VECTOR_WORD:VECTOR_WORD + 4] = b[VECTOR_WORD:VECTOR_WORD + 4]
            if a == b:
                diff = diff[1:]
        print(f'against {args.reference.name}: {len(diff)} firmware sectors differ'
              + (f' (first {diff[0]:#x})' if diff else ''))
    if args.slice:
        args.slice.write_bytes(data)
        print(f'{args.slice}: the 8 MB restore image')
    if bad:
        print('NOT A GOOD BACKUP: ' + '; '.join(bad))
        sys.exit(1)
    print('looks like a good backup')


def cmd_info(args):
    image = args.image.read_bytes()
    check(image, args.image)
    print(f'{args.image}: {describe(image)}')


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('image')
    p.add_argument('raw', type=Path)
    p.add_argument('--default', default='stock')
    p.add_argument('--stock-entry', type=lambda s: int(s, 0))
    p.add_argument('--output', type=Path, required=True)
    p.set_defaults(fn=cmd_image)
    p = sub.add_parser('nor')
    p.add_argument('--dump', type=Path, required=True)
    p.add_argument('--loader', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.set_defaults(fn=cmd_nor)
    p = sub.add_parser('verify')
    p.add_argument('--flash', type=Path, required=True)
    p.add_argument('--loader', type=Path, required=True)
    p.set_defaults(fn=cmd_verify)
    p = sub.add_parser('backup-check')
    p.add_argument('--backup', type=Path, required=True)
    p.add_argument('--reference', type=Path)
    p.add_argument('--slice', type=Path)
    p.set_defaults(fn=cmd_backup_check)
    p = sub.add_parser('info')
    p.add_argument('image', type=Path)
    p.set_defaults(fn=cmd_info)
    args = parser.parse_args()
    args.fn(args)


if __name__ == '__main__':
    main()
