#!/usr/bin/env python3
"""Text in QEMU: the fonts beyond ASCII, the multi-tap editor in several input
languages (os/ui/textedit.c, os/ui/textlang.c) and word completion
(os/ui/words.c), driven by the keypad in Notes; the store read through the
NET shell's "db" (test-db.py's Phone) and from the card image.

The card has Notes, a Turkish word list made by os/tools/mkwords.py from a
small frequency list written here, and a file with a long Turkish name.

  1. Notes, one note in four languages (English, # held: Turkish, German,
     Russian): "Hi çiğİIıi süß мир.€2" byte for byte: Turkish i/İ and ı/I in
     ABC and abc, ü and ß in German, Cyrillic, "." from key 1, "€" from the
     symbol picker (* held), "2" by holding 2; ui.input_lang follows # held.
  2. A second note: "Hello", then "he" and Dial: the learned word completes
     ("hello"); in Turkish "me", Dial twice: the word list's second word.
  3. Files: the long Turkish name in a list row, cut on a character.
  Restart: the learned words are in the store's words table; Settings ->
  Text input -> Language: English; a new note "He" + Dial: "Hello" again.
  Completion off in Settings: "he" + Dial stays "He"; a language switched on
  in Settings -> Text input -> Languages.
  Throughout: no character drawn as '?' (os_status.ui_missing_glyphs), and a
  screenshot of each step in build/os/test-text/.

  python os/tests/test-text.py

SPDX-License-Identifier: GPL-2.0-or-later
"""
import argparse
import json
from pathlib import Path
import runpy
import subprocess
import sys
import time

REPO = Path(__file__).resolve().parents[2]
BOOT = runpy.run_path(str(REPO / 'os/tests/test-boot.py'))
DBT = runpy.run_path(str(REPO / 'os/tests/test-db.py'))
Phone, get_value = DBT['Phone'], DBT['get_value']
status, string, wait_for, store, card_list = (
    BOOT[n] for n in ('status', 'string', 'wait_for', 'store', 'card_list'))
MENU, SETTINGS = BOOT['MENU'], BOOT['SETTINGS']
INPUT_ROWS = ['Language', 'Languages', 'Completion']
LANGS = ['en', 'tr', 'de', 'fr', 'es', 'ru']         # input_langs[] order

# the multi-tap letters of 2-9 (os/ui/textlang.c), small
KEYS = {
    'en': {2: 'abc2', 3: 'def3', 4: 'ghi4', 5: 'jkl5', 6: 'mno6', 7: 'pqrs7', 8: 'tuv8', 9: 'wxyz9'},
    'tr': {2: 'abcç2', 3: 'def3', 4: 'ghıiğ4', 5: 'jkl5', 6: 'mnoö6', 7: 'pqrsş7', 8: 'tuvü8',
           9: 'wxyz9'},
    'de': {2: 'abcä2', 3: 'def3', 4: 'ghi4', 5: 'jkl5', 6: 'mnoö6', 7: 'pqrsß7', 8: 'tuvü8',
           9: 'wxyz9'},
    'ru': {2: 'абвг2', 3: 'деёжз3', 4: 'ийкл4', 5: 'мноп5', 6: 'рсту6', 7: 'фхцч7', 8: 'шщъы8',
           9: 'ьэюя9'},
}
RU_EXTRA = 'ёЁъЪьЬ'
SYMBOLS = ('.,?!\'"-()@/:_;+&%*=<>#$[]{}\\~^`|¡¿«»‹›“”„‘’‚–—…•·§¶†‡°′″©®™№'
           '€$£¥₺₽₴₹₩¢₿¤±×÷−≠≈≤≥∞√∑∆πµ‰¹²³¼½¾←↑→↓')
TR_WORDS = ['merhaba 900', 'merkez 800', 'mersin 300', 'meyve 200', 'masa 150', 'çiçek 120',
            'ılık 90', 'istanbul 80']
LONG_NAME = 'Çok uzun bir Türkçe dosya adı ğüşıöç İĞÜŞÖÇ.txt'
NOTE1 = 'Hi çiğİIıi süß мир.€2'


class Editor:
    """keys for the phone, as a thumb would: the screen never dims (set first)"""

    def __init__(self, ph, out):
        self.ph, self.out, self.last = ph, out, None

    def key(self, k, ms=100, wait=0.3):
        self.ph.qmp.hmp(f'sendkey {k} {ms}')
        time.sleep(ms / 1000 + wait)

    def hold(self, k):
        self.key(k, 1000, 0.4)
        self.last = None

    def word(self, text, lang):
        """multi-tap: each letter's key that many times; the same key again
        waits for the letter before to be chosen"""
        for ch in text:
            k, i = next((k, s.index(ch)) for k, s in KEYS[lang].items() if ch in s)
            if self.last == k:
                time.sleep(1.2)
            for _ in range(i + 1):
                self.key(str(k), wait=0.15)
            self.last = k
        time.sleep(1.2)                 # the last letter chosen
        self.last = None

    def other(self, k, n=1):
        for _ in range(n):
            self.key(k)
        self.last = None

    def shot(self, name):
        self.ph.qmp.command('screendump', {'filename': str(self.out / f'{name}.png'), 'format': 'png'})


