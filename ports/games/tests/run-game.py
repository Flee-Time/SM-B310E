#!/usr/bin/env python3
"""Boot a packed game through the real SD loader on a disposable SD image.

Requires the user's local firmware dump and ROM; neither is distributed.
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import runpy
import shutil
import socket
import struct
import subprocess
import time

ROOT = Path(__file__).resolve().parents[3]
QMP = runpy.run_path(str(ROOT / 'emulator/qemu/scripts/capture-stock.py'))['QMP']


def port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0)); return s.getsockname()[1]


def rsp(s, payload):
    data = payload.encode()
    s.sendall(b'$' + data + b'#' + f'{sum(data)&255:02x}'.encode())
    while s.recv(1) != b'$':
        pass
    result = bytearray()
    while True:
        c = s.recv(1)
        if c == b'#': break
        result += c
    s.recv(2); s.sendall(b'+')
    return result.decode()


def main(args):
    out = args.output.resolve()
    if not out.is_relative_to(ROOT / 'build/validation'):
        raise RuntimeError('Test output must be inside build/validation')
    out.mkdir(parents=True, exist_ok=True)
    boot = out / 'sdboot'
    shutil.copytree(ROOT / 'build/fpdoom/sdboot', boot, dirs_exist_ok=True,
                    ignore=shutil.ignore_patterns('obj*', '*.bin'))
    shutil.copy2(ROOT / 'build/fpdoom/build_sc6531.make', out / 'build_sc6531.make')
    # The pinned loader has a CCS typo. Its source stays in this disposable
    # test directory, allowing either baseline or fixed games to share it.
    sysdir = out / 'fpdoom'
    shutil.copytree(ROOT / 'build/fpdoom/fpdoom', sysdir, dirs_exist_ok=True,
                    ignore=shutil.ignore_patterns('obj*', '*.bin', '*.elf'))
    driver = sysdir / 'sdio.c'
    driver.write_text(driver.read_text().replace(
        'sdio_shl = sd_ver == 1 ? resp[0] >> 30 & 0 : 9;',
        'sdio_shl = sd_ver && (resp[0] & (1u << 30)) ? 0 : 9;'))
    entry = boot / 'entry.c'
    text = entry.read_text()
    # QEMU exposes its NOR at zero and PSRAM at 0x04000000. Use this alias
    # for the loader handoff; the game itself is completely unmodified.
    text = text.replace('ram_addr = fw_addr + 0x04000000;', 'fw_addr = 0;\n\tram_addr = fw_addr + 0x04000000;')
    entry.write_text(text)
    subprocess.run(['make', '-C', str(boot), 'clean', 'CHIP=3'], check=True,
                    stdout=subprocess.DEVNULL)
    subprocess.run(['make', '-C', str(boot), '-j8', 'CHIP=3', 'TOOLCHAIN=arm-none-eabi',
                    'SYSDIR=' + sysdir.as_posix(),
                    'EXTRA_CFLAGS=-Wno-error=incompatible-pointer-types'], check=True,
                    stdout=(out / 'boot-build.log').open('w'), stderr=subprocess.STDOUT)
    spec = importlib.util.spec_from_file_location('sdimage', ROOT / 'ports/rockbox/tests/make-sd-image.py')
    image = importlib.util.module_from_spec(spec); spec.loader.exec_module(image)
    if args.reuse_image:
        if not (out / 'sd.img').is_file():
            raise RuntimeError('No SD image to reuse')
        reader = runpy.run_path(str(Path(__file__).with_name('read-image.py')))['Fat32']
        if reader(out / 'sd.img').read('/fpbin/fpmain.bin') != args.binary.read_bytes():
            raise RuntimeError('The saved image contains another binary; recreate the image for a new build')
    else:
        tree = image.directory()
        image.add_file(tree, 'fpbin/fpmain.bin', args.binary.read_bytes())
        image.add_file(tree, 'fpbin/config.txt', (args.command+'\n').encode())
        if args.rom: image.add_file(tree, args.rom_path, args.rom.read_bytes())
        if args.data:
            for file in args.data.rglob('*'):
                if file.is_file():image.add_file(tree,args.data_path+'/'+file.relative_to(args.data).as_posix(),file.read_bytes())
        image.write_image(out / 'sd.img', tree, args.size, fat32=True)
    qmpport, gdbport = port(), port()
    command = [str(args.qemu), '-M', 'b310e,boot-mode=stock', '-display', 'none', '-serial', 'none',
               '-S', '-gdb', f'tcp:127.0.0.1:{gdbport}', '-qmp',
               f'tcp:127.0.0.1:{qmpport},server=on,wait=off',
               '-drive', f'file={args.nor.resolve().as_posix()},format=raw,if=none,id=nor,readonly=on',
               '-drive', f'file={(out / "sd.img").as_posix()},format=raw,if=none,id=sdcard',
               '-device', f'loader,file={(boot / "sdboot3.bin").as_posix()},addr=0x40004000,force-raw=on',
               '-audiodev', f'wav,id=audio0,path={(out / "audio.wav").as_posix()},out.frequency=22050',
               '-global', 'sc6530_adi.audiodev=audio0']
    if args.trace_int:
        command += ['-d', 'int,guest_errors', '-D', (out/'cpu.log').as_posix()]
    with (out / 'qemu.log').open('w') as log:
        process = subprocess.Popen(command, stdout=log, stderr=log)
        qmp = None
        try:
            for _ in range(100):
                try: qmp = QMP(qmpport); break
                except OSError:
                    if process.poll() is not None: raise RuntimeError((out / 'qemu.log').read_text())
                    time.sleep(.05)
            if qmp is None: raise RuntimeError('QMP startup timed out')
            with socket.create_connection(('127.0.0.1', gdbport)) as s:
                s.settimeout(5)
                result = rsp(s, 'P0f=00400040')
                if result != 'OK': raise RuntimeError('Could not set SD loader PC: ' + result)
                rsp(s, 'D')
            qmp.command('cont')
            started = time.monotonic()
            actions=json.loads(args.actions.read_text()) if args.actions else []
            events=[(s,'capture',None) for s in range(5,args.seconds+1,5)]
            events += [(a['seconds'],'key',a) for a in actions]
            for second,kind,action in sorted(events,key=lambda e:e[0]):
                time.sleep(max(0, started+second-time.monotonic()))
                if kind=='key':
                    qmp.command('human-monitor-command',{'command-line':f'sendkey {action["key"]} {action.get("hold",180)}'})
                    continue
                qmp.command('stop')
                qmp.command('screendump', {'filename': (out / f'frame-{second}.png').as_posix(),'format':'png'})
                regs = qmp.command('human-monitor-command', {'command-line': 'info registers'})
                (out / f'registers-{second}.txt').write_text(str(regs))
                for label, addr, size in [('iram', 0x40009000, 1024), ('psram', 0x04000000, 4*1024*1024),
                                          ('exception-stack', 0x40021000, 4096)]:
                    qmp.command('human-monitor-command', {'command-line':
                        f'pmemsave {addr:#x} {size} "{(out / (label+".bin")).as_posix()}"'})
                qmp.command('cont')
                print(f'Captured {second}s: {out}', flush=True)
            qmp.command('quit'); process.wait(timeout=10)
        finally:
            if qmp: qmp.close()
            if process.poll() is None: process.kill(); process.wait(timeout=10)


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    for arg in ('qemu', 'nor', 'binary', 'output'): p.add_argument('--'+arg, type=Path, required=True)
    p.add_argument('--rom',type=Path)
    p.add_argument('--rom-path', default='Super Metroid.sfc', help='ROM location inside the test card')
    p.add_argument('--data',type=Path)
    p.add_argument('--data-path',default='games')
    p.add_argument('--command',default='snes9x "/Super Metroid.sfc"')
    p.add_argument('--actions',type=Path)
    p.add_argument('--reuse-image', action='store_true', help='Boot the saved SD image again without recreating it')
    p.add_argument('--trace-int', action='store_true', help='Capture CPU exceptions for diagnosing startup faults')
    p.add_argument('--size', type=int, default=64, help='SD image size in MiB; allow space for game data and saves')
    p.add_argument('--seconds', type=int, default=30)
    main(p.parse_args())
