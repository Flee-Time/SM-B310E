#!/usr/bin/env python3
"""Create a partitioned FAT16 SD image with .rockbox and a stereo test WAV.

Uses only Python's standard library. The output is a generated emulator
image, never a physical disk. Run the port build first to stage .rockbox.
"""
import argparse
import io
import json
import math
from pathlib import Path
import struct
import wave

SECTOR = 512
START = 2048
ROOT_ENTRIES = 512
SPC = 4


def test_audio(seconds):
    rate = 44100
    frames = int(seconds * rate)
    pcm = bytearray(frames * 4)
    for i in range(frames):
        gain = min(1, i / 441, (frames - 1 - i) / 441) * 4096
        struct.pack_into('<hh', pcm, i * 4,
                         round(gain * math.sin(2 * math.pi * 440 * i / rate)),
                         round(gain * math.sin(2 * math.pi * 660 * i / rate)))
    stream = io.BytesIO()
    with wave.open(stream, 'wb') as wav:
        wav.setnchannels(2)
        wav.setsampwidth(2)
        wav.setframerate(rate)
        wav.writeframes(pcm)
    return stream.getvalue()


def directory():
    return {'children': {}, 'data': None, 'cluster': 0}


def add_file(root, path, data):
    parts = Path(path).parts
    node = root
    for part in parts[:-1]:
        node = node['children'].setdefault(part, directory())
    node['children'][parts[-1]] = {'children': None, 'data': data, 'cluster': 0}


def short_name(name, used):
    def clean(text):
        return ''.join(c for c in text.upper() if c.isascii() and c.isalnum())
    base, _, ext = name.rpartition('.')
    if not base:
        base, ext = name, ''
    stem, ext = clean(base) or 'FILE', clean(ext)[:3]
    for n in range(1, 100000):
        suffix = '~' + str(n)
        alias = (stem[:8-len(suffix)] + suffix).ljust(8) + ext.ljust(3)
        if alias not in used:
            used.add(alias)
            return alias.encode('ascii')
    raise ValueError('too many colliding names')


def entry(alias, node):
    data = bytearray(32)
    data[:11] = alias
    data[11] = 0x10 if node['children'] is not None else 0x20
    struct.pack_into('<H', data, 26, node['cluster'])
    struct.pack_into('<I', data, 28, len(node['data']) if node['data'] is not None else 0)
    return data


