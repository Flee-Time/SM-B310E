#!/usr/bin/env python3
"""Duke Nukem 3D on B310E OS: copy upstream's engine and game (build/jfbuild,
build/jfduke3d, unchanged) to the build and make the changes a 4 MB phone needs,
each checked. Upstream sizes its tables for the biggest maps Build ever had:
5.6 MB of them, before any code or data.

 1. Maps: 6144 walls, 2048 sprites, 1024 sectors. Duke's own maps need 5471
    walls (E2L8) and 1278 sprites (E4L11). A map beyond them is refused.
 2. 5120 tiles (Duke has 4944, and the game's own few) and the cache's objects.
 3. The screen is 320 x 200: tables sized by it shrink (one of them from 737 KB).
    320 rows, not 200: the tilted view (dying) draws to a tile of 320 x 320.
 4. No voxels in Duke: their tables go, and distrecip (256 KB, voxels only).
 5. Two players' room (no network here): their input queues shrink.
 6. A movie that does not fit the tile cache is skipped (upstream stops the game).
 7. Room for 16384 words of game script (the full game's has 12130).
 8. A level's precache is meant for far more memory: all it loads is as dear to the
    cache as what is on screen (lock 199, one less a second), so for seconds after
    it the cheapest thing to throw out was the tile just read, and the sky's tiles
    took each other's place: 28 MB read again and again, a tile a few times a frame.
    Now the level's own art goes in up to 45% of the cache, then the sounds up to
    20%, then the effects; the art not yet seen is cheap (lock 100), and the effects
    stay dear, so that an explosion does not wait for the card.
 9. The tile cache is what is left of the memory, so what the game keeps needlessly
    comes out of it: the script's 4096 labels (for a console command nobody can type
    here, and they stayed as a hole below the cache), most of the console's text, and
    the cache's room for 5120 objects (it holds a few hundred). 551 KB became 900.
11. Walls and sprites four columns at a time (upstream's way with x86 assembly, off in its C
    code), floors the same: a word to the frame for each four pixels. The PSRAM takes
    each write by itself: the D-cache does not fetch a line to write to it. The two
    loops that draw most of the screen are in ARM code (draw4.S, checked pixel for
    pixel against the C); a compiler keeps a third more on the stack.
10. See-through effects as a chessboard, not a blend for each pixel: less than half the
    work where it hurts most, and the screen's own halving of the columns blends them.

  python os/ports/duke/prepare.py <jfbuild> <jfduke3d> <out>

SPDX-License-Identifier: GPL-2.0-or-later
"""
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'build'))
import engine  # noqa: E402

CHANGES = {
    'gamedefs.h': [
        ('#define MAXPLAYERS 16', '#define MAXPLAYERS 2'),
    ],
    'duke3d.h': [
        ('#define MAXSCRIPTSIZE 32768', '#define MAXSCRIPTSIZE 16384'),
    ],
    'game.c': [
        ('''    label = realloc(label, labelcnt * MAXLABELLEN);
    labelcode = realloc(labelcode, labelcnt * sizeof(int));
''', '''    free(label);
    free(labelcode);
    label = NULL;
    labelcode = NULL;
    labelcnt = 0;                       /* (osdcmds.c's spawn looks them up: not here) */
'''),
    ],
    'premap.c': [
        ('''    precachenecessarysounds();

    cachegoodsprites();
''', '''    cachegoodsprites();
    memcpy(goodpic, gotpic, sizeof(goodpic));
'''),
        ('''void cacheit(void)
{
''', '''static unsigned char goodpic[sizeof(gotpic)];     /* the effects' (os/ports/duke/prepare.py) */

/* Every second or so: the cache's ageing does not reach the effects */
void keep_effects(void)
{
    int i;

    for(i=0;i<MAXTILES;i++)
        if ((goodpic[i>>3] & pow2char[i&7]) && waloff[i] != 0 && walock[i] < 199)
            walock[i] = 199;
}

void cacheit(void)
{
'''),
        ('''    j = 0;
    for(i=0;i<MAXTILES;i++) {
        if(gotpic[i>>3] & pow2char[i&7]) {
            if (waloff[i] == 0)
                loadtile((short)i);

            j++;
        } else continue;

        if((j&7) == 0) { handleevents(); getpackets(); }
    }
''', '''    {
        size_t used = 0;

        j = 0;
        for(i=0;i<MAXTILES;i++) {       /* the level's own, up to their share */
            if((gotpic[i>>3] & pow2char[i&7]) && !(goodpic[i>>3] & pow2char[i&7])) {
                size_t size = (size_t)tilesizx[i] * tilesizy[i];

                if (waloff[i] == 0 && used + size <= cachesize * 45 / 100) {
                    loadtile((short)i);
                    used += size;
                }
                j++;
            } else continue;

            if((j&7) == 0) { handleevents(); getpackets(); }
        }
    }

    precachenecessarysounds();

    j = 0;
    for(i=0;i<MAXTILES;i++) {           /* the effects, last: they stay */
        if(goodpic[i>>3] & pow2char[i&7]) {
            if (waloff[i] == 0)
                loadtile((short)i);

            j++;
        } else continue;

        if((j&7) == 0) { handleevents(); getpackets(); }
    }

    {
        size_t art = 0, effects = 0;

        for(i=0;i<MAXTILES;i++) {
            if (waloff[i] == 0 || walock[i] >= 200)
                continue;
            if (goodpic[i>>3] & pow2char[i&7])
                effects += (size_t)tilesizx[i] * tilesizy[i];
            else {
                art += (size_t)tilesizx[i] * tilesizy[i];
                walock[i] = 100;        /* before the first frame shows what is seen */
            }
        }
        buildprintf("Cache: %u KB level art, %u KB effects, of %u KB\\n",
                    (unsigned)(art / 1024), (unsigned)(effects / 1024),
                    (unsigned)(cachesize / 1024));
    }
'''),
        ('''            if( (j&7) == 0 )
                { handleevents(); getpackets(); }
            getsound(i);
''', '''            if( (j&7) == 0 )
                { handleevents(); getpackets(); }
            if (bytes > cachesize / 5)
                break;
            getsound(i);
            if (Sound[i].ptr)
                bytes += soundsiz[i];
'''),
        ('''void precachenecessarysounds(void)
{
    short i, j;
''', '''extern size_t cachesize;

void precachenecessarysounds(void)
{
    short i, j;
    size_t bytes = 0;
'''),
    ],
    'menues.c': [
        ('''    length = kfilelength(handle);

    walock[TILE_ANIM] = 219+t;
''', '''    length = kfilelength(handle);
    {
        extern size_t cachesize;        /* (the engine's tile cache) */

        if ((size_t)length + 256 * 1024 > cachesize) {
            kclose(handle);             /* no room: it would stop the game */
            return;
        }
    }

    walock[TILE_ANIM] = 219+t;
'''),
    ],
}


def main():
    build, game, out = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
    engine.prepare(build, game / 'src', out, CHANGES, tiles=5120)


if __name__ == '__main__':
    main()
