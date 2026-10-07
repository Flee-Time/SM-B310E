#!/usr/bin/env python3
"""Build-entry contracts: local data survives cleaning and package updates."""
import argparse
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

ENTRY = Path(__file__).resolve().parents[1] / 'build.py'
spec = importlib.util.spec_from_file_location('b310e_build', ENTRY)
build = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build)


class BuildTests(unittest.TestCase):
    def builder(self, downloads=False, dry=False):
        return build.Builder(argparse.Namespace(msys='C:/msys64', toolchain=None,
                             host_cc=None, jobs=2, qemu_source=None, dsp_blob=None,
                             dry_run=dry, downloads=downloads, force=False, image=Path('card.img')))

    def test_entry_from_another_directory(self):
        with tempfile.TemporaryDirectory() as temp:
            result = subprocess.run([sys.executable, ENTRY, 'sdcard', '--dry-run'],
                                    cwd=temp, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('scripts', result.stdout)
            self.assertEqual(list(Path(temp).iterdir()), [])

    def test_invalid_target_and_jobs(self):
        for args in (['unknown'], ['check', '--jobs', '0']):
            result = subprocess.run([sys.executable, ENTRY, *args], capture_output=True)
            self.assertEqual(result.returncode, 2)

    def test_clean_preserves_card_and_downloads(self):
        with tempfile.TemporaryDirectory() as temp, patch.object(build, 'ROOT', Path(temp)):
            root = Path(temp)
            paths = ['build/bin/os.bin', 'build/firmware/start.o', 'build/fpdoom/source.c',
                     'build/rockbox/source.c', 'build/qemu/source.c', 'sdcard/music/song.wav',
                     'sdcard/.rockbox/config.cfg', 'sdcard/fpbin/config.json']
            for name in paths:
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b'keep')
            self.builder().target('clean')
            for name in paths[:2]:
                self.assertFalse((root / name).exists())
            for name in paths[2:]:
                self.assertEqual((root / name).read_bytes(), b'keep')
            self.builder(downloads=True).target('clean')
            for name in paths[2:5]:
                self.assertFalse((root / name).exists())
            self.assertEqual((root / paths[-1]).read_bytes(), b'keep')

    def test_clean_refuses_redirected_root(self):
        with tempfile.TemporaryDirectory() as temp, patch.object(build, 'ROOT', Path(temp) / 'repo'):
            root, outside = Path(temp) / 'repo', Path(temp) / 'outside'
            root.mkdir()
            (outside / 'bin').mkdir(parents=True)
            keep = outside / 'bin/keep'
            keep.write_bytes(b'keep')
            try:
                if os.name == 'nt':
                    subprocess.run(['cmd', '/c', 'mklink', '/J', str(root / 'build'), str(outside)],
                                   capture_output=True, check=True)
                else:
                    (root / 'build').symlink_to(outside, target_is_directory=True)
            except OSError:
                self.skipTest('Creating test symlinks requires OS privileges')
            with self.assertRaises(RuntimeError):
                self.builder().target('clean')
            self.assertEqual(keep.read_bytes(), b'keep')

    def test_runtime_preserves_user_files(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            runtime = root / 'sdcard/.rockbox'
            runtime.mkdir(parents=True)
            for name in ('config.cfg', 'resume.cfg', 'dynamic.m3u8'):
                (runtime / name).write_bytes(b'user data')
            package = root / 'rockbox.zip'
            with zipfile.ZipFile(package, 'w') as archive:
                archive.writestr('.rockbox/rockbox-info.txt', 'new runtime')
                archive.writestr('.rockbox/codecs/wav.codec', b'codec')
            build.stage_runtime(package, root / 'sdcard')
            for name in ('config.cfg', 'resume.cfg', 'dynamic.m3u8'):
                self.assertEqual((runtime / name).read_bytes(), b'user data')
            self.assertEqual((runtime / 'codecs/wav.codec').read_bytes(), b'codec')

    def test_rejects_unsafe_archive_before_writing(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            for member in ('../outside', '.rockbox/../progs/file', '.rockbox/../../outside', '.rockbox/..\\outside'):
                package = root / 'rockbox.zip'
                with zipfile.ZipFile(package, 'w') as archive:
                    archive.writestr('.rockbox/rockbox-info.txt', 'would write')
                    archive.writestr(member, 'unsafe')
                with self.assertRaises(RuntimeError):
                    build.stage_runtime(package, root / 'sdcard')
                self.assertFalse((root / 'sdcard/.rockbox/rockbox-info.txt').exists())


if __name__ == '__main__':
    unittest.main()
