#!/usr/bin/env python3
"""Build the real ARM save test, boot twice, and inspect its card independently."""
import argparse
import array
import importlib.util
from pathlib import Path
import subprocess
import sys
import wave

ROOT = Path(__file__).resolve().parents[3]
HERE = Path(__file__).resolve().parent


def main(args):
    prepared = ROOT / 'build/game-ports'
    if not (prepared / '.b310e-game-ports.json').is_file():
        raise RuntimeError('Build the games target first to prepare the pinned sources')
    app = ROOT / 'build/validation/arm-save-app'
    app.mkdir(parents=True, exist_ok=True)
    (app / 'arm-save.c').write_bytes((HERE / 'arm-save.c').read_bytes())
    (app / 'Makefile').write_text(
        'NAME = arm-save\n'
        'APP_CFLAGS = -I$(SYSDIR)\n'
        'APP_OBJS1 = $(OBJDIR)/app/arm-save.o\n'
        'include ' + (prepared / 'build_sc6531.make').as_posix() + '\n')
    flags = ['CHIP=3', 'LIBC_SDIO=3', 'TOOLCHAIN=' + args.toolchain,
             # This minimal diagnostic ignores argv. Keep its two-stage
             # startup independent of GCC's cross-stage LTO optimizations.
             'LTO=0', 'SYSDIR=' + (prepared / 'fpdoom').as_posix(),
             'PACK_RELOC=' + (prepared / 'pack_reloc/pack_reloc').as_posix()]
    with (app / 'build.log').open('w') as log:
        for target in ('clean', 'all'):
            subprocess.run([args.make, '-C', str(app), '-j8', target] + flags,
                           check=True, stdout=log, stderr=subprocess.STDOUT)
    output = ROOT / 'build/validation/arm-save'
    command = [sys.executable, str(HERE / 'run-game.py'),
               '--qemu', str(args.qemu), '--nor', str(args.nor),
               '--binary', str(app / 'arm-save.bin'), '--output', str(output),
               '--command', 'arm-save', '--seconds', '10']
    spec = importlib.util.spec_from_file_location('fat_image', HERE / 'read-image.py')
    reader = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(reader)
    for boot in (1, 2):
        subprocess.run(command + (['--reuse-image'] if boot == 2 else []), check=True)
        card = reader.Fat32(output / 'sd.img')
        assert card.read('/result.txt') == (
            f'PASS boot={boot} bytes=73728 append={boot*4} audio=stereo\n').encode()
        assert card.read('/append.txt') == b'test' * boot
        expected = bytes((block+i) & 255 for block in range(72) for i in range(1024))
        assert card.read('/saves/Super Metroid long filename.srm') == expected
        with wave.open(str(output / 'audio.wav'), 'rb') as wav:
            assert wav.getnchannels() == 2 and wav.getsampwidth() == 2
            samples = array.array('h', wav.readframes(wav.getnframes()))
        assert any(samples[::2]) and any(samples[1::2]), 'Both audio channels must run during saves'
        print(f'PASS: ARM SDIO save, boot {boot}, verified bytes and stereo audio')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qemu', type=Path, required=True)
    parser.add_argument('--nor', type=Path, required=True)
    parser.add_argument('--toolchain', default='arm-none-eabi')
    parser.add_argument('--make', default='make')
    main(parser.parse_args())
