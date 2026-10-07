#!/usr/bin/env python3
"""Fetch the additional pinned game cores; no ROMs or game data are downloaded."""
import argparse
import hashlib
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import zipfile


def fetch(root, curl):
    jobs = [
        ('infones', 'InfoNES', 'InfoNES.patch', 'jay-kumogata/InfoNES', '363bac8bbb030c3d3708ab32cd719ae6b2919971'),
        ('wolf3d', 'Wolf4SDL', 'wolf3d.patch', 'KS-Presto/Wolf4SDL', 'dc8b250af35fb0ace68db5eb879490b50068c20e'),
        ('fpbuild', 'jfbuild', 'jfbuild.patch', 'jonof/jfbuild', 'efd88d9cc24f753038a28479c9d8e7ac398909c8'),
        ('fpbuild', 'jfmact', 'jfmact.patch', 'jonof/jfmact', '1f0746a3b9704906669d8aaed2bbb982053a393e'),
        ('fpbuild', 'jfduke3d', 'jfduke3d.patch', 'jonof/jfduke3d', '41cd46bc00633e7457d07d88c8add9f99a7d9d41'),
        ('fpbuild', 'jfsw', 'jfsw.patch', 'jonof/jfsw', '1282878348bff97c5cf92401c1253f81da290cc4'),
        ('fpbuild', 'NBlood', 'NBlood.patch', 'nukeykt/NBlood', '5917ab82214a9f0fa8a9d408f9e40143ad72171b'),
        ('snes9x', 'snes9x_src', 'snes9x.patch', None, 'c1afb7804ca27af5ee580d4a45a197241bbb13b8d321ee1bf8b3abf516ff7ec1'),
    ]
    for port, name, patch, repo, pin in jobs:
        parent = root / port
        dest = parent / name
        marker = parent / ('.b310e-source-' + name)
        if dest.is_dir() and marker.is_file() and marker.read_text() == pin:
            continue
        if dest.exists():
            raise RuntimeError(f'Unrecognized source directory: {dest}; move it aside before retrying')
        url = (f'https://github.com/{repo}/archive/{pin}.zip' if repo else
               'https://old-releases.ubuntu.com/ubuntu/pool/multiverse/s/snes9x/snes9x_1.43.orig.tar.gz')
        parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='fetch-', dir=parent) as tmp:
            tmp = Path(tmp)
            archive = tmp / 'source'
            subprocess.run([curl, '-fLsS', '--retry', '3', '-o', str(archive), url], check=True)
            if not repo and hashlib.sha256(archive.read_bytes()).hexdigest() != pin:
                raise RuntimeError('SNES source checksum mismatch')
            unpack = tmp / 'unpack'
            if repo:
                with zipfile.ZipFile(archive) as z:
                    z.extractall(unpack)
                source, = unpack.iterdir()
            else:
                with tarfile.open(archive) as t:
                    t.extractall(unpack, filter='data')
                source = unpack / 'snes9x-1.43.orig/snes9x-1.43-src/snes9x'
            for file in source.rglob('*'):
                if file.is_file():
                    data = file.read_bytes()
                    if b'\0' not in data:
                        file.write_bytes(data.replace(b'\r\n', b'\n'))
            # The upstream port's DSP1 integer implementation is a separate pinned file.
            if not repo:
                subprocess.run([curl, '-fLsS', '--retry', '3', '-o', str(source / 'dsp1new.cpp'),
                    'https://raw.githubusercontent.com/snes9xgit/snes9x/96059dd45aed03859bff5a3e30f1d1b13136a8f9/dsp1.cpp'], check=True)
            patchfile = parent / patch
            subprocess.run(['git', 'apply', '--ignore-space-change', '--unsafe-paths',
                            f'--directory={source.as_posix()}', str(patchfile)], check=True)
            if name == 'NBlood':
                for file in (parent / 'eduke32').glob('*'):
                    shutil.copy2(file, source / 'source/blood/src' / file.name)
            # A failed download or patch leaves only the disposable staging tree.
            shutil.move(source, dest)
        marker.write_text(pin)
        print(f'Fetched and patched {name}', flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', type=Path)
    parser.add_argument('--curl', default='curl')
    args = parser.parse_args()
    fetch(args.root.resolve(), args.curl)
