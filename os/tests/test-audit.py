#!/usr/bin/env python3
"""The bugs an audit found on 2026-11-09, in QEMU.

  - Bad files: a cover JPEG with too many Huffman codes, a cover PNG of depth 0, WAVs at
    5512 Hz and at 1 Hz (the resampler). They play or are refused, and the OS goes on.
  - time_zone=-300 in the settings loads as -300 (it was 0).
  - An alarm over Music: the music pauses for the ring and goes on after it.
  - An alarm over the radio: the DAC gets its route for the tone, the radio gets it back.
  - An alarm due while another rings is not lost: it rings next.

The time moves by setting the RTC model, so this takes a minute, not many.

SPDX-License-Identifier: GPL-2.0-or-later
"""
import argparse
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
boot = runpy.run_path(str(Path(__file__).with_name('test-boot.py')))
QT, capture = boot['QT'], boot['capture']
symbol, status, string, wait_for, free_port, card_file = (
    boot[n] for n in ('symbol', 'status', 'string', 'wait_for', 'free_port', 'card_file'))
MAGIC = boot['MAGIC']
RTC = 0x82001600
TZ_OFFSET = 13 * 4                  # ui_settings_t.tz_min
MEDIA_PLAYING, MEDIA_PAUSED = 2, 3


def tone_wav(path, seconds, rate=8000):
    with wave.open(str(path), 'wb') as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(b''.join(struct.pack('<h', int(8000 * math.sin(2 * math.pi * 440 * i / rate)))
                               for i in range(int(rate * seconds))))


def odd_rate_wav(path, rate, frames):
    """A header saying a rate the OS must not trust."""
    data = struct.pack(f'<{frames}h', *[(i * 3000) % 20000 - 10000 for i in range(frames)])
    fmt = struct.pack('<HHIIHH', 1, 1, rate, rate * 2, 2, 16)
    body = b'WAVE' + b'fmt ' + struct.pack('<I', 16) + fmt + b'data' + struct.pack('<I', len(data)) + data
    path.write_bytes(b'RIFF' + struct.pack('<I', len(body)) + body)


def bad_jpeg(path):
    """SOI, then a DHT with three codes of one bit (only two can exist), then SOF and EOI."""
    bits = bytes([3, 200] + [0] * 14)       # lengths 1 and 2: 2 and 4 are all there can be
    vals = bytes(203)
    seg = bytes([0x00]) + bits + vals
    dht = b'\xff\xc4' + struct.pack('>H', len(seg) + 2) + seg
    sof = b'\xff\xc0' + struct.pack('>HBHHB', 11, 8, 16, 16, 1) + bytes([1, 0x11, 0])
    path.write_bytes(b'\xff\xd8' + dht + sof + b'\xff\xd9')


