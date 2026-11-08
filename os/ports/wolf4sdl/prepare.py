#!/usr/bin/env python3
"""Wolf4SDL on B310E OS: copy upstream's source (build/wolf4sdl, unchanged)
to the build and make the changes the ARM926 needs, each checked:

 1. wl_def.h packs every struct (#pragma pack(1)): their pointers and
    ints at odd addresses. An unaligned word load on this ARMv5 does not
    fault, it rotates: where Wolf4SDL takes such a field's address, wrong
    pointers. The pragma goes ...
 2. ... and the two structs read from the map files as they are on disk
    (maptype in id_ca.h, mapfiletype in id_ca.c) are packed one by one.
 3. dosbox/dbopl.cpp packs its own classes the same way: that goes too
    (id_sd.c, which holds the OPL chip, must see the same layout).

 4. PollKeyboardMove (wl_play.c) turns at full speed from the first tic;
    with a frame every few tics a tap turns too far. wolf_turn (wolf_glue.cpp)
    starts it slowly and brings it up to speed.

 5. Its itoa and ltoa (wl_def.h) take the room to print into from strlen of
    the buffer they are given, which is not yet set: 100 became "1" or "10",
    whatever the stack held. The status bar's numbers went wrong with it.

Nothing else of Wolf4SDL's is changed.

  python os/ports/wolf4sdl/prepare.py <upstream dir> <out dir>

SPDX-License-Identifier: GPL-2.0-or-later
"""
from pathlib import Path
import shutil
import sys

CHANGES = {
    'wl_play.c': [("""    if (Keyboard[dirscan[di_west]])
        controlx -= delta;
    if (Keyboard[dirscan[di_east]])
        controlx += delta;
""", """    controlx += wolf_turn(Keyboard[dirscan[di_west]], Keyboard[dirscan[di_east]], delta, tics);
"""),
                  ("""void PollKeyboardMove (void)
""", """int wolf_turn(int left, int right, int delta, int tics);     /* wolf_glue.cpp */

void PollKeyboardMove (void)
""")],
    'wl_def.h': [('\n#pragma pack(1)\n',
                  '\n/* (B310E: no global #pragma pack(1); see os/ports/wolf4sdl/prepare.py) */\n'),
                 ('static inline char *itoa (int value, char *string, int radix)\n    {\n        int len = strlen(string) + 1;',
                  'static inline char *itoa (int value, char *string, int radix)\n    {\n        int len = 12;'),
                 ('static inline char *ltoa (long value, char *string, int radix)\n    {\n        int len = strlen(string) + 1;',
                  'static inline char *ltoa (long value, char *string, int radix)\n    {\n        int len = 12;')],
    'id_ca.h': [('    char    name[16];\n} maptype;',
                 '    char    name[16];\n} __attribute__((packed)) maptype;')],
    'id_ca.c': [('    int32_t headeroffsets[NUMMAPS];\n} mapfiletype;',
                 '    int32_t headeroffsets[NUMMAPS];\n} __attribute__((packed)) mapfiletype;')],
    'dbopl.cpp': [('\n#pragma pack(1)\n',
                   '\n/* (B310E: no #pragma pack(1); see os/ports/wolf4sdl/prepare.py) */\n')],
}


def main():
    src, out = Path(sys.argv[1]), Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)
    for path in list(src.glob('*.[ch]')) + list(src.glob('*.inc')) + list(src.glob('dosbox/*')):
        dest = out / path.relative_to(src)
        dest.parent.mkdir(parents=True, exist_ok=True)
        text = path.read_bytes().decode('latin-1')
        for old, new in CHANGES.get(path.name, []):
            if text.replace('\r\n', '\n').count(old) != 1:
                sys.exit(f'prepare.py: {path.name} is not what it was (looked for {old!r})')
            text = text.replace('\r\n', '\n').replace(old, new)
        if not dest.exists() or dest.read_bytes() != text.encode('latin-1'):
            dest.write_bytes(text.encode('latin-1'))
    (out / '.prepared').write_text('ok\n')


if __name__ == '__main__':
    main()
