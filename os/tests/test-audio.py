#!/usr/bin/env python3
"""Audio in the B310E OS, in QEMU: playback, recording, FM, headset, through
the Recorder and FM radio apps (os/modules/recorder, os/modules/radio).

With a 4-pole headset plugged in (sc6530_aux.headset-present,
sc6530_adi.headset-mic) and the audio written to a WAV by QEMU:
  - the headset check enables its buttons; holding the centre button
    ("h") reaches the OS as KEY_HS_CENTER;
  - Recorder -> Recordings plays a known 1 kHz WAV put on the card
    (/b310e-os/rec/TONE1K.WAV, checked in QEMU's output);
  - Recorder records the model's 1 kHz mic tone to
    /b310e-os/rec/REC0001.WAV (checked in the card image), and the
    Recordings list plays it back;
  - a second recording goes on while the app is closed (the OS writes it)
    and is still running when the Recorder opens again;
  - FM radio powers the RTC6211N on I2C 1, records the radio (32 kHz),
    keeps taking keys during a seek, seeks to a station (the model's:
    92.3, 100.0 MHz), saves it as preset 1 and tunes back to it with the
    1 key; * lowers the music volume; the radio plays on after Back and
    Options -> Turn off ends it;
then reads the card log, the settings and the radio's file.

SPDX-License-Identifier: GPL-2.0-or-later
"""
import argparse
from array import array
import importlib.util
import json
import math
import os
from pathlib import Path
import runpy
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
FIELDS, MAGIC = boot['FIELDS'], boot['MAGIC']
symbol, status, string = boot['symbol'], boot['status'], boot['string']
wait_for, free_port = boot['wait_for'], boot['free_port']
card_file = boot['card_file']
KEY_HS_CENTER = 0x70
APPS = ['FM radio', 'Recorder']             # radio.app, recorder.app: the card's order
RADIO_OPTIONS = ['Record', 'Presets', 'Save preset', 'Use speaker', 'Turn off']
TONE_RATE = 22050                           # not 16 kHz: no mix-up with a recording


def tone_hz(pcm, rate):
    """Frequency by zero crossings of the loudest second of a mono list."""
    crossings = sum(1 for a, b in zip(pcm, pcm[1:]) if (a < 0) != (b < 0))
    return crossings / 2 / (len(pcm) / rate) if pcm else 0


def loud_section(pcm, rate, threshold=500):
    """The samples between the first and last of a run above threshold."""
    idx = [i for i, v in enumerate(pcm) if abs(v) > threshold]
    return pcm[idx[0]:idx[-1]] if idx else []


def tone_wav(path, hz, seconds, rate):
    with wave.open(str(path), 'wb') as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(b''.join(struct.pack('<h', int(16000 * math.sin(2 * math.pi * hz * i / rate)))
                               for i in range(int(rate * seconds))))