def bad_png(path):
    """Depth 0: a row of no bytes, so it never ended."""
    ihdr = struct.pack('>IIBBBBB', 16, 16, 0, 0, 0, 0, 0)
    chunk = lambda t, d: struct.pack('>I', len(d)) + t + d + b'\0\0\0\0'
    path.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', ihdr) + chunk(b'IDAT', bytes(64)) + chunk(b'IEND', b''))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qemu', type=Path, default=REPO / 'build/qemu/build/qemu-system-arm.exe')
    parser.add_argument('--build', type=Path, default=REPO / 'build/os')
    args = parser.parse_args()
    out = (args.build / 'test-audit').resolve()
    out.mkdir(parents=True, exist_ok=True)
    elf, image = args.build / 'b310e-os.elf', args.build / 'b310e-os.bin'
    base = symbol(elf, 'os_status')
    ringing = symbol(elf, 'alarm_ringing')
    m_cur, m_on = symbol(elf, 'music_cur'), symbol(elf, 'music_on')
    m_state = symbol(elf, 'media_status')
    settings_at = symbol(elf, 'ui_settings')
    fm_routed = symbol(elf, 'fm_routed')
    tone_wav(out / 'a.wav', 1.5)
    tone_wav(out / 'b.wav', 1.5)
    tone_wav(out / 'long.wav', 40)
    odd_rate_wav(out / 'low.wav', 5512, 5512)
    odd_rate_wav(out / 'tiny.wav', 1, 2)
    bad_jpeg(out / 'cover.jpg')
    bad_png(out / 'cover.png')
    (out / 'settings.txt').write_text('time_zone=-300\nscreen_timeout=0\n', newline='\n')
    (out / 'alarms.txt').write_text('1 61 127 M\n1 63 127 F\n1 65 127 P\n1 66 127 Q\n', newline='\n')
    add = {
        'b310e-os/apps/music.app': args.build / 'modules/music.app',
        'b310e-os/apps/radio.app': args.build / 'modules/radio.app',
        'b310e-os/codecs/wav.cdc': args.build / 'codecs/wav.cdc',
        'b310e-os/settings.txt': out / 'settings.txt',
        'b310e-os/alarms.txt': out / 'alarms.txt',
        'music/a/a.wav': out / 'a.wav',
        'music/a/cover.jpg': out / 'cover.jpg',
        'music/b/b.wav': out / 'b.wav',
        'music/b/cover.png': out / 'cover.png',
        'music/low.wav': out / 'low.wav',
        'music/tiny.wav': out / 'tiny.wav',
        'music/zz/long.wav': out / 'long.wav',
    }
    card = out / 'card.img'
    cmd = [sys.executable, str(REPO / 'ports/rockbox/tests/make-sd-image.py'),
           '--fat32', '--size-mib', '64', '--seconds', '1', '--force',
           '--runtime', str(REPO / 'sdcard/.rockbox'),
           '--rockbox', str(REPO / 'sdcard/payloads/rockbox.bin')]
    for k, v in add.items():
        cmd += ['--add', f'{k}={Path(v).resolve()}']
    subprocess.run(cmd + ['--output', str(card)], check=True, capture_output=True)
    qmp_port, qtest_port = free_port(), free_port()
    cmd = [str(args.qemu.resolve()), '-M', 'b310e,boot-mode=rockbox',
           '-display', os.environ.get('B310E_QEMU_DISPLAY', 'none'), '-serial', 'none', '-monitor', 'none',
           '-drive', f'file={image.resolve().as_posix()},format=raw,if=none,id=os,readonly=on',
           '-drive', f'file={card.as_posix()},format=raw,if=none,id=sdcard',
           '-qmp', f'tcp:127.0.0.1:{qmp_port},server=on,wait=off',
           '-qtest', f'tcp:127.0.0.1:{qtest_port},server=on,wait=off',
           '-audiodev', 'none,id=audio0', '-global', 'sc6530_adi.audiodev=audio0',
           '-global', 'sc6530_aux.headset-present=on']
    env = os.environ.copy()
    if os.name == 'nt':
        env['PATH'] = r'D:\Toolchains\msys64\mingw64\bin;' + env.get('PATH', '')
    result = {}
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

        def title():
            return string(qt, status(qt, base)['ui_title'])

        def key(k, want=None, wait=0.35):
            qmp.hmp(f'sendkey {k} {200 if k == "esc" else 100}')
            if want:
                wait_for(qt, base, lambda s: string(qt, s['ui_title']) == want, 8, f'not {want}, {title()!r}')
            time.sleep(wait)

        def clock(h, m, s):
            for i, v in enumerate((s, m, h, qt.read(RTC + 12) & 0xffff)):
                qt.write(RTC + 0x10 + 4 * i, v)
            qt.write(RTC + 0x38, 0xf00)

        def rings(want, seconds=8):
            end = time.monotonic() + seconds
            while time.monotonic() < end:
                if (qt.read(ringing) & 0xff) == want:
                    return True
                time.sleep(0.1)
            return False

        def state(want, seconds=5):
            end = time.monotonic() + seconds
            while time.monotonic() < end and qt.read(m_state) != want:
                time.sleep(0.1)
            return qt.read(m_state)

        def alive():
            t = status(qt, base)['ticks']
            time.sleep(1.5)                     # it is updated once a second
            return status(qt, base)['ticks'] > t

        wait_for(qt, base, lambda s: s['magic'] == MAGIC, 10, 'OS never started')
        wait_for(qt, base, lambda s: s['ui_depth'] == 1, 60, 'no UI')
        time.sleep(1)

        # A negative setting
        tz = qt.read(settings_at + TZ_OFFSET)
        result['tz_min'] = tz - (1 << 32) if tz >= 1 << 31 else tz
        assert result['tz_min'] == -300, result

        # Bad files, through Music: the covers are read and the odd rates played
        key('ret', 'Menu')
        key('ret', 'Apps')
        key('down')                                   # by name: FM radio, Music
        key('ret', 'Music', wait=1.5)
        key('ret', 'Now playing', wait=0)
        played = set()
        end = time.monotonic() + 40
        while time.monotonic() < end and len(played) < 4:
            cur = qt.read(m_cur)
            played.add(cur)
            assert status(qt, base)['ticks'], 'no status'
            time.sleep(0.2)
        result['tracks_played'] = sorted(played)
        assert len(played) >= 4 and alive(), result
        end = time.monotonic() + 40
        while time.monotonic() < end and qt.read(m_cur) != 4:
            time.sleep(0.2)
        assert qt.read(m_cur) == 4, ('never reached the long track', qt.read(m_cur))
        time.sleep(1)

        # An alarm over Music
        assert state(MEDIA_PLAYING) == MEDIA_PLAYING
        clock(1, 0, 57)
        assert rings(1), ('no alarm at 01:01', result)
        result['music_during_ring'] = state(MEDIA_PAUSED)
        assert result['music_during_ring'] == MEDIA_PAUSED, result
        time.sleep(1)
        key('f2', wait=0.3)
        assert rings(0), 'the ring did not stop'
        result['music_after_ring'] = state(MEDIA_PLAYING)
        assert result['music_after_ring'] == MEDIA_PLAYING, result

        # Quit, then the radio
        if title() != 'Music':
            key('f1', 'Music')
        key('f1', 'Options')
        key('down')
        key('ret', 'Apps', wait=1.0)
        result['music_on_after_quit'] = qt.read(m_on) & 0xff
        assert result['music_on_after_quit'] == 0, result
        key('up')
        key('ret', 'FM radio', wait=2.5)
        result['fm_routed_before'] = qt.read(fm_routed) & 0xff
        assert result['fm_routed_before'] == 1, result
        clock(1, 2, 57)
        assert rings(1), ('no alarm at 01:03', result)
        time.sleep(0.5)
        result['fm_routed_during'] = qt.read(fm_routed) & 0xff
        assert result['fm_routed_during'] == 0, result
        key('f2', wait=0.3)
        assert rings(0), 'the ring did not stop'
        time.sleep(1)
        result['fm_routed_after'] = qt.read(fm_routed) & 0xff
        assert result['fm_routed_after'] == 1, result

        # Due while another rings
        clock(1, 4, 57)
        assert rings(1), ('no alarm at 01:05', result)
        clock(1, 5, 57)
        time.sleep(5)                                 # 01:06 comes while it rings
        key('f2', wait=0.1)
        end = time.monotonic() + 3
        stopped = False
        while time.monotonic() < end:
            if not qt.read(ringing) & 0xff:
                stopped = True
            elif stopped:
                break
            time.sleep(0.05)
        result['second_alarm_rang'] = bool(qt.read(ringing) & 0xff)
        assert result['second_alarm_rang'], result
        key('f2', wait=0.3)
        assert alive(), result
        time.sleep(2.5)                               # the log reaches the card
        qmp.command('quit')
        p.wait(timeout=20)
    except Exception:
        print(json.dumps(result, indent=2))
        raise
    finally:
        if qt:
            qt.close()
        if qmp:
            qmp.close()
        if p.poll() is None:
            p.terminate()
            p.wait()
    log = (card_file(card.read_bytes(), '/b310e-os/log.txt') or b'').decode(errors='replace')
    result['alarm_lines'] = [x.strip() for x in log.splitlines() if 'alarm: ringing' in x]
    result['art_lines'] = [x.strip()[:90] for x in log.splitlines() if ' art: ' in x][:6]
    assert len(result['alarm_lines']) >= 4, result
    print('PASS ' + json.dumps(result))


if __name__ == '__main__':
    main()
