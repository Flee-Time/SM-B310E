#!/usr/bin/env python3
"""Boot the real ARM menu with JSON-only FAT32 cards and launch a ROM fixture.

The test supplies the SD loader's initial card state and entry PC using
QTest/GDB. It runs the unmodified built menu; it does not test the NOR loader.
The tiny emulator fixture checks the arguments handed to it, without using
any commercial game ROMs. A stock NOR dump is needed for fpdoom's board scan.
"""
import argparse
import json
import os
from pathlib import Path
import runpy
import socket
import struct
import subprocess
import time

REPO = Path(__file__).resolve().parents[3]
API = runpy.run_path(str(REPO / 'emulator/qemu/scripts/test-audio.py'))
IMAGE = runpy.run_path(str(REPO / 'ports/rockbox/tests/make-sd-image.py'))


class GDB:
    def __init__(self, port):
        self.sock = socket.create_connection(('127.0.0.1', port), timeout=5)

    def command(self, text):
        data = text.encode()
        self.sock.sendall(b'$' + data + b'#' + f'{sum(data) % 256:02x}'.encode())
        while self.sock.recv(1) != b'$':
            pass
        response = bytearray()
        while True:
            byte = self.sock.recv(1)
            if not byte:
                raise RuntimeError('GDB connection closed')
            if byte == b'#':
                break
            response += byte
        self.sock.recv(2)
        self.sock.sendall(b'+')
        return response.decode()

    def pc(self, addr):
        assert self.command('Pf=' + struct.pack('<I', addr).hex()) == 'OK'

    def close(self):
        self.sock.close()


