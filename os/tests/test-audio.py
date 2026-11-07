#!/usr/bin/env python3
"""Audio in the B310E OS, in QEMU: playback, recording, FM, headset.

With a 4-pole headset plugged in (sc6530_aux.headset-present,
sc6530_adi.headset-mic) and the audio written to a WAV by QEMU:
  - the headset check enables its buttons; holding the centre button
    ("h") reaches the OS as KEY_HS_CENTER;
  - Sound -> Test tone plays a 1 kHz tone (checked in QEMU's output);
  - Sound -> Record mic records the model's 1 kHz mic tone to
    /b310e-os/rec/REC0001.WAV (checked in the card image), and
    Play last recording plays it back;
  - FM radio powers the RTC6211N on I2C 1, seeks to a station and routes
    the line-in to the headphones (the model's stations: 92.3, 100.0 MHz);
then reads the card log for each step.

SPDX-License-Identifier: GPL-2.0-or-later
"""
import argparse
from array import array
import importlib.util
import json
import os
from pathlib import Path
import runpy
import socket
import struct
import subprocess
import sys
import time
import wave

REPO = Path(__file__).resolve().parents[2]
QT = runpy.run_path(str(REPO / 'emulator/qemu/scripts/test-audio.py'))['QTest']
spec = importlib.util.spec_from_file_location(
    'capture', REPO / 'emulator/qemu/scripts/capture-stock.py')
capture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(capture)
boot = runpy.run_path(str(Path(__file__).with_name('test-boot.py')))
FIELDS, MAGIC, MENU = boot['FIELDS'], boot['MAGIC'], boot['MENU']
symbol, status, string = boot['symbol'], boot['status'], boot['string']
wait_for, free_port = boot['wait_for'], boot['free_port']
card_file = boot['card_file']
KEY_HS_CENTER = 0x70


def tone_hz(pcm, rate):
    """Frequency by zero crossings of the loudest second of a mono list."""
    crossings = sum(1 for a, b in zip(pcm, pcm[1:]) if (a < 0) != (b < 0))
    return crossings / 2 / (len(pcm) / rate) if pcm else 0


