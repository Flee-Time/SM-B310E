#!/usr/bin/env python3
"""Compressed audio in the B310E OS, in QEMU: the playback engine, the
codec modules and the Music app (music.app).

Makes a 3 s stereo tone at -6 dBFS for every codec module built (each at
its own frequency) with ffmpeg, plus a 40 s 128 kbit/s 44.1 kHz stereo MP3
of noise and chords (as dense as music for the encoder), puts them in
/music on a card image with the modules in /b310e-os/codecs and
music.app in /b310e-os/apps, and with
QEMU's audio written to a WAV:
  - Apps -> Music, Center: the list plays in order, each track by itself
    after the one before; the tags reach the log;
  - the last track (the 40 s MP3): the decoder thread's CPU share at the
    clock the governor picks (QEMU is not cycle-accurate: this is the
    host's speed, not the phone's), no underruns;
  - Center pauses (the position stands still) and resumes; Right held
    seeks forward; Right tapped goes to the next track (the list repeats);
then checks QEMU's output: every tone at its frequency, in the playlist's
order, as loud as the WAV one within 1.5 dB (an 88.2 kHz FLAC is
resampled to 44.1 kHz on the way).

  python os/tests/test-codecs.py [--build build/os]

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
import shutil
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
MENU, MAGIC = boot['MENU'], boot['MAGIC']
symbol, status, string, wait_for, free_port, card_file = (
    boot[n] for n in ('symbol', 'status', 'string', 'wait_for', 'free_port', 'card_file'))
MEDIA = ('state seq position_ms length_ms rate out_rate underruns decoded_ms '
         'decode_cycles wall_cycles ring_frames cpu_mhz').split()
PLAYING, PAUSED = 2, 3

# module -> the test files for it: (name, Hz, ffmpeg output options)
TONES = {
    'wav': [('01-wav.wav', 300, ['-c:a', 'pcm_s16le'])],
    'flac': [('02-flac.flac', 400, ['-c:a', 'flac']),
             # 88.2 kHz: no codec clock for it, resampled to 44.1 kHz
             ('10-flac-88k.flac', 1100, ['-c:a', 'flac', '-ar', '88200'])],
    'mpa': [('03-mp3.mp3', 1000, ['-c:a', 'libmp3lame', '-b:a', '128k', '-id3v2_version', '3']),
            ('04-mp2.mp2', 500, ['-c:a', 'mp2', '-b:a', '192k'])],
    'vorbis': [('05-vorbis.ogg', 600, ['-c:a', 'libvorbis', '-q:a', '4'])],
    'opus': [('06-opus.opus', 700, ['-c:a', 'libopus', '-b:a', '96k'])],
    'aac': [('07-aac.m4a', 800, ['-c:a', 'aac', '-b:a', '128k'])],
    'alac': [('08-alac.m4a', 1200, ['-c:a', 'alac'])],
    'wavpack': [('09-wavpack.wv', 1500, ['-c:a', 'wavpack'])],
}
LOAD = 'zz-load.mp3'
UNTAGGED = {'04-mp2.mp2'}               # ffmpeg writes raw MPEG there: no ID3


def ffmpeg(*args):
    subprocess.run(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-y', *args], check=True)


def make_files(out, modules):
    files = []
    for mod in modules:
        for name, hz, opts in TONES.get(mod, []):
            tone = f'0.5*sin(2*PI*{hz}*t)'
            ffmpeg('-f', 'lavfi', '-i', f'aevalsrc={tone}|{tone}:s=44100:d=3', *opts,
                   '-metadata', f'title=Tone {hz} Hz', '-metadata', 'artist=B310E test',
                   '-metadata', 'album=Codecs', str(out / name))
            files.append((name, hz))
    files.sort()
    if 'mpa' in modules:
        # noise and a chord: the encoder spends all of its 128 kbit/s
        ffmpeg('-f', 'lavfi', '-i', 'anoisesrc=color=pink:amplitude=0.25:sample_rate=44100:d=40',
               '-f', 'lavfi', '-i', 'aevalsrc=0.2*sin(2*PI*220*t)+0.15*sin(2*PI*277*t)+'
               '0.1*sin(2*PI*330*t)|0.2*sin(2*PI*247*t)+0.15*sin(2*PI*311*t):s=44100:d=40',
               '-filter_complex', 'amix=inputs=2:normalize=0', '-ac', '2', '-ar', '44100',
               '-c:a', 'libmp3lame', '-b:a', '128k', '-metadata', 'title=Load',
               str(out / LOAD))
    return files


def windows(pcm, rate, size):
    """(Hz by zero crossings, RMS) of each window of size samples."""
    res = []
    for at in range(0, len(pcm) - size, size):
        w = pcm[at:at + size]
        crossings = sum(1 for a, b in zip(w, w[1:]) if (a < 0) != (b < 0))
        rms = math.sqrt(sum(v * v for v in w) / size)
        res.append((crossings / 2 / (size / rate), rms))
    return res


def find_tones(pcm, rate, tones):
    """The first run of 2 s or more at each tone's frequency: (start s, RMS)."""
    size = rate // 5
    wins = windows(pcm, rate, size)
    found = {}
    for name, hz in tones:
        run = []
        for i, (f, rms) in enumerate(wins):
            if abs(f - hz) < hz * 0.03 and rms > 300:
                run.append(rms)
                if len(run) >= 10:
                    found[name] = (round((i - len(run) + 1) * size / rate, 2),
                                   round(sorted(run)[len(run) // 2]))
                    break
            else:
                run = []
    return found


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qemu', type=Path, default=REPO / 'build/qemu/build/qemu-system-arm.exe')
    parser.add_argument('--build', type=Path, default=REPO / 'build/os')
    parser.add_argument('--icount', type=int, metavar='SHIFT',
                        help='run QEMU with -icount shift=SHIFT (one instruction per '
                             '2^SHIFT ns of guest time): the decoder thread\'s 26 MHz cycles then '
                             'count instructions, reported as instructions per second of audio')
    args = parser.parse_args()
    out = (args.build / ('test-codecs' if args.icount is None else 'test-codecs-icount')).resolve()
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    assert shutil.which('ffmpeg'), 'ffmpeg is needed to make the test files'
    elf, image = args.build / 'b310e-os.elf', args.build / 'b310e-os.bin'
    base, mstat = symbol(elf, 'os_status'), symbol(elf, 'media_status')
    m_cur, m_on = symbol(elf, 'music_cur'), symbol(elf, 'music_on')
    modules = sorted(p.stem for p in (args.build / 'codecs').glob('*.cdc'))
    assert 'mpa' in modules, 'build the codec modules first (make -C os)'
    assert (args.build / 'modules/music.app').exists(), 'build the apps first (make -C os)'
    files = make_files(out, modules)
    names = [n for n, _ in files] + [LOAD]
    card = out / 'card.img'
    adds = ['--add', f'b310e-os/apps/music.app={(args.build / "modules/music.app").resolve()}']
    for m in modules:
        adds += ['--add', f'b310e-os/codecs/{m}.cdc={args.build / "codecs" / f"{m}.cdc"}']
    for n in names:
        adds += ['--add', f'music/{n}={out / n}']
    subprocess.run([sys.executable, str(REPO / 'ports/rockbox/tests/make-sd-image.py'),
                    '--fat32', '--size-mib', '64', '--seconds', '1', '--force',
                    '--runtime', str(REPO / 'sdcard/.rockbox'),
                    '--rockbox', str(REPO / 'sdcard/payloads/rockbox.bin'),
                    *adds, '--output', str(card)], check=True, capture_output=True)
    wav_out = out / 'audio.wav'
    qmp_port, qtest_port = free_port(), free_port()
    cmd = [str(args.qemu.resolve()), '-M', 'b310e,boot-mode=rockbox',
           '-display', 'none', '-serial', 'none', '-monitor', 'none',
           '-drive', f'file={image.resolve().as_posix()},format=raw,if=none,id=os,readonly=on',
           '-drive', f'file={card.as_posix()},format=raw,if=none,id=sdcard',
           '-qmp', f'tcp:127.0.0.1:{qmp_port},server=on,wait=off',
           '-qtest', f'tcp:127.0.0.1:{qtest_port},server=on,wait=off',
           '-global', 'sc6530_aux.headset-present=on',
           '-audiodev', f'wav,id=audio0,path={wav_out.as_posix()},out.frequency=44100,'
                        'out.channels=2,out.format=s16',
           '-global', 'sc6530_adi.audiodev=audio0']
    if args.icount is not None:
        cmd += ['-icount', f'shift={args.icount},sleep=on']
    env = os.environ.copy()
    if os.name == 'nt':
        env['PATH'] = r'D:\Toolchains\msys64\mingw64\bin;' + env.get('PATH', '')
    result = {'modules': modules}
    p = subprocess.Popen(cmd, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    qmp = qt = None
    try:
        for _ in range(100):
            try:
                qmp, qt = capture.QMP(qmp_port), QT(qtest_port)
                break
            except OSError:
                time.sleep(.1)
        assert qmp and qt, 'QEMU did not start'

        def media():
            return dict(zip(MEDIA, (qt.read(mstat + 4 * i) for i in range(len(MEDIA)))))

        def key(k, want=None, wait=0.35, hold=100):
            qmp.hmp(f'sendkey {k} {200 if k == "esc" else hold}')
            if want:
                wait_for(qt, base, lambda s: string(qt, s['ui_title']) == want, 6, f'not {want}')
            time.sleep(wait)

        def shot(name):
            qmp.command('screendump', {'filename': str(out / f'{name}.png'), 'format': 'png'})

        def wait_track(i, seconds):
            end = time.monotonic() + seconds
            while time.monotonic() < end:
                if qt.read(m_cur) == i and media()['state'] == PLAYING:
                    return
                time.sleep(0.05)
            raise AssertionError(f'track {i} ({names[i]}) never played: '
                                 f'cur {qt.read(m_cur)}, {media()}')

        wait_for(qt, base, lambda s: s['magic'] == MAGIC, 10, 'OS never started')
        wait_for(qt, base, lambda s: s['ui_depth'] == 1, 60, 'no UI')
        time.sleep(1.5)

        key('esc', wait=0.5)
        key('ret', 'Menu')
        for _ in range(MENU.index('Apps')):
            key('down')
        key('ret', 'Apps')
        key('ret', 'Music', wait=0.8)                   # the only app on the card
        result['list_rows'] = status(qt, base)['ui_list_count']
        shot('list')
        assert result['list_rows'] == len(names), result
        key('ret', 'Now playing')                       # the first track
        t0 = time.monotonic()
        result['starts'] = []
        for i, n in enumerate(names):
            wait_track(i, 15)
            result['starts'].append((n, round(time.monotonic() - t0, 1)))
            if n.endswith('.mp3') and n != LOAD:
                time.sleep(1.2)
                shot('now-playing-mp3')

        # the load track: the decoder's share of the time, at the clock now
        time.sleep(3)
        a = media()
        clocks = []
        for _ in range(16):
            time.sleep(0.5)
            clocks.append(media()['cpu_mhz'])
        b = media()
        shot('now-playing-load')
        share = (b['decode_cycles'] - a['decode_cycles']) / max(1, b['wall_cycles'] - a['wall_cycles'])
        result['load'] = {
            'decoder_share_percent': round(100 * share, 1),
            'audio_seconds': round((b['decoded_ms'] - a['decoded_ms']) / 1000, 2),
            'wall_seconds': round((b['wall_cycles'] - a['wall_cycles']) / 26e6, 2),
            'mhz_seen': sorted(set(clocks)), 'underruns': b['underruns'],
            'rate': b['rate'], 'out_rate': b['out_rate'],
        }
        if args.icount is not None:
            # guest ns = instructions << shift; 26 MHz cycles -> ns
            insns = (b['decode_cycles'] - a['decode_cycles']) / 26e6 * 1e9 / (1 << args.icount)
            audio = (b['decoded_ms'] - a['decoded_ms']) / 1000
            result['load']['decoder_minstr_per_audio_s'] = round(insns / audio / 1e6, 2)
        assert b['underruns'] == 0, result

        # pause: the position stands; then on again
        key('1', wait=0.5)                               # wakes the dimmed screen only
        key('ret')
        m1 = media()
        time.sleep(1.0)
        m2 = media()
        result['paused'] = (m1['state'], m1['position_ms'], m2['position_ms'])
        assert m1['state'] == PAUSED and abs(m2['position_ms'] - m1['position_ms']) < 100, result
        key('ret', wait=0.8)
        result['resumed'] = media()['state']
        assert result['resumed'] == PLAYING, result

        # held Right seeks forward
        before = media()['position_ms']
        key('right', hold=1200, wait=2.7)                  # sendkey returns at the press
        after = media()['position_ms']
        result['seek'] = (before, after)
        assert after - before > 5000, result
        shot('after-seek')

        # tapped Right: the next track, the first again (the list repeats)
        key('right', wait=0.2)
        end = time.monotonic() + 5
        while time.monotonic() < end and qt.read(m_cur) != 0:
            time.sleep(0.1)
        result['after_tap'] = (qt.read(m_cur), media()['state'])
        assert result['after_tap'][0] == 0, result
        key('f2', 'Music', wait=1.0)                     # the music plays on
        result['music_on_after_back'] = qt.read(m_on) & 0xff
        assert result['music_on_after_back'] == 1, result
        time.sleep(2.5)                                  # the card log
    except Exception:
        print(json.dumps(result, indent=2))
        time.sleep(2.5)                                  # the card log, to look at
        raise
    finally:
        if qt:
            qt.close()
        if qmp:
            try:
                qmp.command('quit')                      # QEMU finishes the WAV header
                p.wait(timeout=10)
            except Exception:
                pass
            qmp.close()
        if p.poll() is None:
            p.terminate()
            p.wait(timeout=10)

    with wave.open(str(wav_out), 'rb') as w:
        rate = w.getframerate()
        pcm = array('h', w.readframes(w.getnframes()))
    left = list(pcm[0::2])
    found = find_tones(left, rate, files)
    result['tones'] = {n: found.get(n) for n, _ in files}
    missing = [n for n, _ in files if n not in found]
    assert not missing, ('not heard', missing, result)
    starts = [found[n][0] for n, _ in files]
    assert starts == sorted(starts), ('out of order', result)
    ref = found[files[0][0]][1] if files[0][0].endswith('.wav') else None
    if ref:
        result['level_db'] = {n: round(20 * math.log10(found[n][1] / ref), 2) for n, _ in files}
        loud = {n: d for n, d in result['level_db'].items() if abs(d) > 1.5}
        assert not loud, ('level', loud, result)

    log = (card_file(card.read_bytes(), '/b310e-os/log.txt') or b'').decode(errors='replace')
    (out / 'card-log.txt').write_text(log)
    result['log'] = [l.split(' media: ', 1)[1] for l in log.splitlines() if ' media: ' in l]
    for n, hz in files:
        if n in UNTAGGED:
            continue
        assert any(f'playing /music/{n}' in l and f'"Tone {hz} Hz" by "B310E test"' in l
                   for l in result['log']), ('tags', n, result['log'])
    (out / 'result.json').write_text(json.dumps(result, indent=2))
    print('PASS ' + json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
