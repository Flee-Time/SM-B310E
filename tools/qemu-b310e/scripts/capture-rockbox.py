#!/usr/bin/env python3
"""Boot the complete Rockbox image with an SD backend and capture UI/audio."""
import argparse
import importlib.util
import json
import os
import math
import re
import sys
from pathlib import Path
import socket
import subprocess
import time
import wave
from array import array

spec = importlib.util.spec_from_file_location('stock_capture', Path(__file__).with_name('capture-stock.py'))
capture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(capture)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qemu', type=Path, required=True)
    parser.add_argument('--rockbox', type=Path, required=True)
    parser.add_argument('--sdcard', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--seconds', type=float, default=25)
    parser.add_argument('--key', action='append', default=[], help='seconds:key[:hold_ms]')
    parser.add_argument('--headset', action='store_true')
    parser.add_argument('--icount', action='store_true', help='deterministic guest instruction timing')
    parser.add_argument('--verify-test-tone', action='store_true',
                        help='Require the generated stereo 440/660 Hz test track in WAV output')
    args = parser.parse_args()
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    with socket.socket() as probe:
        probe.bind(('127.0.0.1', 0))
        port = probe.getsockname()[1]
    command = [str(args.qemu.resolve()), '-M', 'b310e,boot-mode=rockbox',
               '-display', 'none', '-serial', 'none', '-monitor', 'none',
               '-drive', f'file={args.rockbox.resolve().as_posix()},format=raw,if=none,id=os,readonly=on',
               '-drive', f'file={args.sdcard.resolve().as_posix()},format=raw,if=none,id=sdcard',
               '-qmp', f'tcp:127.0.0.1:{port},server=on,wait=off',
               '-audiodev', f'wav,id=audio0,path={(args.output/"audio.wav").as_posix()},out.frequency=44100',
               '-global', 'sc6530_adi.audiodev=audio0', '-D', str(args.output/'trace.log')]
    for pattern in ['sc6530_dma_*', 'sc6530_vbc_*', 'sc6530_ana_write', 'sdhci_*']:
        command += ['--trace', pattern]
    if args.icount:
        command += ['-icount', 'shift=3,sleep=off']
    if args.headset:
        command += ['-global', 'sc6530_aux.headset-present=on']
    (args.output/'command.json').write_text(json.dumps(command, indent=2))
    env = os.environ.copy()
    if os.name == 'nt':
        env['PATH'] = r'C:\msys64\mingw64\bin;' + env.get('PATH', '')
    qmp = None
    snapshots = []
    with (args.output/'stderr.log').open('wb') as log:
        process = subprocess.Popen(command, env=env, stdout=log, stderr=log)
        try:
            for _ in range(100):
                if process.poll() is not None:
                    raise RuntimeError('QEMU exited; see stderr.log')
                try:
                    qmp = capture.QMP(port)
                    break
                except OSError:
                    time.sleep(.1)
            if qmp is None:
                raise RuntimeError('QMP startup timeout')
            started = time.monotonic()
            def snapshot(name):
                qmp.command('stop')
                state = {'name': name, 'wall_seconds': time.monotonic()-started,
                         'registers': qmp.hmp('info registers'),
                         'sdio': qmp.hmp('xp /8wx 0x20700024'),
                         'dma': qmp.hmp('xp /16wx 0x20101080'),
                         'dma_routes': qmp.hmp('xp /4wx 0x20102030'),
                         'vbc': qmp.hmp('xp /20wx 0x82003010'),
                         'codec': qmp.hmp('xp /16wx 0x8a002000'),
                         'lcdc': qmp.hmp('xp /48wx 0x20d00000'),
                         'intc': qmp.hmp('xp /4wx 0x80000000')}
                snapshots.append(state)
                qmp.command('screendump', {'filename': str(args.output/(name+'.png')), 'format': 'png'})
                qmp.command('cont')
            keys = []
            for item in args.key:
                fields = item.split(':')
                if len(fields) not in (2, 3):
                    raise ValueError('--key must be seconds:key[:hold_ms]')
                when, key = float(fields[0]), fields[1]
                hold = int(fields[2]) if len(fields) == 3 else 100
                if (not 0 <= when < args.seconds or not 1 <= hold <= 10000 or
                        not re.fullmatch(r'[a-z0-9_-]+', key)):
                    raise ValueError('--key requires a valid time, key and 1..10000 ms hold')
                keys.append((when, key, hold))
            snapshot('startup')
            for index, (when, key, hold) in enumerate(sorted(keys)):
                time.sleep(max(0, when-(time.monotonic()-started)))
                snapshot(f'before-{index}')
                qmp.hmp(f'sendkey {key} {hold}')
                time.sleep(max(1, hold/1000+.1))
                snapshot(f'key-{index}-{key}')
            time.sleep(max(0, args.seconds-(time.monotonic()-started)))
            snapshot('final')
            qmp.command('quit')
            process.wait(timeout=10)
        finally:
            if qmp:
                qmp.close()
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=10)
    (args.output/'state.json').write_text(json.dumps(snapshots, indent=2))
    with wave.open(str(args.output/'audio.wav'), 'rb') as wav:
        assert wav.getnchannels() == 2 and wav.getsampwidth() == 2
        pcm = array('h', wav.readframes(wav.getnframes()))
        if sys.byteorder != 'little':
            pcm.byteswap()
        stats = {'frames': len(pcm)//2, 'rate': wav.getframerate(),
                 'nonzero_samples': sum(x != 0 for x in pcm),
                 'peak': max((abs(x) for x in pcm), default=0)}
    if args.verify_test_tone:
        assert stats['rate'] == 44100 and stats['nonzero_samples'] > 44100 * 2
        # Skip the short fade and use a complete second of decoded audio.
        first = next(i // 2 for i, value in enumerate(pcm) if value)
        start = (first + 882) * 2
        tones = []
        for channel, expected in [(0, 440), (1, 660)]:
            samples = pcm[start+channel:start+88200:2]
            assert len(samples) == 44100
            def amplitude(hz):
                omega = 2 * math.pi * hz / 44100
                real = sum(v * math.cos(omega*i) for i, v in enumerate(samples))
                imag = sum(v * math.sin(omega*i) for i, v in enumerate(samples))
                return 2 * math.hypot(real, imag) / len(samples)
            signal, cross = amplitude(expected), amplitude(1100-expected)
            assert 2000 < signal < 4500 and cross < signal / 100, (channel, signal, cross)
            tones.append({'channel': channel, 'frequency': expected,
                          'amplitude': signal, 'other_channel_tone': cross})
        stats['test_tones'] = tones
    (args.output/'audio.json').write_text(json.dumps(stats, indent=2))
    print(json.dumps(stats), flush=True)


if __name__ == '__main__':
    main()
