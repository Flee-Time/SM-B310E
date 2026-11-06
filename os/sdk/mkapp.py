#!/usr/bin/env python3
"""Turn an app ELF (linked by os/sdk/app.ld with -q) into a B310E OS .app.

The .app is a 48-byte header, the image (address 0 up to __image_end)
and the offsets of the words the loader must relocate: R_ARM_ABS32 (and
R_ARM_TARGET1) against symbols inside the image. PC-relative relocations
stay valid wherever the image lands; any other kind is refused, as are
undefined symbols. See os/include/os/app.h.

SPDX-License-Identifier: GPL-2.0-or-later
"""
import argparse
from pathlib import Path
import struct
import sys
import zlib

APP_MAGIC = 0x50413342
HEADER = struct.Struct('<IHHIIIII20s')
SHT_SYMTAB, SHT_NOBITS, SHT_REL, SHT_RELA = 2, 8, 9, 4
SHF_ALLOC = 2
SHN_UNDEF, SHN_ABS = 0, 0xfff1
ABSOLUTE = {2, 38}                      # R_ARM_ABS32, R_ARM_TARGET1
RELATIVE = {0, 3, 10, 11, 28, 29, 30, 40, 42, 102, 103}
# NONE, REL32, THM_CALL, THM_PC8, CALL, JUMP24, THM_JUMP24, V4BX, PREL31,
# THM_JUMP11, THM_JUMP8


def fail(message):
    sys.exit(f'mkapp: {message}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('elf', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    elf = args.elf.read_bytes()
    if elf[:4] != b'\x7fELF' or elf[4] != 1 or elf[5] != 1 or \
            struct.unpack_from('<H', elf, 18)[0] != 40:
        fail('not a 32-bit little-endian ARM ELF')
    shoff, = struct.unpack_from('<I', elf, 32)
    shentsize, shnum, shstrndx = struct.unpack_from('<HHH', elf, 46)
    sections = []
    for i in range(shnum):
        name, kind, flags, addr, offset, size, link, info, _, entsize = \
            struct.unpack_from('<10I', elf, shoff + i * shentsize)
        sections.append(dict(name=name, kind=kind, flags=flags, addr=addr,
                             offset=offset, size=size, link=link, info=info,
                             entsize=entsize))
    strtab = sections[shstrndx]
    for s in sections:
        end = elf.index(b'\0', strtab['offset'] + s['name'])
        s['name'] = elf[strtab['offset'] + s['name']:end].decode()

    symtab = next((s for s in sections if s['kind'] == SHT_SYMTAB), None)
    if not symtab:
        fail('no symbol table')
    names = sections[symtab['link']]
    symbols = []
    for i in range(symtab['size'] // 16):
        name, value, size, info, other, shndx = \
            struct.unpack_from('<IIIBBH', elf, symtab['offset'] + i * 16)
        end = elf.index(b'\0', names['offset'] + name)
        symbols.append((elf[names['offset'] + name:end].decode(), value, info, shndx))
    by_name = {s[0]: s for s in symbols if s[0]}
    undefined = [s[0] for s in symbols[1:] if s[3] == SHN_UNDEF and s[0] and s[2] >> 4 != 2]
    if undefined:
        fail('undefined symbols: ' + ', '.join(sorted(set(undefined))))
    for need in ('app_info', '__image_end', '__bss_end'):
        if need not in by_name:
            fail(f'missing symbol {need} (link with os/sdk/app.ld)')
    if by_name['app_info'][1] != 0:
        fail('app_info is not at offset 0 (use the APP() macro)')
    image_size, bss_end = by_name['__image_end'][1], by_name['__bss_end'][1]

    image = bytearray(image_size)
    for s in sections:
        if s['flags'] & SHF_ALLOC and s['size']:
            if s['kind'] == SHT_NOBITS:
                if s['addr'] < image_size:
                    fail(f'{s["name"]}: bss inside the image')
                continue
            if s['addr'] + s['size'] > image_size:
                fail(f'{s["name"]}: outside the image')
            image[s['addr']:s['addr'] + s['size']] = elf[s['offset']:s['offset'] + s['size']]

    relocs = set()
    for s in sections:
        if s['kind'] == SHT_RELA:
            fail(f'{s["name"]}: RELA relocations are not supported')
        if s['kind'] != SHT_REL:
            continue
        target = sections[s['info']]
        if not target['flags'] & SHF_ALLOC:
            continue                    # debug information
        if target['kind'] == SHT_NOBITS:
            fail(f'{s["name"]}: relocations in bss')
        for i in range(s['size'] // 8):
            offset, info = struct.unpack_from('<II', elf, s['offset'] + i * 8)
            kind, sym = info & 0xff, info >> 8
            if kind in RELATIVE:
                continue
            if kind not in ABSOLUTE:
                fail(f'{s["name"]}+{offset:#x}: relocation type {kind} is not supported')
            if symbols[sym][3] == SHN_ABS:
                continue                # an absolute value, not an address
            if offset & 3 or offset + 4 > image_size:
                fail(f'{s["name"]}+{offset:#x}: misplaced relocation')
            value, = struct.unpack_from('<I', image, offset)
            if value > bss_end:
                fail(f'{s["name"]}+{offset:#x}: points outside the app ({value:#x})')
            relocs.add(offset)

    major, minor, name_ptr = struct.unpack_from('<HHI', image, 0)
    if name_ptr >= image_size:
        fail('app_info.name is not in the image')
    name = bytes(image[name_ptr:image.index(b'\0', name_ptr)])
    if len(name) >= 20:
        fail(f'app name {name!r} is longer than 19 bytes')
    reloc_bytes = b''.join(struct.pack('<I', r) for r in sorted(relocs))
    crc = zlib.crc32(bytes(image) + reloc_bytes) & 0xffffffff
    header = HEADER.pack(APP_MAGIC, major, minor, HEADER.size, image_size,
                         bss_end - image_size, len(relocs), crc, name)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(header + bytes(image) + reloc_bytes)
    print(f'{args.output.name}: "{name.decode()}" API {major}.{minor}, '
          f'{image_size} + {bss_end - image_size} bytes, {len(relocs)} relocations')


if __name__ == '__main__':
    main()
