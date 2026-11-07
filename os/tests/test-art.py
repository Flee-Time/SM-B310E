#!/usr/bin/env python3
"""Album art in QEMU: covers from the tags and beside the files.

The cover is four quadrants, red, green, blue and white, so the Now
playing screen's picture can be checked at its quarters. Tracks in /music
(4 s tones, made with ffmpeg):
  01  MP3, a 1000 px 4:2:0 JPEG in ID3v2 APIC   (decoded at 1/8: DC only)
  02  MP3, a 200 px PNG in APIC                 (inflate, the filters)
  03  FLAC, a 300 px JPEG PICTURE block         (1/4)
  04  M4A (AAC), a 300 px JPEG in covr
  05  Ogg Vorbis, the 1000 px JPEG as a METADATA_BLOCK_PICTURE comment
      (base64 across Ogg pages)
  06  Opus, a 48 px JPEG the same way           (full size, not blown up)
  07  MP3, a 1000 px progressive JPEG in APIC  (its DC scans only)
  y/09 WAV, no picture anywhere                  (no cover)
  z/08 WAV with z/cover.jpg beside it            (the folder's picture)
Each track's picture is checked in a screenshot, and the card log must say
where each cover came from; when the list repeats, the first cover must
come from the thumbnail cache. The player is music.app, opened from
Apps -> Music.

  python os/tests/test-art.py

SPDX-License-Identifier: GPL-2.0-or-later
"""
import argparse
import base64
import json
import os
from pathlib import Path
import runpy
import shutil
import struct
import subprocess
import sys
import time

from PIL import Image

REPO = Path(__file__).resolve().parents[2]
boot = runpy.run_path(str(Path(__file__).with_name('test-boot.py')))
QT, capture = boot['QT'], boot['capture']
symbol, status, string, wait_for, free_port, card_file = (
    boot[n] for n in ('symbol', 'status', 'string', 'wait_for', 'free_port', 'card_file'))
MENU, MAGIC = boot['MENU'], boot['MAGIC']
ART_X, ART_Y, ART = 34, 11 + 15 + 13, 60       # where Now playing puts it
QUADS = {'red': (255, 0, 0), 'green': (0, 255, 0), 'blue': (0, 0, 255),
         'white': (255, 255, 255)}


