#!/usr/bin/env python3
"""Boot the stock firmware patched by tools/dsp/stock-dsp-logger and check
its logger as a USB host.

The script enumerates the device the patched firmware presents (1782:3d00,
two vendor interfaces) and opens port 0, the logger's port (control request
0x22, wValue 0x0201).

The emulated USB controller does not carry the stock driver's bulk IN
transfers, so no log bytes arrive over USB. The script therefore reads the
logger's state from RAM (logger.c: struct state at 0x0423e728). It passes
when the device is the two-port UCOM one, the firmware takes the port-open
request, the logger has recorded the usbservice hello and the idle-loop
drain has handed a batch to the USB driver. DSP records depend on how far
the emulated layer 1 gets.
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import runpy
import socket
import struct
import subprocess
import time

HERE = Path(__file__).resolve().parent
usb_api = runpy.run_path(str(HERE / 'test-usb.py'))
spec = importlib.util.spec_from_file_location(
    'usblog', HERE.parents[2] / 'tools' / 'dsp' / 'stock-dsp-logger' / 'usblog.py')
usblog = importlib.util.module_from_spec(spec)
spec.loader.exec_module(usblog)


def enumerate_device(host):
    get = usb_api['get_descriptor']
    host.bus_reset()
    get(host, 1, length=64)
    host.bus_reset()
    host.control(0x00, 5, 7)
    dev = get(host, 1, length=18)
    vid, pid = struct.unpack_from('<HH', dev, 8)
    total = struct.unpack_from('<H', get(host, 2, length=9), 2)[0]
    config = get(host, 2, length=total)
    host.control(0x00, 9, config[5])
    return {'vid': f'{vid:04x}', 'pid': f'{pid:04x}', 'device': dev.hex(), 'config': config.hex(),
            'interfaces': usb_api['parse_config'](config)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qemu', type=Path, required=True)
    parser.add_argument('--firmware', type=Path, required=True, help='stock-usblog.bin from patch.py')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--attach-timeout', type=float, default=120)
    parser.add_argument('--seconds', type=float, default=60, help='how long to read the log')
    parser.add_argument('--plug-after', type=float, default=0,
                        help='boot without the cable and plug it in after this many seconds')
    parser.add_argument('--display', default='none', help='QEMU display, e.g. gtk to watch the phone')
    parser.add_argument('--hold-end', type=float, default=0,
                        help='hold the END key this many seconds from power-on (the stock wants it '
                             'to boot to its UI with a cable in)')
    parser.add_argument('--dsp-sniff', action='store_true',
                        help="the DSP model's own command log, to compare (it leaves the DSP "
                             'interrupt raised, which stalls the stock: no USB then)')
    parser.add_argument('--keep', action='store_true', help='leave QEMU running at the end (with a display)')
    args = parser.parse_args()
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)

    ports = []
    for _ in range(3):
        with socket.socket() as probe:
            probe.bind(('127.0.0.1', 0))
            ports.append(probe.getsockname()[1])
    qmp_port, usb_port, qtest_port = ports
    command = [str(args.qemu.resolve()), '-M', 'b310e', '-display', args.display, '-serial', 'none',
               '-monitor', 'none', '-audiodev', 'none,id=audio0,in.voices=0',
               '-global', 'sc6530_adi.audiodev=audio0',
               '-drive', f'if=none,id=nor,file={args.firmware.resolve().as_posix()},format=raw,readonly=on',
               '-qmp', f'tcp:127.0.0.1:{qmp_port},server=on,wait=off',
               '-qtest', f'tcp:127.0.0.1:{qtest_port},server=on,wait=off',
               '-chardev', f'socket,id=usbhost,host=127.0.0.1,port={usb_port},server=on,wait=off',
               '-global', 'sc6530_usb.chardev=usbhost',
               '-D', str(args.output / 'trace.log'), '--trace', 'sc6530_usb_setup']
    if args.dsp_sniff:
        command += ['--trace', 'sc6530_dsp_cmd']
    if not args.plug_after:
        command += ['-global', 'sc6530_adi.charger-present=on']
    (args.output / 'command.json').write_text(json.dumps(command, indent=2))
    env = os.environ.copy()
    if args.dsp_sniff:
        env['SC6530_DSP_SNIFF'] = '1'
    if os.name == 'nt':
        env['PATH'] = r'D:\Toolchains\msys64\mingw64\bin;C:\msys64\mingw64\bin;' + env.get('PATH', '')
    result = {'lines': []}
    qmp = None
    with (args.output / 'stderr.log').open('wb') as log:
        process = subprocess.Popen(command, env=env, stdout=log, stderr=log)
        try:
            for _ in range(100):
                if process.poll() is not None:
                    raise RuntimeError('QEMU exited; see stderr.log')
                try:
                    qmp = usb_api['capture'].QMP(qmp_port)
                    break
                except OSError:
                    time.sleep(.1)
            if qmp is None:
                raise RuntimeError('QMP startup timeout')
            if args.hold_end:
                qmp.hmp(f'sendkey esc {int(args.hold_end * 1000)}')
            host = usb_api['Host'](usb_port)
            if args.plug_after:
                time.sleep(args.plug_after)
                qmp.command('screendump', {'filename': str(args.output / 'before-plug.png'), 'format': 'png'})
                qt = usb_api['qtest_api']['QTest'](qtest_port)
                qt.command('set_irq_in /machine/peripheral/sc6530-adi charger-input 0 1')
                qt.close()
            started = time.monotonic()
            try:
                host.wait_attached(args.attach_timeout)
            except Exception:
                state = {name: qmp.hmp(f'xp /{count}wx {addr:#x}') for name, addr, count in [
                    ('usb', 0x20300000, 8), ('usb_service', 0x0422e7d4, 4), ('ucom', 0x0422e7a0, 12),
                    ('sio_data', 0x0425118c, 4), ('sio_debug', 0x04251220, 4)]}
                state['registers'] = qmp.hmp('info registers')
                (args.output / 'failure-state.json').write_text(json.dumps(state, indent=2))
                qmp.command('screendump', {'filename': str(args.output / 'failure.png'), 'format': 'png'})
                raise
            result['attach_seconds'] = round(time.monotonic() - started, 2)
            info = enumerate_device(host)
            result['device'] = info
            host.control(0x21, 0x22, 0x0201, 0, b'')     # open port 0, the logger's
            time.sleep(0.3)
            result['port_state'] = int(qmp.hmp('xp /1wx 0x04277804').split(':')[1], 16)
            endpoints = [e['address'] & 0x7f for i in info['interfaces'] for e in i['endpoints']
                         if e['address'] & 0x80]
            streams = {ep: bytearray() for ep in endpoints}
            deframers = {ep: usblog.Deframer() for ep in endpoints}
            deadline = time.monotonic() + args.seconds
            while time.monotonic() < deadline:
                idle = True
                for ep in endpoints:
                    code, data = host._request('I', ep, struct.pack('<H', 64))
                    if code == 'D' and data:
                        idle = False
                        streams[ep] += data
                        for frame in deframers[ep].feed(data):
                            for line in usblog.lines(frame, True):
                                result['lines'].append({'ep': ep, 'text': line})
                                print(f'ep{ep}: {line}', flush=True)
                if idle:
                    time.sleep(0.01)
            for ep, data in streams.items():
                (args.output / f'ep{ep}.raw').write_bytes(data)
            result['bytes'] = {str(ep): len(data) for ep, data in streams.items()}
            dump = args.output / 'logger-state.bin'
            qmp.command('pmemsave', {'val': 0x0423e728, 'size': 0x23a0, 'filename': str(dump)})
            state = dump.read_bytes()
            head, tail, dropped, busy, _, tx_len, tx_pos = struct.unpack_from('<7I', state, 0)
            text = state[0x20:0x20 + min(tx_len, 0x600)].decode('ascii', 'replace')
            result['logger'] = {'head': head, 'tail': tail, 'dropped': dropped, 'tx_busy': busy,
                                'tx_len': tx_len, 'tx_pos': tx_pos,
                                'tx_lines': text.split()[:0] + text.strip().splitlines()}
            qmp.command('screendump', {'filename': str(args.output / 'screen.png'), 'format': 'png'})
            if not args.keep:
                qmp.command('quit')
                process.wait(timeout=20)
        finally:
            if qmp:
                qmp.close()
            if process.poll() is None and not args.keep:
                process.terminate()
                process.wait(timeout=10)
    (args.output / 'result.json').write_text(json.dumps(result, indent=2))
    print(json.dumps({k: v for k, v in result.items() if k != 'lines'}, indent=2), flush=True)
    device = result['device']
    logger = result['logger']
    for text in logger['tx_lines']:
        print(text)
    assert (device['vid'], device['pid']) == ('1782', '3d00') and len(device['interfaces']) == 2, device
    assert result['port_state'] == 2, 'the firmware did not take the port-open request'
    assert any(text.startswith('~H ') and text.endswith('v6') for text in logger['tx_lines']), \
        'no version 6 hello line in the transmit buffer'
    assert logger['tail'] > 0, 'the idle-loop drain sent nothing'
    print(f'PASS logger: {logger["head"]} records, {logger["tail"]} handed to USB, '
          f'{logger["dropped"]} dropped; bytes over USB: {sum(result["bytes"].values())}')

if __name__ == '__main__':
    main()
