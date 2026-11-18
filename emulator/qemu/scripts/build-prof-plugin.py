#!/usr/bin/env python3
"""Build the b310e-prof TCG plugin (emulator/qemu/plugins/b310e-prof.c) for
the QEMU in build/qemu: build/qemu-plugin/b310e-prof.dll (Windows: linked
against QEMU's delay-load import library, as QEMU's own contrib plugins) or
.so. QEMU itself is not rebuilt; its build must have plugins on (it does
by default: CONFIG_PLUGIN in build/qemu/build/config-host.h).

  python emulator/qemu/scripts/build-prof-plugin.py [--force]

Prints the plugin's path. os/tests/bench-instr.py runs it when the plugin
is missing or older than its source.

SPDX-License-Identifier: GPL-2.0-or-later
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys

REPO = Path(__file__).resolve().parents[3]
SOURCE = REPO / 'emulator/qemu/plugins/b310e-prof.c'
SUFFIX = '.dll' if os.name == 'nt' else '.so'


def output_path(build=REPO / 'build'):
    return build / 'qemu-plugin' / f'b310e-prof{SUFFIX}'


def build_plugin(qemu=REPO / 'build/qemu', force=False):
    out = output_path()
    if not force and out.is_file() and out.stat().st_mtime >= SOURCE.stat().st_mtime:
        return out
    out.parent.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    if os.name == 'nt':
        env['PATH'] = r'D:\Toolchains\msys64\mingw64\bin;C:\msys64\mingw64\bin;' + env.get('PATH', '')
    gcc = shutil.which('gcc', path=env['PATH']) or 'gcc'
    if not (qemu / 'build/config-host.h').is_file() or \
            '#define CONFIG_PLUGIN' not in (qemu / 'build/config-host.h').read_text():
        raise SystemExit(f'{qemu}/build has no plugin support (configure --enable-plugins)')
    prefix = Path(gcc).resolve().parent.parent
    try:
        glib = subprocess.run(['pkg-config', '--cflags', '--libs', 'glib-2.0'], env=env,
                              capture_output=True, text=True, check=True).stdout.split()
    except (OSError, subprocess.CalledProcessError):
        glib = [f'-I{prefix}/include/glib-2.0', f'-I{prefix}/lib/glib-2.0/include',
                f'-L{prefix}/lib', '-lglib-2.0']
    cmd = [gcc, '-shared', '-O2', '-g', '-std=gnu11', '-Wall', '-Wno-unused-parameter',
           f'-I{qemu / "include/plugins"}', str(SOURCE)]
    if os.name == 'nt':
        api = qemu / 'build/plugins/libqemu_plugin_api.a'
        if not api.is_file():
            raise SystemExit(f'{api} is missing: build QEMU first (python scripts/build.py qemu)')
        cmd += [str(qemu / 'plugins/win32_linker.c'), '-Wl,--allow-shlib-undefined',
                f'-L{api.parent}', '-lqemu_plugin_api']
    else:
        cmd += ['-fPIC']
    cmd += glib + ['-o', str(out)]
    subprocess.run(cmd, env=env, check=True)
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--qemu', type=Path, default=REPO / 'build/qemu',
                        help='the QEMU source tree with its build/ (default: build/qemu)')
    parser.add_argument('--force', action='store_true', help='build even if up to date')
    args = parser.parse_args()
    print(build_plugin(args.qemu.resolve(), args.force))


if __name__ == '__main__':
    sys.exit(main())