def title(ph):
    return string(ph.qt, status(ph.qt, ph.base)['ui_title'])


def want(ph, t, seconds=8):
    wait_for(ph.qt, ph.base, lambda s: string(ph.qt, s['ui_title']) == t, seconds, f'not {t}')


def missing(ph):
    return status(ph.qt, ph.base)['ui_missing_glyphs']


def notes(card):
    t = store(card.read_bytes()).tables.get('app/Notes/notes')
    return [r[0] for _, r in sorted(t.records.items())] if t else []


def learned(card):
    t = store(card.read_bytes()).tables.get('words')
    return {(r[1], r[0]): r[2] for r in t.records.values()} if t else {}


def open_notes(ph):
    ph.press('esc')
    ph.press('ret', 'Menu')
    ph.press('ret', 'Apps')
    ph.press('ret', 'Notes')


def new_note(ph):
    ph.press('ret', 'New note')
    time.sleep(0.5)


def save(ph):
    ph.press('ret', 'Notes')
    time.sleep(1.0)


def open_input_settings(ph):
    ph.press('esc')
    ph.press('ret', 'Menu')
    for _ in range(MENU.index('Settings')):
        ph.press('down')
    ph.press('ret', 'Settings')
    for _ in range(SETTINGS.index('Text input')):
        ph.press('down')
    ph.press('ret', 'Text input')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--qemu', type=Path, default=REPO / 'build/qemu/build/qemu-system-arm.exe')
    parser.add_argument('--build', type=Path, default=REPO / 'build/os')
    args = parser.parse_args()
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    out = (args.build / 'test-text').resolve()
    out.mkdir(parents=True, exist_ok=True)
    (out / 'settings.txt').write_bytes(b'usb_default=2\nscreen_timeout=0\n')
    (out / 'tr-freq.txt').write_text('\n'.join(TR_WORDS) + '\n', encoding='utf-8')
    (out / 'long.txt').write_bytes(b'uzun\n')
    subprocess.run([sys.executable, str(REPO / 'os/tools/mkwords.py'), '--lang', 'tr',
                    str(out / 'tr-freq.txt'), '-o', str(out / 'tr.wl')], check=True)
    card = out / 'card.img'
    subprocess.run([sys.executable, str(REPO / 'ports/rockbox/tests/make-sd-image.py'),
                    '--fat32', '--size-mib', '64', '--seconds', '1', '--force',
                    '--runtime', str(REPO / 'sdcard/.rockbox'),
                    '--rockbox', str(REPO / 'sdcard/payloads/rockbox.bin'),
                    '--add', f'b310e-os/settings.txt={out / "settings.txt"}',
                    '--add', f'b310e-os/apps/notes.app={(args.build / "modules/notes.app").resolve()}',
                    '--add', f'b310e-os/words/tr.wl={out / "tr.wl"}',
                    '--add', f'docs/{LONG_NAME}={out / "long.txt"}',
                    '--output', str(card)], check=True, capture_output=True)
    root = [n for n, d, h in card_list(card.read_bytes(), '/') if d and not h]
    result = {}

    # ---- boot 1
    ph = Phone(args, card, out, 'first')
    try:
        ed = Editor(ph, out)
        assert ph.cmd('db set ui.input_langs "en,tr,de,ru" str') == 'ui.input_langs str "en,tr,de,ru"'
        ph.cmd('db set ui.input_lang en str')
        m0 = missing(ph)

        # 1. four languages in one note
        open_notes(ph)
        new_note(ph)
        ed.word('hi', 'en')                     # Abc: H, then small
        ed.other('0')                           # a space
        ed.hold('slash')                        # # held: Turkish
        ed.word('çiğ', 'tr')
        ed.other('minus')                       # *: ABC
        ed.word('iı', 'tr')                     # İ I
        ed.other('minus', 3)                    # 123, Abc, abc
        ed.word('ıi', 'tr')
        ed.shot('turkish')
        ed.other('0')
        ed.hold('slash')                        # German
        ed.word('süß', 'de')
        ed.shot('german')
        ed.other('0')
        ed.hold('slash')                        # Russian
        ed.word('мир', 'ru')
        ed.other('1')                           # key 1: "."
        time.sleep(1.2)
        ed.hold('minus')                        # * held: the symbol picker
        want(ph, 'Symbols')
        at = len(RU_EXTRA) + SYMBOLS.index('€')
        ed.shot('picker')
        ed.other('down', at // 8)
        ed.other('right', at % 8)
        ed.shot('picker-euro')
        ph.press('ret', 'New note')
        ed.hold('2')                            # 2 held: the digit
        ed.shot('note1')
        result['missing_in_editor'] = missing(ph) - m0
        save(ph)
        result['note1'] = notes(card)
        assert result['note1'] == [NOTE1], result
        assert get_value(ph, 'ui.input_lang') == ('str', '"ru"'), get_value(ph, 'ui.input_lang')
        ed.shot('notes-list')

        # 2. a learned word, the word list
        new_note(ph)
        ed.hold('slash')                        # Russian -> English
        ed.word('hello', 'en')
        ed.other('0')                           # learned at the space
        ed.word('he', 'en')
        time.sleep(0.5)
        ed.shot('completion-offered')
        ed.other('kp_enter')                    # Dial: takes it
        ed.shot('completion-taken')
        ed.other('0')
        ed.hold('slash')                        # Turkish
        ed.word('me', 'tr')
        time.sleep(0.5)
        ed.shot('wordlist-offered')
        ed.other('kp_enter')                    # merhaba
        ed.other('kp_enter')                    # merkez
        ed.shot('wordlist-second')
        save(ph)
        result['note2'] = notes(card)[-1]
        assert result['note2'] == 'Hello hello merkez', result

        # 3. Files: the long name, cut on a character
        ph.press('esc')
        ph.press('ret', 'Menu')
        for _ in range(MENU.index('Files')):
            ph.press('down')
        ph.press('ret')
        for _ in range(root.index('docs')):
            ph.press('down')
        ph.press('ret', '/docs')
        time.sleep(0.5)
        ed.shot('files-long-name')
        result['missing_total'] = missing(ph) - m0
        assert result['missing_in_editor'] == 0 and result['missing_total'] == 0, result
        ph.power_off()
    except Exception:
        print(json.dumps(result, indent=2, ensure_ascii=False))
        raise
    finally:
        ph.close()

    words = learned(card)
    result['learned'] = {f'{l}:{w}': n for (l, w), n in words.items()}
    assert words.get(('en', 'hello'), 0) >= 2, result
    assert ('de', 'süß') in words and ('ru', 'мир') in words and ('tr', 'çiğiııi') in words, result

    # ---- boot 2: the words are back; Settings
    ph = Phone(args, card, out, 'second')
    try:
        ed = Editor(ph, out)
        m0 = missing(ph)
        open_input_settings(ph)
        ed.shot('settings-input')
        ph.press('ret', 'Language')
        cur = ['en', 'tr', 'de', 'ru'].index(get_value(ph, 'ui.input_lang')[1].strip('"'))
        for _ in range(cur):
            ph.press('up')
        ph.press('ret', 'Text input')
        assert get_value(ph, 'ui.input_lang') == ('str', '"en"'), get_value(ph, 'ui.input_lang')

        open_notes(ph)
        new_note(ph)
        ed.word('he', 'en')
        time.sleep(0.5)
        ed.other('kp_enter')
        ed.shot('after-restart')
        save(ph)
        result['note3'] = notes(card)[-1]
        assert result['note3'] == 'Hello', result

        # completion off
        open_input_settings(ph)
        for _ in range(INPUT_ROWS.index('Completion')):
            ph.press('down')
        ph.press('ret')
        time.sleep(0.5)
        assert get_value(ph, 'ui.completion') == ('int', '0'), get_value(ph, 'ui.completion')
        # a language on: Languages -> Français
        ph.press('up')
        ph.press('ret', 'Languages')
        for _ in range(LANGS.index('fr')):
            ph.press('down')
        ph.press('ret')
        ed.shot('settings-languages')
        result['langs'] = get_value(ph, 'ui.input_langs')
        assert result['langs'] == ('str', '"en,tr,de,fr,ru"'), result
        open_notes(ph)
        new_note(ph)
        ed.word('he', 'en')
        time.sleep(0.5)
        ed.other('kp_enter')
        ed.shot('completion-off')
        save(ph)
        result['note4'] = notes(card)[-1]
        assert result['note4'] == 'He', result
        result['missing_second'] = missing(ph) - m0
        assert result['missing_second'] == 0, result
        ph.power_off()
    except Exception:
        print(json.dumps(result, indent=2, ensure_ascii=False))
        raise
    finally:
        ph.close()
    (out / 'result.json').write_text(json.dumps(result, indent=2, ensure_ascii=False), encoding='utf-8')
    print('PASS ' + json.dumps(result, ensure_ascii=False))


if __name__ == '__main__':
    main()