def ffmpeg(*args):
    subprocess.run(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-y', *args], check=True)


def cover(path, side, fmt, **kw):
    im = Image.new('RGB', (side, side))
    h = side // 2
    for (x, y), c in zip(((0, 0), (h, 0), (0, h), (h, h)), QUADS.values()):
        im.paste(c, (x, y, x + h, y + h))
    im.save(path, fmt, **kw)
    return path


def picture_block(jpeg):
    """A FLAC PICTURE block (front cover, image/jpeg), for a Vorbis comment."""
    data = jpeg.read_bytes()
    w, h = Image.open(jpeg).size
    mime = b'image/jpeg'
    return (struct.pack('>II', 3, len(mime)) + mime + struct.pack('>I', 0) +
            struct.pack('>IIIII', w, h, 24, 0, len(data)) + data)


def tone(hz, seconds=4):
    t = f'0.4*sin(2*PI*{hz}*t)'
    return ['-f', 'lavfi', '-i', f'aevalsrc={t}|{t}:s=44100:d={seconds}']


def make(out):
    big = cover(out / 'big.jpg', 1000, 'JPEG', quality=90, subsampling=2)     # 4:2:0
    mid = cover(out / 'mid.jpg', 300, 'JPEG', quality=90)
    tiny = cover(out / 'tiny.jpg', 48, 'JPEG', quality=95)
    png = cover(out / 'cov.png', 200, 'PNG')
    prog = cover(out / 'prog.jpg', 1000, 'JPEG', quality=90, progressive=True, subsampling=2)
    music = out / 'music'
    (music / 'z').mkdir(parents=True)
    (music / 'y').mkdir(parents=True)
    tags = lambda n: ['-metadata', f'title=Art {n}', '-metadata', 'artist=B310E test']
    pic = ['-map', '0:a', '-map', '1:v', '-c:v', 'copy', '-disposition:v:0', 'attached_pic']
    ffmpeg(*tone(440), '-i', str(big), *pic, '-c:a', 'libmp3lame', '-b:a', '96k',
           '-id3v2_version', '3', *tags(1), str(music / '01-mp3-big.mp3'))
    ffmpeg(*tone(550), '-i', str(png), *pic, '-c:a', 'libmp3lame', '-b:a', '96k',
           '-id3v2_version', '3', *tags(2), str(music / '02-mp3-png.mp3'))
    ffmpeg(*tone(660), '-i', str(mid), *pic, '-c:a', 'flac', *tags(3),
           str(music / '03-flac.flac'))
    ffmpeg(*tone(770), '-i', str(mid), *pic, '-c:a', 'aac', '-b:a', '96k', *tags(4),
           str(music / '04-aac.m4a'))
    for name, jpeg, codec in (('05-vorbis.ogg', big, ['-c:a', 'libvorbis', '-q:a', '2']),
                              ('06-opus.opus', tiny, ['-c:a', 'libopus', '-b:a', '64k'])):
        b64 = base64.b64encode(picture_block(jpeg)).decode()
        meta = out / f'{name}.meta'
        meta.write_text(';FFMETADATA1\nMETADATA_BLOCK_PICTURE=' + b64.replace('=', '\\=') +
                        f'\ntitle=Art {name[:2]}\nartist=B310E test\n', newline='\n')
        ffmpeg(*tone(880 if name[:2] == '05' else 990), '-i', str(meta), '-map', '0:a',
               '-map_metadata', '1', *codec, str(music / name))
    ffmpeg(*tone(1100), '-i', str(prog), *pic, '-c:a', 'libmp3lame', '-b:a', '96k',
           '-id3v2_version', '3', *tags(7), str(music / '07-mp3-prog.mp3'))
    ffmpeg(*tone(1200), '-c:a', 'pcm_s16le', str(music / 'z' / '08-wav.wav'))
    ffmpeg(*tone(1300), '-c:a', 'pcm_s16le', str(music / 'y' / '09-wav.wav'))
    shutil.copy(mid, music / 'z' / 'cover.jpg')
    return music


def quadrants(png):
    """The four quarters' mean colours of the cover as shown, or None."""
    im = Image.open(png).convert('RGB')
    q = ART // 4
    found = {}
    for name, (dx, dy) in zip(QUADS, ((q, q), (3 * q, q), (q, 3 * q), (3 * q, 3 * q))):
        px = [im.getpixel((ART_X + dx + i, ART_Y + dy + j)) for i in range(-3, 4) for j in range(-3, 4)]
        found[name] = tuple(sum(p[c] for p in px) // len(px) for c in range(3))
    return found


def is_cover(found, small=False):
    if not found:
        return False
    for name, want in QUADS.items():
        got = found[name]
        if any(abs(g - w) > 48 for g, w in zip(got, want)):
            return False
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qemu', type=Path, default=REPO / 'build/qemu/build/qemu-system-arm.exe')
    parser.add_argument('--build', type=Path, default=REPO / 'build/os')
    args = parser.parse_args()
    out = (args.build / 'test-art').resolve()
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    assert shutil.which('ffmpeg'), 'ffmpeg is needed to make the test files'
    elf, image = args.build / 'b310e-os.elf', args.build / 'b310e-os.bin'
    base, m_cur = symbol(elf, 'os_status'), symbol(elf, 'music_cur')
    music = make(out)
    tracks = sorted(str(p.relative_to(music)).replace('\\', '/') for p in music.rglob('*')
                    if p.suffix in ('.mp3', '.flac', '.m4a', '.ogg', '.opus', '.wav'))
    adds = ['--add', f'b310e-os/apps/music.app={(args.build / "modules/music.app").resolve()}']
    for cdc in sorted((args.build / 'codecs').glob('*.cdc')):
        adds += ['--add', f'b310e-os/codecs/{cdc.name}={cdc}']
    for p in music.rglob('*'):
        if p.is_file():
            adds += ['--add', f'music/{p.relative_to(music).as_posix()}={p}']
    card = out / 'card.img'
    subprocess.run([sys.executable, str(REPO / 'ports/rockbox/tests/make-sd-image.py'),
                    '--fat32', '--size-mib', '64', '--seconds', '1', '--force',
                    '--runtime', str(REPO / 'sdcard/.rockbox'),
                    '--rockbox', str(REPO / 'sdcard/progs/rockbox.bin'),
                    *adds, '--output', str(card)], check=True, capture_output=True)
    qmp_port, qtest_port = free_port(), free_port()
    cmd = [str(args.qemu.resolve()), '-M', 'b310e,boot-mode=rockbox',
           '-display', 'none', '-serial', 'none', '-monitor', 'none',
           '-drive', f'file={image.resolve().as_posix()},format=raw,if=none,id=os,readonly=on',
           '-drive', f'file={card.as_posix()},format=raw,if=none,id=sdcard',
           '-qmp', f'tcp:127.0.0.1:{qmp_port},server=on,wait=off',
           '-qtest', f'tcp:127.0.0.1:{qtest_port},server=on,wait=off',
           '-audiodev', 'none,id=audio0', '-global', 'sc6530_adi.audiodev=audio0']
    env = os.environ.copy()
    if os.name == 'nt':
        env['PATH'] = r'D:\Toolchains\msys64\mingw64\bin;' + env.get('PATH', '')
    result = {'tracks': tracks}
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

        def key(k, want=None, wait=0.35):
            qmp.hmp(f'sendkey {k} {200 if k == "esc" else 100}')
            if want:
                wait_for(qt, base, lambda s: string(qt, s['ui_title']) == want, 8, f'not {want}')
            time.sleep(wait)

        def shot(name):
            path = out / f'{name}.png'
            qmp.command('screendump', {'filename': str(path), 'format': 'png'})
            return path

        wait_for(qt, base, lambda s: s['magic'] == MAGIC, 10, 'OS never started')
        wait_for(qt, base, lambda s: s['ui_depth'] == 1, 60, 'no UI')
        time.sleep(1)
        key('ret', 'Menu')
        for _ in range(MENU.index('Apps')):
            key('down')
        key('ret', 'Apps')
        key('ret', 'Music', wait=0.8)                   # the only app on the card
        key('ret', 'Now playing')                           # the first track
        seen = {}
        for i, name in enumerate(tracks):
            end = time.monotonic() + 15
            while time.monotonic() < end and qt.read(m_cur) != i:
                time.sleep(0.05)
            assert qt.read(m_cur) == i, f'track {i} {name} never came: {result}'
            ok = False
            time.sleep(0.4)                     # the screen shows the new track
            # the cover comes from a low thread: look until the track ends
            while time.monotonic() < end and qt.read(m_cur) == i:
                found = quadrants(shot(f'{i:02d}'))
                if is_cover(found):
                    ok = True
                    break
                time.sleep(0.25)
            seen[name] = {'cover': ok, 'colours': found}
            if i + 1 < len(tracks):
                while qt.read(m_cur) == i and time.monotonic() < end + 5:
                    time.sleep(0.05)
        result['seen'] = {n: v['cover'] for n, v in seen.items()}
        # the list repeats: the first cover again, now from the cache
        end = time.monotonic() + 15
        while time.monotonic() < end and qt.read(m_cur) != 0:
            time.sleep(0.1)
        time.sleep(1.5)
        result['again'] = is_cover(quadrants(shot('again')))
        key('1', wait=0.5)                  # past the screen timeout: it only wakes
        key('f2', 'Music')
        key('esc', wait=0.5)
        key('ret', 'Menu')                                   # power off: the log reaches the card
        for _ in range(MENU.index('Power')):
            key('down')
        key('ret', 'Power')
        key('ret', 'Power off')
        qmp.hmp('sendkey f1 100')
        p.wait(timeout=15)
    except Exception:
        print(json.dumps(result, indent=2, default=str))
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
    result['log'] = [l.strip() for l in log.splitlines() if 'art:' in l]
    (out / 'result.json').write_text(json.dumps(result, indent=2))
    want = {t: not t.startswith('y/') for t in tracks}
    assert result['seen'] == want, json.dumps(result, indent=2)
    for t in tracks:
        line = next((l for l in result['log'] if f'/music/{t}:' in l), '')
        if t.startswith('y/'):
            assert 'no cover' in line, (t, line)
        elif t.startswith('z/'):
            assert 'cover from /music/z/cover.jpg' in line, (t, line)
        else:
            assert 'cover from the tags' in line, (t, line)
    first = tracks[0]
    assert result['again'], result
    assert any(f'/music/{first}: cover from the tags (the cache)' in l for l in result['log']),         result['log']
    print('PASS ' + json.dumps({k: v for k, v in result.items() if k != 'tracks'}))


if __name__ == '__main__':
    main()