def loud_section(pcm, rate, threshold=500):
    """The samples between the first and last of a run above threshold."""
    idx = [i for i, v in enumerate(pcm) if abs(v) > threshold]
    return pcm[idx[0]:idx[-1]] if idx else []


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qemu', type=Path, default=REPO / 'build/qemu/build/qemu-system-arm.exe')
    parser.add_argument('--elf', type=Path, default=REPO / 'build/os/b310e-os.elf')
    parser.add_argument('--image', type=Path, default=REPO / 'build/os/b310e-os.bin')
    parser.add_argument('--output', type=Path, default=REPO / 'build/os/test-audio')
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    base = symbol(args.elf, 'os_status')
    card = out / 'card.img'
    subprocess.run([sys.executable, str(REPO / 'ports/rockbox/tests/make-sd-image.py'),
                    '--fat32', '--size-mib', '64', '--seconds', '1', '--force',
                    '--runtime', str(REPO / 'sdcard/.rockbox'),
                    '--rockbox', str(REPO / 'sdcard/progs/rockbox.bin'),
                    '--output', str(card)], check=True, capture_output=True)
    wav_out = out / 'audio.wav'
    qmp_port, qtest_port = free_port(), free_port()
    command = [str(args.qemu.resolve()), '-M', 'b310e,boot-mode=rockbox',
               '-display', 'none', '-serial', 'none', '-monitor', 'none',
               '-drive', f'file={args.image.resolve().as_posix()},format=raw,if=none,id=os,readonly=on',
               '-drive', f'file={card.as_posix()},format=raw,if=none,id=sdcard',
               '-qmp', f'tcp:127.0.0.1:{qmp_port},server=on,wait=off',
               '-qtest', f'tcp:127.0.0.1:{qtest_port},server=on,wait=off',
               '-global', 'sc6530_aux.headset-present=on',
               '-global', 'sc6530_adi.headset-mic=on',
               '-audiodev', f'wav,id=audio0,path={wav_out.as_posix()},out.frequency=44100,'
                            'out.channels=2,out.format=s16',
               '-global', 'sc6530_adi.audiodev=audio0',
               '--trace', 'sc6530_fm_*', '--trace', 'sc6530_i2c_cmd',
               '-D', str(out / 'qemu.log')]
    env = os.environ.copy()
    if os.name == 'nt':
        env['PATH'] = r'D:\Toolchains\msys64\mingw64\bin;C:\msys64\mingw64\bin;' + env.get('PATH', '')
    result = {}
    marks = {}
    t0 = None
    with (out / 'stderr.log').open('wb') as log:
        process = subprocess.Popen(command, env=env, stdout=log, stderr=log)
        qmp = qt = None
        try:
            for _ in range(100):
                try:
                    qmp = capture.QMP(qmp_port)
                    qt = QT(qtest_port)
                    break
                except OSError:
                    time.sleep(0.1)
            assert qmp and qt, 'QEMU did not start'

            def title():
                return string(qt, status(qt, base)['ui_title'])

            def press(key, want, hold=100, seconds=6):
                qmp.hmp(f'sendkey {key} {hold}')
                end = time.monotonic() + seconds
                while time.monotonic() < end:
                    if title() == want:
                        time.sleep(0.4)
                        return status(qt, base)
                    time.sleep(0.1)
                raise AssertionError(f'after {key}: screen {title()!r}, wanted {want!r}')

            def shot(name):
                qmp.command('screendump', {'filename': str(out / f'{name}.png'), 'format': 'png'})

            def menu(entry):
                press('esc', 'B310E OS')
                press('ret', 'Menu')
                for _ in range(MENU.index(entry)):
                    press('down', 'Menu')
                return press('ret', entry)

            wait_for(qt, base, lambda s: s['magic'] == MAGIC, 10, 'OS never started')
            wait_for(qt, base, lambda s: s['ui_depth'] == 1, 60, 'no UI')
            t0 = time.monotonic()
            time.sleep(1.5)                     # the headset check: 400 ms

            # headset centre button
            qmp.hmp('sendkey h 300')
            s = wait_for(qt, base, lambda s: (s['last_key'] & 0xff) == KEY_HS_CENTER, 5,
                         'headset button never arrived')
            result['headset_key'] = hex(s['last_key'])

            # tone
            menu('Sound')
            press('ret', 'Sound')               # Test tone on
            marks['tone'] = time.monotonic() - t0
            time.sleep(1.5)
            shot('sound-tone')
            press('ret', 'Sound')               # off
            # record the mic
            for _ in range(3):
                press('down', 'Sound')
            press('ret', 'Sound')               # Record mic
            time.sleep(2.5)
            shot('sound-rec')
            press('ret', 'Sound')               # stop
            press('up', 'Sound')                # Play last recording
            press('ret', 'Sound')
            marks['playback'] = time.monotonic() - t0
            time.sleep(2.0)
            press('f2', 'Menu')                 # leaving stops it

            # FM
            press('f2', 'B310E OS')
            menu('FM radio')
            time.sleep(4)                       # probe, power-up, tune
            press('f1', 'FM radio')             # record the radio
            time.sleep(1)
            # Seeking while recording once blocked the UI for minutes on the
            # phone: the UI must keep taking keys during a seek.
            k0 = status(qt, base)['keys']
            qmp.hmp('sendkey up 100')           # seek up
            time.sleep(0.3)
            qmp.hmp('sendkey asterisk 100')     # volume down, and stops the seek
            s = wait_for(qt, base, lambda s: s['keys'] >= k0 + 2, 3,
                         'the UI took no keys during a seek')
            result['keys_during_seek'] = s['keys'] - k0
            press('up', 'FM radio')             # a full seek to a station
            time.sleep(4)
            shot('radio')
            press('f1', 'FM radio')             # stop recording
            press('f2', 'Menu')
            time.sleep(2.5)                     # the card log is written each second
        except Exception:
            print(json.dumps(result, indent=2))
            raise
        finally:
            if qt:
                qt.close()
            if qmp:
                try:
                    qmp.command('quit')         # QEMU finishes the WAV header
                    process.wait(timeout=10)
                except Exception:
                    pass
                qmp.close()
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=10)

    # QEMU's output: a 1 kHz tone first
    with wave.open(str(wav_out), 'rb') as w:
        rate = w.getframerate()
        pcm = array('h', w.readframes(w.getnframes()))
    left = list(pcm[0::2])
    loud = loud_section(left, rate)
    result['output_seconds_loud'] = round(len(loud) / rate, 2)
    first = loud[:rate // 2]
    result['tone_hz'] = round(tone_hz(first, rate))
    assert 900 < result['tone_hz'] < 1100, result

    # the recording on the card
    image = card.read_bytes()
    at = image.find(b'RIFF')
    found = None
    while at >= 0:
        if image[at + 8:at + 16] == b'WAVEfmt ' and struct.unpack_from('<I', image, at + 24)[0] == 16000:
            found = at
            break
        at = image.find(b'RIFF', at + 4)
    assert found is not None, 'no 16 kHz recording on the card'
    size = struct.unpack_from('<I', image, found + 40)[0]
    rec = array('h', image[found + 44:found + 44 + size])
    result['recorded_seconds'] = round(len(rec) / 16000, 2)
    result['recorded_peak'] = max(abs(v) for v in rec) if rec else 0
    result['recorded_hz'] = round(tone_hz(list(rec[len(rec) // 4:len(rec) // 4 + 8000]), 16000))
    assert result['recorded_seconds'] > 1.0 and result['recorded_peak'] > 500, result
    assert 900 < result['recorded_hz'] < 1100, result

    # the card log
    # read through the FAT: the recording folder fragments the log
    log_text = (card_file(image, '/b310e-os/log.txt') or b'').decode(errors='replace')
    (out / 'card-log.txt').write_text(log_text)
    result['log'] = [l for l in log_text.splitlines() if 'audio:' in l or 'fm:' in l]
    assert 'audio: recording /b310e-os/rec/REC0001.WAV' in log_text, log_text
    assert 'audio: recorded' in log_text, log_text
    fm_lines = [l.split(' fm: ', 1)[1] for l in log_text.splitlines() if ' fm: id 1688' in l]
    assert fm_lines, 'no FM probe in the log'
    result['fm_log'] = 'fm: ' + fm_lines[0]
    assert 'present 1' in result['fm_log'], result
    at = image.find(b'WAVEfmt ')
    fm_rec = None
    while at >= 0:
        if struct.unpack_from('<I', image, at + 16)[0] == 32000:
            fm_rec = struct.unpack_from('<I', image, at + 32)[0]
            break
        at = image.find(b'WAVEfmt ', at + 8)
    result['fm_recorded_bytes'] = fm_rec
    assert fm_rec and fm_rec > 32000, ('no FM recording on the card', result)

    # FM trace: the tuner was started and tuned
    trace = (out / 'qemu.log').read_text(errors='replace')
    result['fm_trace_lines'] = sum(1 for l in trace.splitlines() if 'sc6530_fm' in l)
    assert result['fm_trace_lines'] > 0, 'no FM tuner activity in the trace'
    (out / 'result.json').write_text(json.dumps(result, indent=2))
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
