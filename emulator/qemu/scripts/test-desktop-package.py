#!/usr/bin/env python3
"""Start packaged GTK/SDL displays without MSYS2 on PATH and exercise LCD/key input."""
import argparse
import json
import os
from pathlib import Path
import runpy
import shutil
import subprocess
import tempfile
import time

API = runpy.run_path(str(Path(__file__).with_name('test-audio.py')))


def check(package, display, output, audio_backend):
    output.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env['PATH'] = os.pathsep.join([str(package / 'bin'), str(Path(os.environ['SystemRoot']) / 'System32')])
    env['XDG_DATA_DIRS'] = str(package / 'share')
    env['GSETTINGS_SCHEMA_DIR'] = str(package / 'share/glib-2.0/schemas')
    env['GDK_PIXBUF_MODULE_FILE'] = str(package / 'lib/gdk-pixbuf-2.0/2.10.0/loaders.cache')
    env['FONTCONFIG_PATH'] = str(package / 'etc/fonts')
    qmp_port, qt_port = API['free_port'](), API['free_port']()
    command = [str(package / 'bin/qemu-system-arm.exe'), '-M', 'b310e,boot-mode=ours',
               '-accel', 'qtest', '-display', display, '-serial', 'none',
               '-audiodev', f'{audio_backend},id=audio0,in.voices=0', '-global', 'sc6530_adi.audiodev=audio0',
               '-L', str(package / 'share/qemu'), '-qmp',
               f'tcp:127.0.0.1:{qmp_port},server=on,wait=off', '-qtest',
               f'tcp:127.0.0.1:{qt_port},server=on,wait=off',
               '-qtest-log', str(output / 'qtest.log')]
    startup = subprocess.STARTUPINFO()
    startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = subprocess.SW_HIDE
    qmp = qt = None
    with (output / 'stderr.log').open('wb') as log:
        process = subprocess.Popen(command, env=env, cwd=package.parent,
                                   stdout=log, stderr=log, startupinfo=startup)
        try:
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    raise RuntimeError(f'{display} exited ({process.returncode}); see {output / "stderr.log"}')
                try:
                    qmp = API['QMP'](qmp_port)
                    break
                except OSError:
                    time.sleep(.1)
            if qmp is None:
                raise RuntimeError(f'{display} did not start')
            qt = API['QTest'](qt_port)
            # A known LCD image must survive presentation through each GUI backend.
            qt.write(0x60000000, 0x11)
            qt.write(0x60000000, 0x29)
            qt.memory(0x34040000, b'\x00\xf8' * (128 * 160))
            for offset, value in [(4, 128 | (160 << 16)), (8, 0), (12, 128 | (160 << 16)),
                                  (0x20, 0x251), (0x24, 0x34040000 >> 2),
                                  (0x2c, 128 | (160 << 16)), (0x30, 128)]:
                qt.write(0x20d00000 + offset, value)
            for command_id, end in ((0x2a, 127), (0x2b, 159)):
                qt.write(0x60000000, command_id)
                for value in (0, 0, 0, end):
                    qt.write(0x60020000, value)
            qt.write(0x60000000, 0x2c)
            qt.write(0x20d00000, 8)
            time.sleep(1)
            qmp.command('screendump', {'filename': str(output / 'lcd.ppm'), 'format': 'ppm'})
            pixels = (output / 'lcd.ppm').read_bytes()
            assert pixels[-128*160*3:] == b'\xf8\0\0' * (128*160)
            qmp.command('screendump', {'filename': str(output / 'lcd.png'), 'format': 'png'})
            for down in (True, False):
                qmp.command('input-send-event', {'events': [{'type': 'key', 'data': {
                    'down': down, 'key': {'type': 'qcode', 'data': 'f1'}}}]})
            assert qt.read(0x87000008) & 0x88 == 0x88, 'soft-key press/release did not reach keypad'
            qmp.command('quit')
            assert process.wait(timeout=10) == 0
        finally:
            if qt:
                qt.close()
            if qmp:
                qmp.close()
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=10)
    messages = (output / 'stderr.log').read_text(errors='replace')
    assert not any(word in messages for word in ('Gtk-WARNING', 'Gtk-CRITICAL', 'GdkPixbuf-WARNING',
                                                 'Failed to load', 'Could not load a pixbuf'))
    print(f'PASS relocated package: {display}, {audio_backend} audio startup, LCD, PNG and keypad')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--package', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--audio-backend', choices=('none', 'dsound', 'sdl'), default='none',
                        help='use native audio only when the host has an output device')
    parser.add_argument('--opengl', action='store_true', help='also test GTK GL on a host with a working GPU context')
    args = parser.parse_args()
    if os.name != 'nt':
        parser.error('This check requires a Windows desktop session.')
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='desktop-relocation-', dir=output, ignore_cleanup_errors=True) as temp:
        package = Path(temp) / 'Folder with spaces' / 'qemu-desktop'
        shutil.copytree(args.package.resolve(), package)
        displays = ['gtk,gl=off', 'sdl,gl=off']
        if args.opengl:
            displays.append('gtk,gl=on')
        for index, display in enumerate(displays):
            check(package, display, output / f'display-{index}', args.audio_backend)
    (output / 'results.json').write_text(json.dumps({'displays': displays, 'passed': True}, indent=2))


if __name__ == '__main__':
    main()
