#!/usr/bin/env python3
"""The quick panel's Sound row and page, and a volume of each app's own, in QEMU.

  - the panel has Sound, Brightness, Torch; Left/Right on Sound changes what
    is playing (with nothing playing, the music's); Center opens the page;
  - the page changes any one level (the alarm's here);
  - volumes.txt on the card is read at boot: a tag for an app not loaded;
  - the Recorder plays with no change of its own: it gets a tag under its name
    at the Apps level, and the codec takes that level;
  - over the playing app the panel's row and the page change that tag, heard
    at once in the codec's volume, and the page opens on its row;
  - * + # closes the page and the panel together;
  - the card has the tag and its icon after power-off, and a second boot lists
    the same tags at the same levels;
  - Reset puts every level back and clears the app rows, but for what is
    playing; a loaded app that plays again is listed again; the card agrees.

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
MENU, MAGIC = boot['MENU'], boot['MAGIC']
VOL_OFFSET = 24                         # ui_settings.vol: after six ints
MUSIC, APPS, ALARM = 0, 1, 2
TAG_SIZE = 95                           # tag_t: name[20], level, has_icon, hidden, icon_t
DB10 = [-1000, -400, -340, -280, -230, -180, -140, -100, -60, -30, 0]


def tone_wav(path, seconds):
    with wave.open(str(path), 'wb') as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(16000)
        w.writeframes(b''.join(struct.pack('<h', int(8000 * math.sin(i / 16000 * 2 * math.pi * 1000)))
                               for i in range(int(seconds * 16000))))


class Phone:
    def __init__(self, qemu, elf, image, card):
        self.base = symbol(elf, 'os_status')
        self.settings = symbol(elf, 'ui_settings')
        self.n_tags = symbol(elf, 'n_tags')
        self.tags = symbol(elf, 'tags')
        self.codec = symbol(elf, 'codec_db10')
        qmp_port, qtest_port = free_port(), free_port()
        cmd = [str(qemu.resolve()), '-M', 'b310e,boot-mode=rockbox',
               '-display', os.environ.get('B310E_QEMU_DISPLAY', 'none'),
               *os.environ.get('B310E_QEMU_AUDIO', '').split(),
               '-serial', 'none', '-monitor', 'none',
               '-drive', f'file={image.resolve().as_posix()},format=raw,if=none,id=os,readonly=on',
               '-drive', f'file={card.as_posix()},format=raw,if=none,id=sdcard',
               '-qmp', f'tcp:127.0.0.1:{qmp_port},server=on,wait=off',
               '-qtest', f'tcp:127.0.0.1:{qtest_port},server=on,wait=off']
        env = os.environ.copy()
        if os.name == 'nt':
            env['PATH'] = r'D:\Toolchains\msys64\mingw64\bin;' + env.get('PATH', '')
        self.p = subprocess.Popen(cmd, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.qmp = self.qt = None
        for _ in range(100):
            try:
                self.qmp, self.qt = capture.QMP(qmp_port), QT(qtest_port)
                break
            except OSError:
                time.sleep(.1)
        assert self.qmp and self.qt, 'QEMU did not start'
        wait_for(self.qt, self.base, lambda s: s['magic'] == MAGIC, 10, 'OS never started')
        wait_for(self.qt, self.base, lambda s: s['ui_depth'] == 1, 60, 'no UI')
        time.sleep(1)

    def title(self):
        return string(self.qt, status(self.qt, self.base)['ui_title'])

    def press(self, k, want=None):
        self.qmp.hmp(f'sendkey {k} 100')
        if want:
            wait_for(self.qt, self.base, lambda s: string(self.qt, s['ui_title']) == want, 6, f'not {want}')
        time.sleep(0.35)

    def chord(self, want):
        self.qmp.hmp('sendkey minus-slash 150')
        wait_for(self.qt, self.base, lambda s: string(self.qt, s['ui_title']) == want, 6, f'* + #: not {want}')
        time.sleep(0.4)

    def shot(self, out, name):
        self.qmp.command('screendump', {'filename': str(out / f'{name}.png'), 'format': 'png'})
        time.sleep(0.2)

    def level(self, cat):
        return self.qt.read(self.settings + VOL_OFFSET + 4 * cat)

    def tag_list(self):
        """[(name, level)] from the OS's table; read through 32-bit words"""
        tags = []
        for i in range(self.qt.read(self.n_tags)):
            at = self.tags + i * TAG_SIZE
            tags.append((string(self.qt, at), self.byte(at + 20)))
        return tags

    def listed(self):
        """[(name, level)] of the rows the page shows"""
        return [t for i, t in enumerate(self.tag_list())
                if not self.byte(self.tags + i * TAG_SIZE + 22)]

    def reset(self):
        """from the Sound page: LSoft, then LSoft for Yes"""
        self.press('f1', 'Reset sound')
        self.press('f1', 'Sound')

    def byte(self, addr):
        return self.qt.read(addr & ~3) >> (8 * (addr & 3)) & 0xff

    def db10(self):
        v = self.qt.read(self.codec)
        return v - (1 << 32) if v >= 1 << 31 else v

    def power_off(self):
        self.press('esc')
        self.press('ret', 'Menu')
        for _ in range(MENU.index('Power')):
            self.press('down')
        self.press('ret', 'Power')
        self.press('ret', 'Power off')
        self.qmp.hmp('sendkey f1 100')
        self.p.wait(timeout=15)

    def close(self):
        if self.qt:
            self.qt.close()
        if self.qmp:
            self.qmp.close()
        if self.p.poll() is None:
            self.p.terminate()
            self.p.wait()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qemu', type=Path, default=REPO / 'build/qemu/build/qemu-system-arm.exe')
    parser.add_argument('--build', type=Path, default=REPO / 'build/os')
    args = parser.parse_args()
    out = (args.build / 'test-volume').resolve()
    out.mkdir(parents=True, exist_ok=True)
    elf, image = args.build / 'b310e-os.elf', args.build / 'b310e-os.bin'
    tone_wav(out / 'TONE.WAV', 30)
    (out / 'volumes.txt').write_text('3 - Old game\n')
    card = out / 'card.img'
    subprocess.run([sys.executable, str(REPO / 'ports/rockbox/tests/make-sd-image.py'),
                    '--fat32', '--size-mib', '64', '--seconds', '1', '--force',
                    '--runtime', str(REPO / 'sdcard/.rockbox'),
                    '--rockbox', str(REPO / 'sdcard/payloads/rockbox.bin'),
                    '--add', f'b310e-os/volumes.txt={out / "volumes.txt"}',
                    '--add', f'b310e-os/apps/recorder.app={(args.build / "modules/recorder.app").resolve()}',
                    '--add', f'b310e-os/rec/TONE.WAV={out / "TONE.WAV"}',
                    '--output', str(card)], check=True, capture_output=True)
    result = {}
    ph = Phone(args.qemu, elf, image, card)
    try:
        result['tags_at_boot'] = ph.tag_list()
        assert result['tags_at_boot'] == [('Old game', 3)], result
        music, alarm, apps = ph.level(MUSIC), ph.level(ALARM), ph.level(APPS)
        home = ph.title()

        # Nothing playing: the row is the music's; the page has the rest
        ph.chord('Quick panel')
        ph.shot(out, 'panel')
        ph.press('right')
        result['music_by_row'] = [music, ph.level(MUSIC)]
        assert ph.level(MUSIC) == music + 1, result
        ph.press('ret', 'Sound')
        ph.shot(out, 'sound')
        ph.press('down')                # from Music to Alarm
        ph.press('left')
        result['alarm_by_page'] = [alarm, ph.level(ALARM)]
        assert ph.level(ALARM) == alarm - 1 and ph.level(MUSIC) == music + 1, result
        for _ in range(5):              # the last row: Old game
            ph.press('down')
        ph.shot(out, 'sound-last')
        ph.press('right')
        result['old_game'] = ph.tag_list()
        assert result['old_game'] == [('Old game', 4)], result
        ph.press('f2', 'Quick panel')   # Back, to the panel
        ph.chord(home)

        # The Recorder plays and did nothing for it: a tag of its own
        ph.press('ret', 'Menu')
        ph.press('ret', 'Apps')
        ph.press('ret', 'Recorder')
        ph.press('f1', 'Recordings')
        ph.press('ret', 'Recordings')
        result['tags_playing'] = ph.tag_list()
        assert result['tags_playing'] == [('Old game', 4), ('Recorder', apps)], result
        result['codec_playing'] = ph.db10()
        assert ph.db10() == DB10[apps], result

        # Over it: the row, then the page opened on its row
        ph.chord('Quick panel')
        ph.shot(out, 'panel-playing')
        ph.press('left')
        result['by_row'] = [ph.tag_list()[1], ph.db10()]
        assert ph.tag_list()[1] == ('Recorder', apps - 1) and ph.db10() == DB10[apps - 1], result
        assert ph.level(APPS) == apps, result
        ph.press('f1', 'Sound')
        ph.shot(out, 'sound-playing')
        ph.press('left')
        result['by_page'] = [ph.tag_list()[1], ph.db10()]
        assert ph.tag_list()[1] == ('Recorder', apps - 2) and ph.db10() == DB10[apps - 2], result
        assert ph.tag_list()[0] == ('Old game', 4), result

        # * + #: the page and the panel at once
        ph.chord('Recordings')
        ph.press('ret')                 # stop
        ph.power_off()
    except Exception:
        print(json.dumps(result, indent=2))
        raise
    finally:
        ph.close()

    # The card after power-off
    img = card.read_bytes()
    text = (card_file(img, '/b310e-os/volumes.txt') or b'').decode()
    lines = text.splitlines()
    result['file'] = [(x.split(' ', 2)[0], len(x.split(' ', 2)[1]), x.split(' ', 2)[2]) for x in lines]
    assert result['file'] == [('4', 1, 'Old game'), (str(apps - 2), 144, 'Recorder')], result
    settings = (card_file(img, '/b310e-os/settings.txt') or b'').decode().splitlines()
    assert f'vol_music={music + 1}' in settings and f'vol_alarm={alarm - 1}' in settings, settings

    # And a second boot: the same tags, the icon read back
    ph = Phone(args.qemu, elf, image, card)
    try:
        result['second_boot'] = ph.tag_list()
        assert result['second_boot'] == [('Old game', 4), ('Recorder', apps - 2)], result
        assert ph.byte(ph.tags + TAG_SIZE + 21) == 1, 'the icon did not load'
        ph.chord('Quick panel')
        ph.press('ret', 'Sound')
        for _ in range(6):
            ph.press('down')
        ph.shot(out, 'sound-second-boot')

        # Reset with nothing playing: the levels back, no app rows
        ph.press('f1', 'Reset sound')
        ph.shot(out, 'reset-ask')
        ph.press('f1', 'Sound')
        ph.shot(out, 'sound-reset')
        result['reset_idle'] = [ph.level(MUSIC), ph.level(ALARM), ph.listed()]
        assert result['reset_idle'] == [music, alarm, []], result
        ph.chord(home)

        # The Recorder is listed again by playing, at the default
        ph.press('ret', 'Menu')
        ph.press('ret', 'Apps')
        ph.press('ret', 'Recorder')
        ph.press('f1', 'Recordings')
        ph.press('ret', 'Recordings')
        result['played_again'] = ph.listed()
        assert result['played_again'] == [('Recorder', apps)], result
        ph.press('ret')                 # stop

        # Reset while it is loaded and silent; playing lists it again
        ph.chord('Quick panel')
        ph.press('ret', 'Sound')
        ph.reset()
        result['reset_silent'] = ph.listed()
        assert result['reset_silent'] == [], result
        ph.chord('Recordings')
        ph.press('ret')
        result['played_after_reset'] = ph.listed()
        assert result['played_after_reset'] == [('Recorder', apps)], result

        # Reset while it plays: it keeps its row, its level back and heard
        ph.chord('Quick panel')
        ph.press('left')
        assert ph.db10() == DB10[apps - 1], result
        ph.press('f1', 'Sound')
        ph.reset()
        ph.shot(out, 'sound-reset-playing')
        result['reset_playing'] = [ph.listed(), ph.db10()]
        assert result['reset_playing'] == [[('Recorder', apps)], DB10[apps]], result
        ph.chord('Recordings')
        ph.press('ret')                 # stop
        ph.power_off()
    except Exception:
        print(json.dumps(result, indent=2))
        raise
    finally:
        ph.close()
    img = card.read_bytes()
    lines = (card_file(img, '/b310e-os/volumes.txt') or b'').decode().splitlines()
    result['file_after_reset'] = [(x.split(' ', 2)[0], x.split(' ', 2)[2]) for x in lines]
    assert result['file_after_reset'] == [(str(apps), 'Recorder')], result
    settings = (card_file(img, '/b310e-os/settings.txt') or b'').decode().splitlines()
    assert f'vol_music={music}' in settings and f'vol_alarm={alarm}' in settings, settings
    print('PASS ' + json.dumps(result))


if __name__ == '__main__':
    main()