def boot_case(args, name, config, symbols, fixture):
    out = args.output / name
    out.mkdir(parents=True, exist_ok=True)
    root = IMAGE['directory']()
    add = IMAGE['add_file']
    add(root, 'fpbin/fpmain.bin', args.fpmain.read_bytes())
    add(root, 'progs/fixture.bin', fixture)
    add(root, 'fpbin/emulator.bin', fixture)
    add(root, 'games/nes/Test Game.NES', b'ROM fixture; no executable game content')
    add(root, 'games/nes/readme.txt', b'not a ROM')
    # A directory ending in .nes must not become a selectable ROM.
    add(root, 'games/nes/Directory.nes/other.txt', b'directory filter fixture')
    if config is not None:
        add(root, 'fpbin/config.json', config.encode())
    IMAGE['write_image'](out / 'sd.img', root, 64, fat32=True)
    qmp_port, qt_port, gdb_port = [API['free_port']() for _ in range(3)]
    cmd = [str(args.qemu), '-M', 'b310e,boot-mode=rockbox,boot-overlays=off',
           '-S', '-display', 'none', '-serial', 'none', '-monitor', 'none',
           '-qmp', f'tcp:127.0.0.1:{qmp_port},server=on,wait=off',
           '-qtest', f'tcp:127.0.0.1:{qt_port},server=on,wait=off',
           '-qtest-log', str(out / 'qtest.log'), '-gdb', f'tcp:127.0.0.1:{gdb_port}',
           '-drive', f'file={args.fpmain.as_posix()},format=raw,if=none,id=os,readonly=on',
           '-drive', f'file={args.firmware.as_posix()},format=raw,if=none,id=nor,readonly=on',
           '-drive', f'file={(out / "sd.img").as_posix()},format=raw,if=none,id=sdcard',
           '-D', str(out / 'trace.log'), '--trace', 'sdhci_send_command',
           '--trace', 'sdhci_error', '--trace', 'sc6530_lcdc_refresh']
    (out / 'command.json').write_text(json.dumps(cmd, indent=2))
    env = os.environ.copy()
    if os.name == 'nt':
        env['PATH'] = r'C:\msys64\mingw64\bin;' + env.get('PATH', '')
    qmp = qt = gdb = None
    with (out / 'stderr.log').open('wb') as log:
        process = subprocess.Popen(cmd, stdout=log, stderr=log, env=env)
        try:
            for _ in range(100):
                if process.poll() is not None:
                    raise RuntimeError(f'QEMU exited; see {out}')
                try:
                    qmp = API['QMP'](qmp_port)
                    break
                except OSError:
                    time.sleep(.05)
            if qmp is None:
                raise RuntimeError('QMP startup timed out')
            qt, gdb = API['QTest'](qt_port), GDB(gdb_port)
            # Idle in IRAM while the virtual SD card is selected, as sdboot
            # would select it before entering the menu with LIBC_SDIO=3.
            qt.write(0x40007ff0, 0xeafffffe)
            gdb.pc(0x40007ff0)
            qmp.command('cont')
            qt.write(0x20700028, 0x0f00)
            qt.write(0x2070002c, 0xe0007)
            qt.write(0x20700034, 0xffffffff)

            def command(num, arg=0, flags=0x1a):
                qt.write(0x20700030, 0xffffffff)
                qt.write(0x20700008, arg)
                qt.write(0x2070000c, (num << 24) | (flags << 16))
                time.sleep(.015)
                status = qt.read(0x20700030)
                assert not status & 0x8000 and status & 1, (num, status)
                return qt.read(0x20700010)

            command(0, flags=0)
            command(8, 0x1aa)
            command(55, flags=2)
            command(41, 0x00ff8000, flags=2)
            command(2, flags=9)
            rca = command(3) & 0xffff0000
            command(9, rca, 9)
            command(7, rca, 0x1b)
            command(16, 512)
            qmp.command('stop')
            qt.write(0x40000000, 9)  # SDSC byte addressing, shared loader state
            qt.write(0x40000004, 0)
            gdb.pc(0x04000000)
            gdb.close()
            gdb = None
            qmp.command('cont')
            time.sleep(4)
            nitem = qt.read(symbols['s_nitem'])
            ncat = qt.read(symbols['s_ncat'])
            qmp.command('screendump', {'filename': str(out / 'menu.png'), 'format': 'png'})
            (out / 'registers.txt').write_text(qmp.hmp('info registers'))
            expected = (3, 1) if name == 'rom-discovery' else (2, 0)
            assert (nitem, ncat) == expected, (name, nitem, ncat, expected)
            if name == 'rom-discovery':
                assert qt.read(symbols['s_cat'] + 16) == 1  # only the regular .NES file
                for key in ['down', 'down', 'ret']:
                    qmp.hmp(f'sendkey {key} 100')
                    time.sleep(.4)
                qmp.command('screendump', {'filename': str(out / 'roms.png'), 'format': 'png'})
                qmp.hmp('sendkey ret 100')
            else:
                # Exercise the progs/ launch path even without valid JSON.
                for key in ['down', 'ret']:
                    qmp.hmp(f'sendkey {key} 100')
                    time.sleep(.4)
            time.sleep(1)
            qmp.command('screendump', {'filename': str(out / 'launched.png'), 'format': 'png'})
            (out / 'launch-registers.txt').write_text(qmp.hmp('info registers'))
            assert qt.read(0x40002000) == 0x424f4f54, 'selected binary did not launch fixture'
            if name == 'rom-discovery':
                data = bytes.fromhex(qt.command('read 0x40002004 0x100')[0].removeprefix('0x'))
                argc = struct.unpack_from('<H', data)[0]
                actual = [x.decode() for x in data[2:].split(b'\0')[:argc]]
                assert actual == ['--bright', '50', '--rotate', '2,0', '--dir',
                                  'games/nes', 'infones', 'Test Game.NES'], actual
                (out / 'launch-args.json').write_text(json.dumps(actual, indent=2))
            else:
                assert qt.read(0x40002004) & 0xffff == 0, 'progs launch must clear argv'
            qmp.command('quit')
            process.wait(timeout=10)
        finally:
            if gdb:
                gdb.close()
            if qt:
                qt.close()
            if qmp:
                qmp.close()
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=10)
    print(f'PASS ARM menu {name}: root={nitem}, categories={ncat}', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qemu', type=Path, required=True)
    parser.add_argument('--firmware', type=Path, required=True)
    parser.add_argument('--toolchain', type=Path, required=True)
    parser.add_argument('--fpmain', type=Path, default=REPO / 'sdcard/fpbin/fpmain.bin')
    parser.add_argument('--elf', type=Path, default=REPO / 'build/fpdoom/fpmain-b310e/obj3/fpmain_part1.elf')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--pad-loader', type=int, default=0,
                        help='Add word-aligned padding between loader and menu to test size independence')
    args = parser.parse_args()
    if args.pad_loader < 0 or args.pad_loader & 3:
        parser.error('--pad-loader must be a nonnegative multiple of four')
    for field in ['qemu', 'firmware', 'toolchain', 'fpmain', 'elf', 'output']:
        setattr(args, field, getattr(args, field).resolve())
    args.output.mkdir(parents=True, exist_ok=True)
    nm = args.toolchain / 'arm-none-eabi-nm'
    result = subprocess.run([str(nm), str(args.elf)], capture_output=True, text=True, check=True)
    packed = args.fpmain.read_bytes()
    part2_size = struct.unpack_from('<I', packed, 4)[0]
    if args.pad_loader:
        padded = bytearray(packed[:part2_size] + bytes(args.pad_loader) + packed[part2_size:])
        part2_size += args.pad_loader
        # The first ARM BL skips the loader and enters the relocated menu.
        struct.pack_into('<II', padded, 0, 0xeb000000 | ((part2_size - 8) // 4), part2_size)
        args.fpmain = args.output / 'fpmain-padded.bin'
        args.fpmain.write_bytes(padded)
    symbols = {row.split()[-1]: int(row.split()[0], 16) - 0x14000000 + 0x04000000 + part2_size
               for row in result.stdout.splitlines() if len(row.split()) == 3}
    asm = args.output / 'fixture.s'
    asm.write_text('''.syntax unified
.arch armv5te
.arm
.global _start
_start:
 ldr r0, =0x04020000
 ldr r1, [r0]
 ldr r2, =0x13579bdf
 cmp r1, r2
 bne 3f
 ldr r0, =0x40000004
 ldr r1, =0x40002004
 mov r2, #256
1: ldr r3, [r0], #4
 str r3, [r1], #4
 subs r2, r2, #4
 bne 1b
 ldr r0, =0x40002000
 ldr r1, =0x424f4f54
 str r1, [r0]
2: b 2b
3:
 ldr r0, =0x40002000
 ldr r1, =0x42414421
 str r1, [r0]
 b 2b
.ltorg
/* Force reads across many FAT clusters and verify the end reached PSRAM. */
.org 0x20000
.word 0x13579bdf
''')
    elf, binary = args.output / 'fixture.elf', args.output / 'fixture.bin'
    subprocess.run([str(args.toolchain / 'arm-none-eabi-gcc'), '-nostdlib', '-Wl,-Ttext=0x04000000',
                    str(asm), '-o', str(elf)], check=True)
    subprocess.run([str(args.toolchain / 'arm-none-eabi-objcopy'), '-O', 'binary', str(elf), str(binary)], check=True)
    config = json.dumps({'emulators': [{'name': 'NES', 'bin': 'fpbin/emulator.bin',
                         'directory': 'games/nes', 'extensions': ['.nes'],
                         'args': ['--dir', 'games/nes', 'infones']}],
                         'system': ['--bright', '50', '--rotate', '2,0']})
    for name, source in [('rom-discovery', config), ('empty-json', '{}'),
                         ('invalid-json', '{"categories":['), ('missing-json', None)]:
        boot_case(args, name, source, symbols, binary.read_bytes())


if __name__ == '__main__':
    main()
