#!/usr/bin/env python3
"""Create and verify the pinned QEMU checkout and support Windows without symlinks."""
import argparse
import os
from pathlib import Path
import subprocess
import sys

TAG = 'v11.1.0'
COMMIT = '84f07211cc5b4fc6a371559bf8a5de4fb068e648'
URL = 'https://gitlab.com/qemu-project/qemu.git'
MARKER = '# B310E: copy resources when Windows denies symlinks.'
FALLBACK = '''
def b310e_install_link(source, destination):
    """Retain resources needed by GTK without requiring Developer Mode."""
    try:
        os.symlink(source, destination)
    except OSError as error:
        if os.name != 'nt' or getattr(error, 'winerror', None) != 1314:
            raise
        # B310E: copy resources when Windows denies symlinks.
        if os.path.isdir(source):
            shutil.copytree(source, destination, dirs_exist_ok=True)
        elif os.path.isfile(source):
            shutil.copy2(source, destination)
        # Meson also lists executables that have not been built yet.
        # The desktop packager copies those after Ninja finishes.

'''


def git(source, *arguments):
    result = subprocess.run(['git', '-c', f'safe.directory={source.as_posix()}',
                             '-C', str(source), *arguments], capture_output=True,
                            text=True, encoding='utf-8', errors='replace')
    if result.returncode:
        raise RuntimeError(f'Git failed in {source}: {result.stderr.strip()}')
    return result.stdout.strip()


def git_path(value):
    # MSYS2 Git prints /c/... even when invoked by native Windows Python.
    if os.name == 'nt' and len(value) > 2 and value[0] == '/' and value[1].isalpha() and value[2] == '/':
        value = value[1] + ':' + value[2:]
    return Path(value).resolve()


def checkout(source):
    if not source.exists() or (source.is_dir() and not any(source.iterdir())):
        source.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(['git', '-c', 'advice.detachedHead=false', 'clone', '--depth', '1',
                        '--branch', TAG, '--single-branch', URL, str(source)], check=True)
    if not (source / '.git').exists():
        raise RuntimeError(f'{source} is not a QEMU Git checkout. Use an empty folder '
                           'or --qemu-source pointing to a clone; files were not reset.')
    root = git_path(git(source, 'rev-parse', '--show-toplevel'))
    if root != source:
        raise RuntimeError(f'Git resolved {source} to {root}; refusing to modify it.')
    # HEAD is already the checked-out commit. MSYS2's argument globbing can
    # strip braces from HEAD^{commit} when called from native Python.
    actual = git(source, 'rev-parse', '--verify', 'HEAD')
    if actual != COMMIT:
        raise RuntimeError(f'QEMU {TAG} requires commit {COMMIT}; found {actual} in '
                           f'{source}. Existing files were not reset.')
    if (source / 'VERSION').read_text().strip() != TAG[1:]:
        raise RuntimeError(f'QEMU VERSION was modified in {source}; expected {TAG[1:]}.')
    print(f'QEMU {TAG} verified: {actual}', flush=True)


def prepare_windows(source):
    path = source / 'scripts/symlink-install-tree.py'
    contents = path.read_text(encoding='utf-8')
    if MARKER in contents:
        return
    anchor = "introspect = os.environ.get('MESONINTROSPECT')"
    call = '        os.symlink(source, bundle_dest)'
    if contents.count(anchor) != 1 or contents.count(call) != 1:
        raise RuntimeError(f'Unsupported symlink helper layout in {path}; '
                           'this is a build-helper error, not a QEMU version mismatch.')
    contents = contents.replace('import os\n', 'import os\nimport shutil\n', 1)
    contents = contents.replace(anchor, FALLBACK + anchor, 1)
    contents = contents.replace(call, '        b310e_install_link(source, bundle_dest)', 1)
    path.write_text(contents, encoding='utf-8', newline='\n')
    print('Prepared Windows resource copies without administrator access', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('qemu_source', type=Path)
    args = parser.parse_args()
    try:
        source = args.qemu_source.expanduser().resolve()
        checkout(source)
        if os.name == 'nt':
            prepare_windows(source)
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        parser.exit(1, f'Preparing QEMU failed: {error}\n')


if __name__ == '__main__':
    main()
