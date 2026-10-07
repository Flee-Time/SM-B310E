#!/usr/bin/env python3
"""Compile the prepared game's actual FAT writer and test saves on a FAT32 image."""
import argparse
import importlib.util
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[3]
HERE = Path(__file__).resolve().parent


def main(cc):
    spec = importlib.util.spec_from_file_location('sdimage', ROOT / 'ports/rockbox/tests/make-sd-image.py')
    image = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(image)
    out = ROOT / 'build/validation/game-fat'
    out.mkdir(parents=True, exist_ok=True)
    image.write_image(out / 'test.img', image.directory(), 64, fat32=True)
    sys = ROOT / 'build/game-ports/fpdoom'
    exe = out / 'fat-test.exe'
    subprocess.run([cc, '-std=c99', '-Wall', '-Wextra', '-O2', '-DFAT_WRITE=1',
                    '-Dmkdir=b310e_mkdir', '-Dremove=b310e_remove', '-Drename=b310e_rename',
                    '-I', str(sys), str(HERE / 'fat-test.c'), str(HERE / 'fat-backend.c'),
                    str(sys / 'microfat.c'), str(sys / 'fatfile.c'), str(sys / 'b310e-fs.c'),
                    '-o', str(exe)], check=True)
    subprocess.run([str(exe), str(out / 'test.img')], check=True, timeout=30)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='gcc')
    main(parser.parse_args().cc)
