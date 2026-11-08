#!/usr/bin/env python3
"""The CRCs the loader logs when an .app (or .cdc) fails its CRC: CRC-32
of each 64 KB of the image and of the relocations, as in the file. Lines
of the card's log that differ from these show where the phone read it
wrong (os/loader/loader.c, crc_report).

  python os/sdk/appcrc.py sdcard/b310e-os/apps/doom.app

SPDX-License-Identifier: GPL-2.0-or-later
"""
import argparse
from pathlib import Path
import struct
import zlib

HEADER = struct.Struct('<IHHIIIII20s')
PART = 64 * 1024


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('app', type=Path)
    args = parser.parse_args()
    data = args.app.read_bytes()
    (magic, major, minor, header_size, image_size, bss_size, relocs, crc,
     name) = HEADER.unpack_from(data)
    name = name.split(b'\0')[0].decode(errors='replace')
    image = data[header_size:header_size + image_size]
    reloc = data[header_size + image_size:]
    whole = zlib.crc32(image + reloc)
    print(f'{name}: {image_size} + {bss_size} bytes, {relocs} relocations, '
          f'CRC {whole:08x} (header {crc:08x}{"" if whole == crc else ", DAMAGED"})')
    for off in range(0, image_size, PART):
        print(f'{off // 1024:4} KB {zlib.crc32(image[off:off + PART]):08x}')
    print(f'relocs  {zlib.crc32(reloc):08x}')


if __name__ == '__main__':
    main()