def long_entries(name, alias):
    checksum = 0
    for b in alias:
        checksum = (((checksum & 1) << 7) | (checksum >> 1)) + b
        checksum &= 255
    units = list(struct.unpack('<' + 'H' * (len(name.encode('utf-16le')) // 2),
                               name.encode('utf-16le')))
    if len(units) > 255:
        raise ValueError('FAT filename too long: ' + name)
    units.append(0)
    units += [0xffff] * ((-len(units)) % 13)
    result = bytearray()
    slots = [1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30]
    count = len(units) // 13
    for ordinal in range(count, 0, -1):
        data = bytearray(32)
        data[0] = ordinal | (0x40 if ordinal == count else 0)
        data[11], data[13] = 0x0f, checksum
        for offset, value in zip(slots, units[(ordinal-1)*13:ordinal*13]):
            struct.pack_into('<H', data, offset, value)
        result += data
    return result


def write_image(path, root, size_mib):
    total = size_mib * 1024 * 1024 // SECTOR
    partition = total - START
    root_sectors = ROOT_ENTRIES * 32 // SECTOR
    fat_sectors = 1
    while True:
        clusters = (partition - 1 - 2 * fat_sectors - root_sectors) // SPC
        needed = (2 * (clusters + 2) + SECTOR - 1) // SECTOR
        if needed <= fat_sectors:
            break
        fat_sectors = needed
    if not 4085 <= clusters < 65525:
        raise ValueError('image size is outside this FAT16 layout')
    image = bytearray(total * SECTOR)
    fat = bytearray(fat_sectors * SECTOR)
    struct.pack_into('<HH', fat, 0, 0xfff8, 0xffff)
    data_sector = START + 1 + 2 * fat_sectors + root_sectors
    next_cluster = 2
    file_count = 0

    def allocate(node, parent_cluster=0):
        nonlocal next_cluster, file_count
        children = node['children']
        if children is None:
            payload = node['data']
            file_count += 1
        else:
            used = set()
            for name, child in sorted(children.items()):
                child['alias'] = short_name(name, used)
            # Reserve the directory first so children's '..' can refer to it.
            length = 64 + sum(len(long_entries(name, child['alias'])) + 32
                              for name, child in children.items()) + 32
            payload = bytes(length)
        count = max(1, (len(payload) + SPC * SECTOR - 1) // (SPC * SECTOR))
        first = next_cluster
        node['cluster'] = first
        next_cluster += count
        if next_cluster > clusters + 2:
            raise ValueError('runtime and test audio do not fit in image')
        for c in range(first, first + count):
            struct.pack_into('<H', fat, c * 2, c + 1 if c + 1 < first + count else 0xffff)
        if children is not None:
            payload = entry(b'.          ', node)
            payload += entry(b'..         ', {'children': {}, 'data': None, 'cluster': parent_cluster})
            for name, child in sorted(children.items()):
                allocate(child, first)
                payload += long_entries(name, child['alias']) + entry(child['alias'], child)
            payload += bytes(32)
        offset = (data_sector + (first - 2) * SPC) * SECTOR
        image[offset:offset + len(payload)] = payload

    used = set()
    root_data = bytearray()
    for name, node in sorted(root['children'].items()):
        alias = short_name(name, used)
        allocate(node)
        root_data += long_entries(name, alias) + entry(alias, node)
    if len(root_data) >= ROOT_ENTRIES * 32:
        raise ValueError('root directory is full')
    root_offset = (START + 1 + 2 * fat_sectors) * SECTOR
    image[root_offset:root_offset + len(root_data)] = root_data
    for copy in range(2):
        offset = (START + 1 + copy * fat_sectors) * SECTOR
        image[offset:offset + len(fat)] = fat
    # MBR and standard DOS FAT16 BPB, deterministic timestamps/volume ID.
    struct.pack_into('<B3sB3sII', image, 446, 0x80, b'\xfe\xff\xff', 0x06,
                     b'\xfe\xff\xff', START, partition)
    image[510:512] = b'\x55\xaa'
    boot = bytearray(SECTOR)
    boot[:11] = b'\xeb\x3c\x90B310ESD '
    struct.pack_into('<HBHBHHBHHHII', boot, 11, SECTOR, SPC, 1, 2,
                     ROOT_ENTRIES, 0, 0xf8, fat_sectors, 63, 255, START, partition)
    boot[36:39] = b'\x80\x00\x29'
    struct.pack_into('<I', boot, 39, 0x6530b310)
    boot[43:54], boot[54:62] = b'B310E AUDIO', b'FAT16   '
    boot[510:512] = b'\x55\xaa'
    image[START*SECTOR:(START+1)*SECTOR] = boot
    path.write_bytes(image)
    return {'image': str(path), 'files': file_count, 'size_bytes': len(image),
            'used_clusters': next_cluster - 2, 'cluster_bytes': SPC * SECTOR}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime', type=Path, default=Path('sdcard/.rockbox'))
    parser.add_argument('--rockbox', type=Path, default=Path('sdcard/progs/rockbox.bin'))
    parser.add_argument('--output', type=Path, default=Path('sdcard/emulator-sd.img'))
    parser.add_argument('--size-mib', type=int, default=64)
    parser.add_argument('--seconds', type=float, default=30)
    parser.add_argument('--force', action='store_true', help='replace an existing generated image')
    args = parser.parse_args()
    if not args.runtime.is_dir() or not args.rockbox.is_file():
        parser.error('build Rockbox first to stage the runtime and binary')
    if not 1 <= args.seconds <= 120:
        parser.error('--seconds must be 1..120')
    if args.output.exists() and not args.force:
        parser.error('output exists; use --force to replace this generated image')
    root = directory()
    for path in sorted(args.runtime.rglob('*')):
        if path.is_file():
            add_file(root, Path('.rockbox') / path.relative_to(args.runtime), path.read_bytes())
    add_file(root, 'progs/rockbox.bin', args.rockbox.read_bytes())
    add_file(root, 'test.wav', test_audio(args.seconds))
    add_file(root, '.rockbox/config.cfg', b'volume: 0\nstart in screen: files\nstart directory: /\n')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    print(json.dumps(write_image(args.output, root, args.size_mib), indent=2))


if __name__ == '__main__':
    main()
