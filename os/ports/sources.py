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
    # Ken Silverman's Build engine, JonoF's port (Ken's licence, buildlic.txt: not GPL
    # compatible, see os/ports/duke/README)
    'jfbuild': (
        'https://github.com/jonof/jfbuild/archive/'
        'efd88d9cc24f753038a28479c9d8e7ac398909c8.zip',
        '767d6bfdedc801608d110423de255b39dbe57b629668126a65d9c853e9472f29',
        'src/engine.c'),
    # 3D Realms' MACT library (GPLv2)
    'jfmact': (
        'https://github.com/jonof/jfmact/archive/'
        '1f0746a3b9704906669d8aaed2bbb982053a393e.zip',
        '68f16ea5ecb38ea732b04f10ed86465819ae512258fb884903ebc7bbfe5c7a25',
        'control.c'),
    # Duke Nukem 3D (GPLv2)
    'jfduke3d': (
        'https://github.com/jonof/jfduke3d/archive/'
        '41cd46bc00633e7457d07d88c8add9f99a7d9d41.zip',
        'edde47b058d0c37bc4dbdff078dc55cf0dd2c5d76d8b2526dc759c4ead44b1af',
        'src/game.c'),
    # Shadow Warrior (GPLv2), the 2023 release: the engine, MACT and audiolib it was
    # made with are those of jfduke3d's (same commits)
    'jfsw': (
        'https://github.com/jonof/jfsw/archive/'
        '41b1ce1b7e01f8158fe89b20b79d9f0b23f84e4c.zip',
        'c1c935f03730e9b82c9e7c758f4c99d6049e05dafe13b74e09766576c6eede54',
        'src/game.c'),
    # NBlood (GPLv2): Blood. The commit fpdoom's adaptation to jfbuild is made for
    # (below); only its game code is used, on jfbuild (os/ports/blood)
    'nblood': (
        'https://github.com/nukeykt/NBlood/archive/'
        '5917ab82214a9f0fa8a9d408f9e40143ad72171b.zip',
        'c886ef4cf2b7b451052517127e0662b90d89a48082ef9fab20704a4b56fd5510',
        'source/blood/src/blood.cpp'),
    # fpdoom (public domain): its NBlood on jfbuild (fpbuild/NBlood.patch, eduke32/),
    # applied by os/ports/blood/prepare.py
    'fpdoom': (
        'https://github.com/ilyakurdyukov/fpdoom/archive/'
        '296795c2cfa084b061a7529997a19e75f4f91e36.zip',
        '891e174949c2c7f85b1ca16d84677f9dbb59878775e4fd530f5cec55635e62c4',
        'fpbuild/NBlood.patch'),
    # Quake (GPLv2): id Software's release; WinQuake, the software renderer
    'quake': (
        'https://github.com/id-Software/Quake/archive/'
        'bf4ac424ce754894ac8f1dae6a3981954bc9852d.zip',
        '9b6f6c5ead62e9a64d32d4bcc5ef57fbdc1d1172219fd96454b1fbc159882831',
        'WinQuake/host.c'),
    # snes9x2002 (Snes9x's licence: free, NOT for commercial use, and NOT GPL; libretro's
    # core from PocketSNES, with ARM assembly): the SNES. Its code stays out of the
    # repository; a built snes9x.app is for the user's own phone only
    'snes9x2002': (
        'https://github.com/libretro/snes9x2002/archive/'
        '6ffbf9ef4f0063e1f1b78a40d10c50fc52f2524c.zip',
        '42f79edc8bd3428c575ca770440cc620da3c338cfec6d153c70e3b0f991161f2',
        'src/cpuexec.c'),
    # JonoF's audiolib: only its headers are used (GPLv2)
    'jfaudiolib': (
        'https://github.com/jonof/jfaudiolib/archive/'
        '96cb69e686a56069d1789cf6748a63593af31e6e.zip',
        '491d8f9df332d8d4d9248d5ea41d92f744c12a167c2cd00fd5b9dd8dc7e99a95',
        'include/fx_man.h'),
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
