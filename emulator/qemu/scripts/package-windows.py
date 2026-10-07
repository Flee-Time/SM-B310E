#!/usr/bin/env python3
"""Package the desktop emulator with its MinGW DLLs and GTK runtime resources."""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
MARKER = '.b310e-package.json'
LOADER_DIR = Path('lib/gdk-pixbuf-2.0/2.10.0')


def copy(source, destination):
    if source.is_dir():
        shutil.copytree(source, destination, dirs_exist_ok=True)
    else:
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)


def imports(binary, objdump):
    result = subprocess.run([str(objdump), '-p', str(binary)], capture_output=True,
                            text=True, check=True)
    return re.findall(r'DLL Name:\s*(\S+)', result.stdout)


def copy_dependencies(binaries, prefix, output):
    """Fail on missing dependencies instead of shipping a non-starting executable."""
    available = {p.name.lower(): p for p in (prefix / 'bin').glob('*.dll')}
    system = Path(os.environ['SystemRoot']) / 'System32'
    done = set()
    pending = list(binaries)
    while pending:
        binary = pending.pop()
        for name in imports(binary, prefix / 'bin/objdump.exe'):
            key = name.lower()
            if key in done:
                continue
            done.add(key)
            if key in available:
                dependency = available[key]
                copy(dependency, output / 'bin' / dependency.name)
                pending.append(dependency)
            elif not (system / name).is_file() and not key.startswith(('api-ms-', 'ext-ms-')):
                raise RuntimeError(f'Missing DLL {name}, imported by {binary}')
    return sorted(done)


