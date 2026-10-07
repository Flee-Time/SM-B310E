#!/usr/bin/env python3
"""Build B310E firmware, ports and emulator from one cross-platform entry point."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[1]
TARGETS = ('firmware', 'os-sd', 'debug', 'dsp-diag', 'fpmain', 'games', 'rockbox',
           'qemu', 'sdcard', 'sd-image', 'hosttest', 'check', 'clean', 'doctor',
           'dis', 'size', 'stockram', 'stockram-diag')


def compiler_dir(value, program, candidates=()):
    if value:
        directory = Path(value).expanduser().resolve()
        if not shutil.which(program, path=str(directory)):
            raise RuntimeError(f'{program} not found in {directory}')
        return directory
    found = shutil.which(program)
    if found:
        return Path(found).resolve().parent
    for directory in candidates:
        if shutil.which(program, path=str(directory)):
            return Path(directory).resolve()
    return None


def msys_path(path):
    value = str(path).replace('\\', '/')
    if len(value) > 2 and value[1] == ':':
        return '/' + value[0].lower() + value[2:]
    return value


def msys_root(value):
    if value:
        return Path(value).expanduser().resolve()
    for candidate in (Path('D:/Toolchains/msys64'), Path('C:/msys64')):
        if (candidate / 'usr/bin/bash.exe').is_file():
            return candidate.resolve()
    return Path('D:/Toolchains/msys64')


class Builder:
    def __init__(self, args):
        self.args = args
        self.env = os.environ.copy()
        self.msys = msys_root(args.msys) if os.name == 'nt' else None
        self.arm = compiler_dir(args.toolchain, 'arm-none-eabi-gcc', (
            Path('D:/Toolchains/arm-none-eabi/bin'), ROOT.parent / '.tools/arm-toolchain/bin', Path('C:/arm-gcc/bin'),
            Path('C:/Program Files (x86)/Arm GNU Toolchain arm-none-eabi/14.2 rel1/bin'),
        ))
        host_hint = args.host_cc
        if not host_hint and self.msys and (self.msys / 'mingw64/bin/gcc.exe').is_file():
            # A WinLibs/UCRT compiler on Windows PATH must not override MINGW64.
            host_hint = self.msys / 'mingw64/bin'
        self.host = compiler_dir(host_hint, 'gcc')
        paths = [str(p) for p in (self.arm, self.host) if p]
        if os.name == 'nt':
            paths += [str(self.msys / 'usr/bin'), 'C:/Program Files/7-Zip']
            self.env['MSYSTEM'] = 'MINGW64'
            # Use Windows' certificate store rather than a missing MSYS CA bundle.
            curl = Path('C:/Windows/System32/curl.exe')
            if curl.is_file():
                self.env['B310E_CURL'] = msys_path(curl)
        self.env['PATH'] = os.pathsep.join(paths + [self.env.get('PATH', '')])
        self.env['B310E_BUILD_JOBS'] = str(args.jobs)
        self.env['B310E_PYTHON'] = msys_path(sys.executable) if os.name == 'nt' else sys.executable
        for key, value in [('B310E_TOOLCHAIN', self.arm), ('B310E_HOST_CC', self.host)]:
            if value:
                self.env[key] = msys_path(value) if os.name == 'nt' else str(value)
        self.qemu_source = Path(args.qemu_source).expanduser().resolve() if args.qemu_source else ROOT / 'build/qemu'

    def run(self, command, label):
        if self.args.dry_run:
            print(label + ': ' + subprocess.list2cmdline([str(p) for p in command]))
            return
        logs = ROOT / 'build/logs'
        logs.mkdir(parents=True, exist_ok=True)
        log = logs / (label + '.log')
        print(f'Building {label}; log: {log}', flush=True)
        with log.open('w', encoding='utf-8') as output:
            with subprocess.Popen([str(p) for p in command], cwd=ROOT, env=self.env,
                                  stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                  text=True, encoding='utf-8', errors='replace') as process:
                for line in process.stdout:
                    print(line, end='', flush=True)
                    output.write(line)
                    output.flush()
                code = process.wait()
        if code:
            raise RuntimeError(f'{label} failed (exit {code}); see {log}')

    def shell(self, relative, label, *arguments):
        bash = self.msys / 'usr/bin/bash.exe' if self.msys else Path(shutil.which('bash') or 'bash')
        if not self.args.dry_run and not shutil.which(str(bash), path=self.env['PATH']):
            raise RuntimeError('Bash is required for ports and QEMU; set --msys on Windows.')
        if os.name == 'nt':
            # Paths are data arguments; never interpolate them into shell source.
            command = [bash, '-c', 'export PATH=/usr/bin:/bin:$PATH; exec bash "$@"',
                       'b310e', msys_path(ROOT / relative), *arguments]
        else:
            command = [bash, ROOT / relative, *arguments]
        self.run(command, label)

    def native(self, target):
        command = [sys.executable, ROOT / 'scripts/targets/firmware.py', target,
                   '--jobs', str(self.args.jobs)]
        if self.args.dsp_blob:
            command += ['--dsp-blob', str(Path(self.args.dsp_blob).resolve())]
        self.run(command, target)

    def copy(self, source, dest):
        if self.args.dry_run:
            print(f'Stage {source} -> {dest}')
            return
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, dest)

    def target(self, name):
        if name in ('firmware', 'os-sd', 'debug', 'dsp-diag', 'hosttest', 'check', 'dis', 'size'):
            self.native(name)
        elif name in ('fpmain', 'games', 'rockbox'):
            if not self.args.dry_run and (not self.arm or not self.host):
                raise RuntimeError('ARM and host GCC are required; use --toolchain and --host-cc.')
            if name == 'rockbox' and self.arm and ' ' in str(self.arm):
                raise RuntimeError('Rockbox requires a toolchain path without spaces; use --toolchain.')
            self.shell(f'scripts/targets/{name}.sh', name)
        elif name == 'qemu':
            if os.name == 'nt':
                self.run(['powershell', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
                          ROOT / 'scripts/targets/qemu.ps1', '-QemuSrc', self.qemu_source,
                          '-Msys64', self.msys, '-Jobs', str(self.args.jobs),
                          '-DisplayMode', 'headless' if self.args.qemu_headless else 'desktop'], 'qemu')
            else:
                self.shell('scripts/targets/qemu.sh', 'qemu', str(self.qemu_source), str(self.args.jobs),
                           'headless' if self.args.qemu_headless else 'desktop')
        elif name == 'sdcard':
            for target in ('os-sd', 'fpmain', 'games', 'rockbox'):
                self.target(target)
            self.copy(ROOT / 'build/bin/os-sd.bin', ROOT / 'sdcard/progs/os.bin')
            self.copy(ROOT / 'ports/fpmain/games-README.md', ROOT / 'sdcard/games/README.md')
            if not self.args.dry_run:
                for folder in ('doom1', 'doom2', 'duke3d', 'sw', 'heretic', 'hexen', 'wolf3d', 'blood', 'retris', 'snes', 'gameboy', 'nes'):
                    (ROOT / 'sdcard/games' / folder).mkdir(parents=True, exist_ok=True)
        elif name == 'sd-image':
            self.run([sys.executable, ROOT / 'ports/rockbox/tests/make-sd-image.py',
                      '--fat32', '--runtime', ROOT / 'sdcard/.rockbox',
                      '--rockbox', ROOT / 'sdcard/progs/rockbox.bin',
                      '--fpmain', ROOT / 'sdcard/fpbin/fpmain.bin',
                      '--config', ROOT / 'sdcard/fpbin/config.json',
                      '--output', self.args.image, *(['--force'] if self.args.force else [])], 'sd-image')
        elif name in ('stockram', 'stockram-diag'):
            script = 'pack-stockram.sh' if name == 'stockram' else 'diag-pack.sh'
            self.shell(f'tools/stockram/{script}', name)
        elif name == 'clean':
            # Clean named generated directories only. Card data and external checkouts survive.
            build_root = ROOT / 'build'
            if build_root.is_symlink() or build_root.resolve() != ROOT.resolve() / 'build':
                raise RuntimeError(f'Refusing to clean redirected build root: {build_root}')
            folders = ['bin', 'firmware', 'host', 'fpmain', 'games', 'logs', 'qemu-desktop']
            if self.args.downloads:
                folders += ['fpdoom', 'rockbox', 'qemu']
            for folder in folders:
                path = ROOT / 'build' / folder
                if not path.exists():
                    continue
                if path.is_symlink() or path.resolve() != (ROOT / 'build').resolve() / folder:
                    raise RuntimeError(f'Refusing to clean redirected directory: {path}')
                print(f'Remove {path}')
                if not self.args.dry_run:
                    shutil.rmtree(path)
            archive = build_root / 'qemu-desktop.zip'
            if archive.exists():
                if archive.is_symlink() or archive.resolve() != build_root.resolve() / archive.name:
                    raise RuntimeError(f'Refusing to clean redirected archive: {archive}')
                print(f'Remove {archive}')
                if not self.args.dry_run:
                    archive.unlink()
        elif name == 'doctor':
            print(f'Repository: {ROOT}\nPython: {sys.executable}\nARM GCC: {self.arm or "missing"}\nHost GCC: {self.host or "missing"}\nQEMU source: {self.qemu_source}')
            for tool in ('git', 'make', 'bash', '7z'):
                print(f'{tool}: {shutil.which(tool, path=self.env["PATH"]) or "missing"}')


def stage_runtime(archive, destination):
    """Replace packaged runtime files while retaining settings, playlists and saves."""
    root = destination.resolve()
    with zipfile.ZipFile(archive) as package:
        for item in package.infolist():
            path = root / item.filename
            if ('\\' in item.filename or '..' in Path(item.filename).parts
                    or not path.resolve().is_relative_to(root / '.rockbox')
                    or not item.filename.startswith('.rockbox/')):
                raise RuntimeError(f'Unexpected runtime archive member: {item.filename}')
        package.extractall(root)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('target', choices=TARGETS)
    parser.add_argument('--jobs', type=int, default=min(os.cpu_count() or 1, 8))
    parser.add_argument('--toolchain', default=os.environ.get('B310E_TOOLCHAIN'))
    parser.add_argument('--host-cc', default=os.environ.get('B310E_HOST_CC'))
    parser.add_argument('--msys', default=os.environ.get('B310E_MSYS'),
                        help='MSYS2 root; auto-detect D:/Toolchains/msys64 then C:/msys64')
    parser.add_argument('--qemu-source', default=os.environ.get('B310E_QEMU_SOURCE'))
    parser.add_argument('--qemu-headless', action='store_true', help='omit GTK/SDL/OpenGL and the Windows desktop package')
    parser.add_argument('--dsp-blob', help='optional hash-verified DSP bundle for dsp-diag')
    parser.add_argument('--dry-run', action='store_true', help='show the plan without changing files')
    parser.add_argument('--downloads', action='store_true', help='also remove in-repo upstream clones with clean')
    parser.add_argument('--image', type=Path, default=ROOT / 'sdcard/emulator-sd.img')
    parser.add_argument('--force', action='store_true', help='replace an existing generated SD image')
    args = parser.parse_args()
    if not 1 <= args.jobs <= 128:
        parser.error('--jobs must be 1..128')
    try:
        Builder(args).target(args.target)
    except (RuntimeError, OSError) as error:
        parser.exit(1, f'Build failed: {error}\n')


if __name__ == '__main__':
    main()
