#!/usr/bin/env python3
"""Regressions for a fresh checkout and Windows resource-copy preparation."""
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import types
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / 'emulator/qemu/scripts/prepare-build.py'
spec = importlib.util.spec_from_file_location('qemu_prepare', SCRIPT)
prepare = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prepare)


class QemuBuildTests(unittest.TestCase):
    def repository(self, path):
        path.mkdir()
        subprocess.run(['git', 'init', str(path)], check=True, capture_output=True)
        (path / 'VERSION').write_text('11.1.0\n')
        command = ['git', '-c', f'safe.directory={path.as_posix()}', '-C', str(path)]
        subprocess.run([*command, 'add', 'VERSION'], check=True, capture_output=True)
        subprocess.run([*command, '-c', 'user.name=Test', '-c', 'user.email=test@example.invalid',
                        'commit', '-m', 'fixture'], check=True, capture_output=True)
        return prepare.git(path, 'rev-parse', 'HEAD')

    def test_wrong_commit_reports_actual_without_reset(self):
        with tempfile.TemporaryDirectory() as temp:
            source = Path(temp) / 'qemu source'
            actual = self.repository(source)
            with self.assertRaisesRegex(RuntimeError, actual):
                prepare.checkout(source)
            self.assertEqual((source / 'VERSION').read_text(), '11.1.0\n')

    def test_empty_folder_clones_and_linked_worktree_is_accepted(self):
        with tempfile.TemporaryDirectory() as temp:
            parent = Path(temp)
            origin = parent / 'origin'
            commit = self.repository(origin)
            subprocess.run(['git', '-c', f'safe.directory={origin.as_posix()}',
                            '-c', 'user.name=Test', '-c', 'user.email=test@example.invalid',
                            '-C', str(origin), 'tag', '-a', 'v11.1.0', '-m', 'release'],
                           check=True, capture_output=True)
            empty = parent / 'empty'
            empty.mkdir()
            with patch.object(prepare, 'URL', str(origin)), patch.object(prepare, 'COMMIT', commit):
                prepare.checkout(empty)
                worktree = parent / 'linked'
                prepare.git(empty, 'worktree', 'add', '--detach', str(worktree))
                self.assertTrue((worktree / '.git').is_file())
                prepare.checkout(worktree)

    def test_folder_inside_parent_repo_is_not_used_as_qemu(self):
        with tempfile.TemporaryDirectory() as temp:
            parent = Path(temp) / 'parent'
            self.repository(parent)
            source = parent / 'archive'
            source.mkdir()
            (source / 'VERSION').write_text('11.1.0\n')
            with self.assertRaisesRegex(RuntimeError, 'not a QEMU Git checkout'):
                prepare.checkout(source)
            self.assertEqual((source / 'VERSION').read_text(), '11.1.0\n')

    def test_current_upstream_exception_layout_and_idempotency(self):
        with tempfile.TemporaryDirectory() as temp:
            source = Path(temp)
            path = source / 'scripts/symlink-install-tree.py'
            path.parent.mkdir()
            path.write_text("import os\nintrospect = os.environ.get('MESONINTROSPECT')\n"
                            "for source, bundle_dest in []:\n    try:\n"
                            "        os.symlink(source, bundle_dest)\n"
                            "    except BaseException as e:\n        raise e\n")
            prepare.prepare_windows(source)
            first = path.read_bytes()
            compile(first, str(path), 'exec')
            prepare.prepare_windows(source)
            self.assertEqual(path.read_bytes(), first)

    def test_denied_symlinks_copy_resources_and_preserve_other_errors(self):
        namespace = {'os': types.SimpleNamespace(name='nt', path=os.path), 'shutil': shutil}
        exec(prepare.FALLBACK, namespace)
        function = namespace['b310e_install_link']
        error = OSError('Windows privilege not held')
        error.winerror = 1314
        with tempfile.TemporaryDirectory() as temp:
            source = Path(temp) / 'keymaps'
            source.mkdir()
            (source / 'en-us').write_bytes(b'keyboard data')
            namespace['os'].symlink = lambda *a: (_ for _ in ()).throw(error)
            function(str(source), str(Path(temp) / 'copied'))
            self.assertEqual((Path(temp) / 'copied/en-us').read_bytes(), b'keyboard data')
            function(str(source / 'en-us'), str(Path(temp) / 'file'))
            self.assertEqual((Path(temp) / 'file').read_bytes(), b'keyboard data')
            function(str(Path(temp) / 'unbuilt.exe'), str(Path(temp) / 'future.exe'))
            other = OSError('disk full')
            namespace['os'].symlink = lambda *a: (_ for _ in ()).throw(other)
            with self.assertRaisesRegex(OSError, 'disk full'):
                function(str(source), str(Path(temp) / 'failed'))

    def test_desktop_default_and_explicit_headless_reach_wrapper(self):
        for arguments, mode in (([], 'desktop'), (['--qemu-headless'], 'headless')):
            result = subprocess.run([sys.executable, ROOT / 'scripts/build.py',
                                     'qemu', '--dry-run', *arguments], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn(mode, result.stdout)


if __name__ == '__main__':
    unittest.main()