def package(source, prefix, output):
    build = source / 'build'
    flags = (build / 'config-host.h').read_text()
    for feature in ('CONFIG_GTK', 'CONFIG_SDL', 'CONFIG_OPENGL', 'CONFIG_AUDIO_DSOUND'):
        if not re.search(rf'^#define {feature}(?: +1)? *$', flags, re.MULTILINE):
            raise RuntimeError(f'Desktop build is missing {feature}')
    binaries = []
    for name in ('qemu-system-arm.exe', 'qemu-system-armw.exe'):
        binary = build / name
        copy(binary, output / 'bin' / name)
        subprocess.run([str(prefix / 'bin/strip.exe'), '--strip-debug',
                        str(output / 'bin' / name)], check=True)
        binaries.append(binary)
    # MSYS2 libraries relocate relative to bin/, with lib/, share/ and etc/ beside it.
    for relative in ('share/glib-2.0/schemas', 'share/icons/Adwaita',
                     'share/icons/AdwaitaLegacy', 'share/icons/hicolor',
                     'etc/fonts', 'share/fontconfig', str(LOADER_DIR)):
        path = prefix / relative
        if path.exists():
            copy(path, output / relative)
    for required in ('share/glib-2.0/schemas/gschemas.compiled', str(LOADER_DIR / 'loaders.cache')):
        if not (output / required).is_file():
            raise RuntimeError(f'Missing GTK runtime resource: {required}')
    cache = output / LOADER_DIR / 'loaders.cache'
    # MSYS2 generates relocatable loader paths. Reject a cache tied to the build PC.
    if re.search(r'^"(?:[A-Za-z]:|/)[^"\n]*\.dll"\s*$', cache.read_text(encoding='utf-8'), re.MULTILINE):
        raise RuntimeError('GDK loader cache contains absolute paths; regenerate it with MSYS2.')
    binaries.extend((output / LOADER_DIR / 'loaders').glob('*.dll'))
    dependencies = copy_dependencies(binaries, prefix, output)
    copy(source / 'pc-bios/keymaps', output / 'share/qemu/keymaps')
    for size in (16, 24, 32, 48, 64, 128, 256, 512):
        copy(source / f'ui/icons/qemu_{size}x{size}.png',
             output / f'share/icons/hicolor/{size}x{size}/apps/qemu.png')
    copy(source / 'ui/icons/qemu.svg', output / 'share/icons/hicolor/scalable/apps/qemu.svg')
    copy(source / 'COPYING', output / 'licenses/QEMU-COPYING')
    copy(prefix / 'share/licenses', output / 'licenses/MSYS2')
    copy(ROOT / 'docs/b310e-qemu.md', output / 'emulator-guide.md')
    (output / 'run-stock.cmd').write_text(r'''@echo off
setlocal
if "%~1"=="" (
    echo Usage: run-stock.cmd "path to stock.bin" ["path to sdcard.img"]
    exit /b 2
)
set "B310E_ROM=%~f1"
set "B310E_CARD=%~f2"
set "PATH=%~dp0bin;%SystemRoot%\System32;%SystemRoot%"
set "XDG_DATA_DIRS=%~dp0share"
set "GSETTINGS_SCHEMA_DIR=%~dp0share\glib-2.0\schemas"
set "GDK_PIXBUF_MODULE_FILE=%~dp0lib\gdk-pixbuf-2.0\2.10.0\loaders.cache"
set "FONTCONFIG_PATH=%~dp0etc\fonts"
if "%~2"=="" (
    "%~dp0bin\qemu-system-arm.exe" -M b310e -display gtk -audiodev sdl,id=audio0,in.voices=0 -global sc6530_adi.audiodev=audio0 -L "%~dp0share\qemu" -serial stdio -drive "if=none,id=nor,file=%B310E_ROM%,format=raw,readonly=on"
) else (
    "%~dp0bin\qemu-system-arm.exe" -M b310e -display gtk -audiodev sdl,id=audio0,in.voices=0 -global sc6530_adi.audiodev=audio0 -L "%~dp0share\qemu" -serial stdio -drive "if=none,id=nor,file=%B310E_ROM%,format=raw,readonly=on" -drive "if=none,id=sdcard,file=%B310E_CARD%,format=raw"
)
exit /b %errorlevel%
''', encoding='utf-8', newline='\r\n')
    (output / 'README.txt').write_text('''B310E desktop emulator for 64-bit Windows

Extract the entire archive into a writable folder. No MSYS2 installation is needed.
Drag your own stock .bin dump onto run-stock.cmd, or run:
  run-stock.cmd "C:\\path\\stock.bin" "C:\\path\\sdcard.img"
The NOR dump is read-only; an optional SD image is writable.

Arrow keys: D-pad. Enter: OK. F1/F2: soft keys. Escape: END.
Digits: phone keypad. Keypad Enter: DIAL. See emulator-guide.md for all keys.
GTK menus provide zoom, fullscreen and input grab; Ctrl+Alt+G releases the grab.
The console shows the firmware's serial output and startup errors.
For advanced use, bin/qemu-system-arm.exe supports -display sdl and normal QEMU options.
bin/qemu-system-armw.exe provides the same emulator without a console window.

This emulator models many peripherals but remains incomplete. It does not contain
stock firmware, music, game ROMs or proprietary reference sources.
''', encoding='utf-8')
    revision = subprocess.run(['git', '-c', f'safe.directory={ROOT.as_posix()}',
                               '-C', str(ROOT), 'rev-parse', 'HEAD'], capture_output=True,
                              text=True, check=True).stdout.strip()
    (output / 'SOURCE.txt').write_text(
        'QEMU v11.1.0: https://gitlab.com/qemu-project/qemu\n'
        'Commit: 84f07211cc5b4fc6a371559bf8a5de4fb068e648\n'
        'B310E machine/build sources: https://github.com/Flee-Time/SM-B310E\n'
        f'Build checkout base commit: {revision} (local changes may be present)\n'
        'Build instructions: docs/build.md in the B310E repository\n'
        'MSYS2 library sources and package recipes: https://github.com/msys2/MINGW-packages\n'
        'License texts are in licenses/. Preserve them when sharing this package.\n', encoding='utf-8')
    (output / MARKER).write_text(json.dumps({'format': 1, 'imports': dependencies}, indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qemu-source', type=Path, required=True)
    parser.add_argument('--prefix', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    try:
        output = args.output.absolute()
        # Only the named generated package below build/ may be replaced.
        expected = ROOT / 'build/qemu-desktop'
        if output != expected or output.resolve() != expected or output.is_symlink():
            raise RuntimeError(f'Refusing redirected or unexpected package directory: {output}')
        if output.exists() and not (output / MARKER).is_file():
            raise RuntimeError(f'Refusing to replace an unrecognized directory: {output}')
        output.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='qemu-package-', dir=output.parent) as temp:
            stage = Path(temp) / 'qemu-desktop'
            stage.mkdir()
            package(args.qemu_source.resolve(), args.prefix.resolve(), stage)
            if output.exists():
                output.rename(Path(temp) / 'previous-package')
            stage.rename(output)
        archive = shutil.make_archive(str(output), 'zip', output.parent, output.name)
        print(f'Desktop package: {output}\nPortable archive: {archive}')
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        parser.exit(1, f'QEMU packaging failed: {error}\n')


if __name__ == '__main__':
    main()
