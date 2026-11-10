#!/usr/bin/env python3
"""Shadow Warrior on B310E OS: copy upstream's engine and game (build/jfbuild,
build/jfsw, unchanged) to the build with the engine's changes (os/ports/build/engine.py)
and the game's, each checked.

 1. Maps: 6400 walls ($WOODS has 6232), 2048 sprites ($YAMATO starts with 1961; fpdoom's);
    6144 tiles (its reserve.h: under 6144; SW.GRP has 5632).
 2. One player's room, not eight, and no network's sync queue in it (fpdoom's way):
    its PLAYER was 20 KB each. The console keeps 8 arguments, not 100 (25 KB).
 3. No voxels (the engine has room for none: os/ports/build/engine.py): items are
    sprites, as in the game of 1997. Its 3D models filled the tile cache, locked.
 4. A movie that does not fit the tile cache is skipped (upstream stops the game).
 5. A level's precache is meant for far more memory (it read 40 MB into 480 KB): the
    level's art and the effects up to 60% of the cache, and what is not yet seen is
    the cheapest to throw out (lock 100; on screen it is 199), as Duke's.

  python os/ports/sw/prepare.py <jfbuild> <jfsw> <out>

SPDX-License-Identifier: GPL-2.0-or-later
"""
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'build'))
import engine  # noqa: E402

CHANGES = {
    'anim.c': [
        ('''        length = kfilelength(handle);

        allocache((void **) &anm_ptr[anim_num]''', '''        length = kfilelength(handle);
        {
            extern size_t cachesize;    /* (the engine's tile cache) */

            if ((size_t)length + sizeof(anim_t) + 256 * 1024 > cachesize) {
                kclose(handle);         /* no room: it would stop the game */
                return(NULL);
            }
        }

        allocache((void **) &anm_ptr[anim_num]'''),
    ],
    'cache.c': [
        ('    for (i = 0; i < MAXTILES; i++)\n        {\n        if ((TEST(gotpic[i>>3], 1<<(i&7))) && (!waloff[i]))\n            {\n            loadtile(i);\n            cnt++;\n            if (!(cnt&7))\n                {\n                handleevents();\n                getpackets();\n                }\n            }\n        }\n\n    memset(gotpic,0,sizeof(gotpic));\n',
         "    {\n        extern size_t cachesize;        /* (the engine's tile cache) */\n        size_t used = 0;\n\n        for (i = 0; i < MAXTILES; i++)  /* up to 60% of the cache (os/ports/sw/prepare.py) */\n            {\n            if ((TEST(gotpic[i>>3], 1<<(i&7))) && (!waloff[i]))\n                {\n                size_t size = (size_t)tilesizx[i] * tilesizy[i];\n\n                if (used + size > cachesize * 60 / 100)\n                    continue;\n                used += size;\n                loadtile(i);\n                cnt++;\n                if (!(cnt&7))\n                    {\n                    handleevents();\n                    getpackets();\n                    }\n                }\n            }\n        for (i = 0; i < MAXTILES; i++)  /* not yet seen: the first to go */\n            if (waloff[i] && walock[i] < 200)\n                walock[i] = 100;\n    }\n\n    memset(gotpic,0,sizeof(gotpic));\n"),
    ],
    'scrip2.c': [
        ('        if (!qloadkvx(lNumber,sName)) {\n',
         '        if (0) {                        /* no voxels (os/ports/sw/prepare.py) */\n'),
    ],
    'game.h': [
        ('#define MAX_SW_PLAYERS_SW  (4)', '#define MAX_SW_PLAYERS_SW  (1)'),
        ('#define MAX_SW_PLAYERS_REG (8)', '#define MAX_SW_PLAYERS_REG (1)'),
        ('\t#define SYNCFIFOSIZ 1024', '\t#define SYNCFIFOSIZ 1'),
    ],
    'console.c': [
        ('#define MAX_USER_ARGS           100', '#define MAX_USER_ARGS           8'),
    ],
}


def main():
    build, game, out = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
    engine.prepare(build, game / 'src', out, CHANGES, tiles=6144, walls=6400, sprites=2048, players=2)


if __name__ == '__main__':
    main()