def wav_info(data):
    """(rate, data bytes, mono samples) of one of our 44-byte-header WAVs."""
    assert data and data[:4] == b'RIFF' and data[8:16] == b'WAVEfmt ', 'not a WAV'
    rate = struct.unpack_from('<I', data, 24)[0]
    size = struct.unpack_from('<I', data, 40)[0]
    return rate, size, array('h', data[44:44 + size])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qemu', type=Path, default=REPO / 'build/qemu/build/qemu-system-arm.exe')
    parser.add_argument('--build', type=Path, default=REPO / 'build/os')
    parser.add_argument('--output', type=Path, default=None)
    args = parser.parse_args()
    out = (args.output or args.build / 'test-audio').resolve()
    out.mkdir(parents=True, exist_ok=True)
    elf, image = args.build / 'b310e-os.elf', args.build / 'b310e-os.bin'
    base = symbol(elf, 'os_status')
    s_playing, s_recording = symbol(elf, 'playing'), symbol(elf, 'recording')  # apps/wavio.c
    s_fm_on, s_freq = symbol(elf, 'is_on'), symbol(elf, 'freq_khz')           # drivers/fm.c
    s_speaker = symbol(elf, 'speaker_enabled')                                  # drivers/audio.c
    card = out / 'card.img'
    tone_wav(out / 'TONE1K.WAV', 1000, 2.0, TONE_RATE)
    mods = args.build / 'modules'
    subprocess.run([sys.executable, str(REPO / 'ports/rockbox/tests/make-sd-image.py'),
                    '--fat32', '--size-mib', '64', '--seconds', '1', '--force',
                    '--runtime', str(REPO / 'sdcard/.rockbox'),
                    '--rockbox', str(REPO / 'sdcard/payloads/rockbox.bin'),
                    '--add', f'b310e-os/apps/radio.app={(mods / "radio.app").resolve()}',
                    '--add', f'b310e-os/apps/recorder.app={(mods / "recorder.app").resolve()}',
                    '--add', f'b310e-os/rec/TONE1K.WAV={out / "TONE1K.WAV"}',
                    '--output', str(card)], check=True, capture_output=True)
    wav_out = out / 'audio.wav'
    qmp_port, qtest_port = free_port(), free_port()
    command = [str(args.qemu.resolve()), '-M', 'b310e,boot-mode=rockbox',
               '-display', 'none', '-serial', 'none', '-monitor', 'none',
               '-drive', f'file={image.resolve().as_posix()},format=raw,if=none,id=os,readonly=on',
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

            def flag(addr):
                return qt.read(addr) & 0xff

            def app(name):
                press('esc', 'B310E OS')
                press('ret', 'Menu')
                press('ret', 'Apps')                # the first entry
                for _ in range(APPS.index(name)):
                    press('down', 'Apps')
                return press('ret', name, seconds=10)

            def radio_option(name, want='FM radio'):
                press('f1', 'Options')
                for _ in range(RADIO_OPTIONS.index(name)):
                    press('down', 'Options')
                return press('ret', want)

            wait_for(qt, base, lambda s: s['magic'] == MAGIC, 10, 'OS never started')
            wait_for(qt, base, lambda s: s['ui_depth'] == 1, 60, 'no UI')
            t0 = time.monotonic()
            time.sleep(1.5)                     # the headset check: 400 ms

            # headset centre button
            qmp.hmp('sendkey h 300')
            s = wait_for(qt, base, lambda s: (s['last_key'] & 0xff) == KEY_HS_CENTER, 5,
                         'headset button never arrived')
            result['headset_key'] = hex(s['last_key'])

            # a known WAV from the Recorder's list
            app('Recorder')
            shot('recorder')
            s = press('f1', 'Recordings')
            result['recordings_at_start'] = s['ui_list_count']
            assert s['ui_list_count'] == 1, s   # TONE1K.WAV
            press('ret', 'Recordings')          # play it
            marks['tone'] = time.monotonic() - t0
            result['tone_playing'] = flag(s_playing)
            time.sleep(1.5)
            shot('recordings-playing')
            press('ret', 'Recordings')          # stop
            result['tone_stopped'] = flag(s_playing) == 0
            assert result['tone_playing'] == 1 and result['tone_stopped'], result

            # record the mic
            press('f2', 'Recorder')
            press('ret', 'Recorder')            # Rec
            result['mic_recording'] = flag(s_recording)
            time.sleep(2.5)
            shot('recorder-rec')
            press('ret', 'Recorder')            # Stop
            assert result['mic_recording'] == 1 and flag(s_recording) == 0, result
            s = press('f1', 'Recordings')       # REC0001.WAV first, then TONE1K.WAV
            result['recordings_after'] = s['ui_list_count']
            assert s['ui_list_count'] == 2, s
            press('ret', 'Recordings')          # play it back
            marks['playback'] = time.monotonic() - t0
            result['playback_playing'] = flag(s_playing)
            time.sleep(2.0)
            shot('recordings')
            press('f2', 'Recorder')
            press('f2', 'Apps')                 # leaving stops the playback
            wait_for(qt, base, lambda s: s['apps_loaded'] == 0, 5, 'Recorder not freed')
            result['playback_stopped_on_leave'] = flag(s_playing) == 0
            assert result['playback_playing'] == 1 and result['playback_stopped_on_leave'], result

            # a recording goes on with the app closed
            app('Recorder')
            press('ret', 'Recorder')            # Rec: REC0002.WAV
            press('f2', 'Apps')
            press('esc', 'B310E OS')
            wait_for(qt, base, lambda s: s['apps_loaded'] == 0, 5, 'Recorder not freed')
            time.sleep(3)
            result['recording_while_closed'] = flag(s_recording)
            assert result['recording_while_closed'] == 1, result
            app('Recorder')
            time.sleep(0.5)
            shot('recorder-back')
            result['recording_on_return'] = flag(s_recording)
            press('ret', 'Recorder')            # Stop
            assert result['recording_on_return'] == 1 and flag(s_recording) == 0, result

            # FM
            app('FM radio')
            time.sleep(4)                       # probe, power-up, tune
            result['fm_on'] = flag(s_fm_on)
            assert result['fm_on'] == 1, result
            radio_option('Record')              # record the radio: REC0003.WAV
            assert flag(s_recording) == 1, 'the radio is not recording'
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
            press('up', 'FM radio')             # a full seek to a station: round
            end = time.monotonic() + 30         # the band's top, a channel a tick
            while time.monotonic() < end and qt.read(s_freq) not in (92300, 100000):
                time.sleep(0.1)
            time.sleep(1)                       # it stays there
            result['station_khz'] = qt.read(s_freq)
            assert result['station_khz'] in (92300, 100000), result
            shot('radio')
            radio_option('Record')              # "Stop recording" in its place
            assert flag(s_recording) == 0, 'the FM recording did not stop'
            # the station as preset 1, a step away, back with the 1 key
            press('f1', 'Options')
            for _ in range(RADIO_OPTIONS.index('Save preset')):
                press('down', 'Options')
            press('ret', 'Save to')
            shot('radio-save-preset')
            press('ret', 'FM radio')            # slot 1
            press('right', 'FM radio')
            result['tuned_away_khz'] = qt.read(s_freq)
            press('1', 'FM radio')
            time.sleep(0.3)
            result['preset_khz'] = qt.read(s_freq)
            assert result['tuned_away_khz'] == result['station_khz'] + 100, result
            assert result['preset_khz'] == result['station_khz'], result
            shot('radio-preset')
            # the speaker with the headset (the antenna) in, and back
            result['speaker_with_headset'] = flag(s_speaker)
            radio_option('Use speaker')
            time.sleep(0.3)
            result['speaker_forced'] = flag(s_speaker)
            shot('radio-speaker')
            radio_option('Use speaker')         # "Use headphones" in its place
            time.sleep(0.3)
            result['speaker_released'] = flag(s_speaker)
            assert (result['speaker_with_headset'], result['speaker_forced'],
                    result['speaker_released']) == (0, 1, 0), result
            # Back: the radio plays on; Options -> Turn off ends it
            press('f2', 'Apps')
            time.sleep(0.5)                     # its notification keeps it loaded
            result['radio_kept'] = status(qt, base)['apps_loaded']
            time.sleep(1)
            result['fm_on_after_back'] = flag(s_fm_on)
            press('ret', 'FM radio')            # the Apps list kept its row
            time.sleep(0.5)
            radio_option('Turn off', 'Apps')
            time.sleep(0.5)
            result['fm_on_after_turn_off'] = flag(s_fm_on)
            assert result['fm_on_after_back'] == 1 and result['fm_on_after_turn_off'] == 0, result
            assert result['radio_kept'] == 1, result
            # off: the notification goes at the next second, then the app
            wait_for(qt, base, lambda s: s['apps_loaded'] == 0, 5, 'FM radio not freed when off')
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

    # QEMU's output: the 1 kHz WAV first
    with wave.open(str(wav_out), 'rb') as w:
        rate = w.getframerate()
        pcm = array('h', w.readframes(w.getnframes()))
    left = list(pcm[0::2])
    loud = loud_section(left, rate)
    result['output_seconds_loud'] = round(len(loud) / rate, 2)
    first = loud[:rate // 2]
    result['tone_hz'] = round(tone_hz(first, rate))
    assert 900 < result['tone_hz'] < 1100, result

    # the recordings on the card
    image = card.read_bytes()
    r, size, rec = wav_info(card_file(image, '/b310e-os/rec/REC0001.WAV'))
    result['recorded_rate'] = r
    result['recorded_seconds'] = round(len(rec) / 16000, 2)
    result['recorded_peak'] = max(abs(v) for v in rec) if rec else 0
    result['recorded_hz'] = round(tone_hz(list(rec[len(rec) // 4:len(rec) // 4 + 8000]), 16000))
    assert r == 16000, result
    assert result['recorded_seconds'] > 1.0 and result['recorded_peak'] > 500, result
    assert 900 < result['recorded_hz'] < 1100, result
    r, size, rec = wav_info(card_file(image, '/b310e-os/rec/REC0002.WAV'))
    result['background_seconds'] = round(len(rec) / 16000, 2)
    assert r == 16000 and result['background_seconds'] > 3.0, result
    r, size, rec = wav_info(card_file(image, '/b310e-os/rec/REC0003.WAV'))
    result['fm_recorded'] = (r, size)
    assert r == 32000 and size > 32000, ('no FM recording on the card', result)

    # the card log
    # read through the FAT: the recording folder fragments the log
    log_text = (card_file(image, '/b310e-os/log.txt') or b'').decode(errors='replace')
    (out / 'card-log.txt').write_text(log_text)
    result['log'] = [l for l in log_text.splitlines()
                     if 'audio:' in l or 'fm:' in l or 'recorder:' in l or 'radio:' in l]
    assert 'audio: playing /b310e-os/rec/TONE1K.WAV' in log_text, log_text
    assert 'audio: recording /b310e-os/rec/REC0001.WAV' in log_text, log_text
    assert 'audio: recorded' in log_text, log_text
    assert 'audio: playing /b310e-os/rec/REC0001.WAV' in log_text, log_text
    assert 'recorder: back, /b310e-os/rec/REC0002.WAV still recording' in log_text, log_text
    assert 'audio: recording /b310e-os/rec/REC0003.WAV, 32000 Hz, FM' in log_text, log_text
    assert 'radio: turned off' in log_text, log_text
    fm_lines = [l.split(' fm: ', 1)[1] for l in log_text.splitlines() if ' fm: id 1688' in l]
    assert fm_lines, 'no FM probe in the log'
    result['fm_log'] = 'fm: ' + fm_lines[0]
    assert 'present 1' in result['fm_log'], result

    # the music volume one down (7 -> 6), the preset kept
    settings = (card_file(image, '/b310e-os/settings.txt') or b'').decode().splitlines()
    result['vol_music'] = [l for l in settings if l.startswith('vol_music')]
    assert result['vol_music'] == ['vol_music=6'], result
    radio = (card_file(image, '/b310e-os/radio.txt') or b'').decode().splitlines()
    result['radio_file'] = radio
    assert f'p1={result["station_khz"]}' in radio and f'freq={result["station_khz"]}' in radio, result

    # FM trace: the tuner was started and tuned
    trace = (out / 'qemu.log').read_text(errors='replace')
    result['fm_trace_lines'] = sum(1 for l in trace.splitlines() if 'sc6530_fm' in l)
    assert result['fm_trace_lines'] > 0, 'no FM tuner activity in the trace'
    (out / 'result.json').write_text(json.dumps(result, indent=2))
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
