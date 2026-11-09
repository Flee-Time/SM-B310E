#!/usr/bin/env python3
"""Music closes itself when it has played nothing for a long time, in QEMU.

With player_idle_s=6 in the settings (15 minutes by default):
  - open with nothing playing: it closes;
  - a 10 s track: open and loaded all through it, closed 6 s after it ends;
  - paused and left in the background, held only by its notification: the queue
    goes and the app is unloaded.

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
LIMIT_S = 6


def tone_wav(path, hz, seconds):
    rate = 8000
    with wave.open(str(path), 'wb') as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(b''.join(struct.pack('<h', int(8000 * math.sin(2 * math.pi * hz * i / rate)))
                               for i in range(int(rate * seconds))))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qemu', type=Path, default=REPO / 'build/qemu/build/qemu-system-arm.exe')
    parser.add_argument('--build', type=Path, default=REPO / 'build/os')
    args = parser.parse_args()
    out = (args.build / 'test-music-idle').resolve()
    out.mkdir(parents=True, exist_ok=True)
    elf, image = args.build / 'b310e-os.elf', args.build / 'b310e-os.bin'
    base = symbol(elf, 'os_status')
    m_on, m_state = symbol(elf, 'music_on'), symbol(elf, 'media_status')
    tone_wav(out / 'long.wav', 440, 10.0)
    settings = out / 'settings.txt'
    settings.write_text(f'player_idle_s={LIMIT_S}\nscreen_timeout=0\n', newline='\n')
    card = out / 'card.img'
    subprocess.run([sys.executable, str(REPO / 'ports/rockbox/tests/make-sd-image.py'),
                    '--fat32', '--size-mib', '64', '--seconds', '1', '--force',
                    '--runtime', str(REPO / 'sdcard/.rockbox'),
                    '--rockbox', str(REPO / 'sdcard/payloads/rockbox.bin'),
                    '--add', f'b310e-os/apps/music.app={(args.build / "modules/music.app").resolve()}',
                    '--add', f'b310e-os/settings.txt={settings}',
                    '--add', f'music/long.wav={out / "long.wav"}',
                    '--add', f'b310e-os/codecs/wav.cdc={args.build / "codecs/wav.cdc"}',
                    '--output', str(card)], check=True, capture_output=True)
    qmp_port, qtest_port = free_port(), free_port()
    cmd = [str(args.qemu.resolve()), '-M', 'b310e,boot-mode=rockbox',
           '-display', os.environ.get('B310E_QEMU_DISPLAY', 'none'), '-serial', 'none', '-monitor', 'none',
           '-drive', f'file={image.resolve().as_posix()},format=raw,if=none,id=os,readonly=on',
           '-drive', f'file={card.as_posix()},format=raw,if=none,id=sdcard',
           '-qmp', f'tcp:127.0.0.1:{qmp_port},server=on,wait=off',
           '-qtest', f'tcp:127.0.0.1:{qtest_port},server=on,wait=off',
           '-audiodev', 'none,id=audio0', '-global', 'sc6530_adi.audiodev=audio0']
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
                wait_for(qt, base, lambda s: string(qt, s['ui_title']) == want, 6, f'not {want}')
            time.sleep(wait)

        def closed(seconds):
            t0 = time.monotonic()
            while time.monotonic() - t0 < seconds:
                if status(qt, base)['apps_loaded'] == 0:
                    return round(time.monotonic() - t0, 1)
                time.sleep(0.1)
            raise AssertionError(f'still loaded after {seconds} s: {json.dumps(result)}')

        wait_for(qt, base, lambda s: s['magic'] == MAGIC, 10, 'OS never started')
        wait_for(qt, base, lambda s: s['ui_depth'] == 1, 60, 'no UI')
        time.sleep(1)
        key('ret', 'Menu')
        key('ret', 'Apps')

        # Open, nothing playing
        key('ret', 'Music', wait=0.5)
        assert status(qt, base)['apps_loaded'] == 1
        result['closed_open_s'] = closed(LIMIT_S + 6)
        assert result['closed_open_s'] >= LIMIT_S - 2, result
        wait_for(qt, base, lambda s: string(qt, s['ui_title']) == 'Apps', 3, 'not back in Apps')

        # A track longer than the limit: open all through it, closed after it
        time.sleep(0.5)
        key('ret', 'Music', wait=0.5)
        key('ret', 'Now playing', wait=0)
        key('slash', wait=0.2)                      # #: Repeat one (all is the default,
        key('slash', wait=0.2)                      # #: No repeat   which never ends)
        t0 = time.monotonic()
        while time.monotonic() - t0 < 8:
            s = status(qt, base)
            assert s['apps_loaded'] == 1 and string(qt, s['ui_title']) == 'Now playing', (
                'closed while playing', round(time.monotonic() - t0, 1), result)
            time.sleep(0.2)
        result['open_while_playing_s'] = 8
        result['closed_after_track_s'] = closed(LIMIT_S + 12)
        result['music_on_after'] = qt.read(m_on) & 0xff
        wait_for(qt, base, lambda s: string(qt, s['ui_title']) == 'Apps', 3, 'not back in Apps')

        # Paused and left: only its notification holds it
        time.sleep(0.5)
        key('ret', 'Music', wait=0.5)
        key('ret', 'Now playing', wait=0.5)
        key('ret')                                  # pause
        result['paused'] = (qt.read(m_on) & 0xff, qt.read(m_state))
        assert result['paused'] == (1, 3), result
        key('f2', 'Music')
        key('f2', 'Apps', wait=0.3)
        assert status(qt, base)['apps_loaded'] == 1, 'the notification should hold it'
        result['closed_paused_s'] = closed(LIMIT_S + 6)
        result['music_on_after_paused'] = qt.read(m_on) & 0xff
        assert result['music_on_after_paused'] == 0, result
        time.sleep(3)                               # the log reaches the card
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
    result['log'] = [l.strip() for l in log.splitlines() if 'music: nothing played' in l]
    assert result['log'], 'no line in the log'
    print('PASS ' + json.dumps(result))


if __name__ == '__main__':
    main()
