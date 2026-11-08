#!/usr/bin/env python3
"""Fetch the upstream source a port of os/ports builds from: a pinned
archive, checked against its SHA-256, unpacked into build/<name>/ as it
is (the ports change nothing in it: what the phone needs is in
os/ports/<port>/).

  python os/ports/sources.py chocolate-doom [--build build]

SPDX-License-Identifier: GPL-2.0-or-later
"""
import argparse
import hashlib
from pathlib import Path
import shutil
import sys
import urllib.request
import zipfile

SOURCES = {
    # Chocolate Doom (GPLv2+): Doom, Heretic, Hexen
    'chocolate-doom': (
        'https://github.com/chocolate-doom/chocolate-doom/archive/'
        '0b3cb528c3f53c61d7a4ebe13a7d522570b98d83.zip',
        'e531c7b591a8b4239e96ae444f0a64eccd91fa33d8659987fe610e9509935440',
        'src/doom/d_main.c'),
    # gnuboy (GPLv2): Game Boy, Game Boy Color
    'gnuboy': (
        'https://github.com/rofl0r/gnuboy/archive/'
        'c367bb4ba96fb07cd62f72f5ecb43aeff7012564.zip',
        'ba23d18746d339e5b04c38cc2b3a6fce9c0889298f2f64dc5efd1bbd3d60084e',
        'cpu.c'),
    # InfoNES (Apache-2.0): the NES
    'infones': (
        'https://github.com/jay-kumogata/InfoNES/archive/'
        '363bac8bbb030c3d3708ab32cd719ae6b2919971.zip',
        '7422ce19b1bd085298a6d6b22cc4ba862030c5a01ae665ff02799155d5af9a8a',
        'src/InfoNES.cpp'),
    # Wolf4SDL (GPLv2 or id's licence): Wolfenstein 3D
    'wolf4sdl': (
        'https://github.com/KS-Presto/Wolf4SDL/archive/'
        'dc8b250af35fb0ace68db5eb879490b50068c20e.zip',
        '393182e7ee9afc77a2ef74d0b583520a4c9b32f5fb39a23015ea6def8f359402',
        'wl_main.c'),
}


def fetch(name, build):
    url, sha256, marker = SOURCES[name]
    dest = build / name
    if (dest / marker).is_file():
        return dest
    build.mkdir(parents=True, exist_ok=True)
    archive = build / f'{name}.zip'
    if not archive.is_file() or hashlib.sha256(archive.read_bytes()).hexdigest() != sha256:
        print(f'{name}: fetching {url}')
        part = archive.with_suffix('.part')
        with urllib.request.urlopen(url) as r, open(part, 'wb') as out:
            shutil.copyfileobj(r, out)
        part.replace(archive)
    got = hashlib.sha256(archive.read_bytes()).hexdigest()
    if got != sha256:
        sys.exit(f'{name}: {archive} has SHA-256 {got}, not {sha256}')
    tmp = build / f'{name}.unpack'
    shutil.rmtree(tmp, ignore_errors=True)
    with zipfile.ZipFile(archive) as z:
        z.extractall(tmp)
    tops = [p for p in tmp.iterdir() if p.is_dir()]
    if len(tops) != 1 or not (tops[0] / marker).is_file():
        sys.exit(f'{name}: {archive} does not hold {marker}')
    shutil.rmtree(dest, ignore_errors=True)
    tops[0].replace(dest)
    shutil.rmtree(tmp, ignore_errors=True)
    print(f'{name}: {dest}')
    return dest


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('names', nargs='+', choices=sorted(SOURCES))
    parser.add_argument('--build', type=Path, default=Path(__file__).resolve().parents[2] / 'build')
    args = parser.parse_args()
    for name in args.names:
        fetch(name, args.build)


if __name__ == '__main__':
    main()
