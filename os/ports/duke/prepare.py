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

CHANGES = {
    'build.h': [
        ('#define MAXSECTORS MAXSECTORSV8', '#define MAXSECTORS MAXSECTORSV7'),
        ('#define MAXWALLS MAXWALLSV8', '#define MAXWALLS 6144'),
        ('#define MAXSPRITES MAXSPRITESV8', '#define MAXSPRITES 2048'),
        ('#define MAXTILES 9216', '#define MAXTILES 5120'),
        ('#define MAXVOXELS 4096', '#define MAXVOXELS 8'),
        ('#define MAXXDIM 2880', '#define MAXXDIM 320'),
        ('#define MAXYDIM 1800', '#define MAXYDIM 320'),
        ('#define MAXSPRITESONSCREEN 2048', '#define MAXSPRITESONSCREEN 512'),
        ('#define MAXPLAYERS 16', '#define MAXPLAYERS 2'),
    ],
    'mmulti.h': [
        ('#define MAXMULTIPLAYERS 16', '#define MAXMULTIPLAYERS 2'),
    ],
    'gamedefs.h': [
        ('#define MAXPLAYERS 16', '#define MAXPLAYERS 2'),
    ],
    'duke3d.h': [
        ('#define MAXSCRIPTSIZE 32768', '#define MAXSCRIPTSIZE 16384'),
    ],
    'engine_priv.h': [
        ('#define MAXPERMS 1024', '#define MAXPERMS 256'),
    ],
    'engine.c': [
        ('''	setupmvlineasm(globalshiftval);

#ifndef USING_A_C
''', '''	setupmvlineasm(globalshiftval);

#if 1	// (a-c.c has them now: os/ports/duke/prepare.py)
'''),
        ('''	setupvlineasm(globalshiftval);

#ifndef USING_A_C
''', '''	setupvlineasm(globalshiftval);

#if 1	// (a-c.c has them now: os/ports/duke/prepare.py)
'''),
        ('static unsigned int distrecip[65536];', 'static unsigned int distrecip[1];'),
        ('for(i=1;i<65536;i++) distrecip[i] = divscale20(xdimen,i);', ''),
    ],
    'cache1d.c': [
        ('#define MAXCACHEOBJECTS 9216', '#define MAXCACHEOBJECTS 2048'),
    ],
    'osd.c': [
        ('#define TEXTSIZE 16384', '#define TEXTSIZE 4096'),
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
    'a.h': [
        ('void vlineasm1(int vinc, void *paloffs, int cnt, unsigned int vplc, void *bufplc, void *p);',
         '''int vlineasm1(int vinc, void *paloffs, int cnt, unsigned int vplc, void *bufplc, void *p);
int prevlineasm1(int vinc, void *paloffs, int cnt, unsigned int vplc, void *bufplc, void *p);
void vlineasm4(int cnt, void *p);'''),
        ('void mvlineasm1(int vinc, void *paloffs, int cnt, unsigned int vplc, void *bufplc, void *p);',
         '''int mvlineasm1(int vinc, void *paloffs, int cnt, unsigned int vplc, void *bufplc, void *p);
void mvlineasm4(int cnt, void *p);'''),
    ],
    'a-c.c': [
        ('''void hlineasm4(int cnt, int skiploadincs,''', '''/* (ports/duke/draw4.S; its offsets) */
struct wall4 { unsigned int v[4]; int inc[4]; const void *buf[4]; int shift, step; };
struct floor4 { unsigned int bx, by; int bxinc, byinc; const void *buf, *pal; int xshift, yup, yshift; };
void draw4_wall(int rows, void *pp, const void *pal, struct wall4 *w);
void draw4_floor(int groups, void *pp, struct floor4 *f);

void hlineasm4(int cnt, int skiploadincs,'''),
        ('''void vlineasm1(int vinc, void *paloffs, int cnt, unsigned int vplc, void *bufplc, void *p)
{
	unsigned char *pp;

	gbuf = (unsigned char *)bufplc;
	gpal = (unsigned char *)paloffs;
	pp = (unsigned char *)p;
	for(;cnt>=0;cnt--)
	{
		*pp = gpal[gbuf[vplc>>glogy]];
		pp += bpl;
		vplc += vinc;
	}
}
''', '''int vlineasm1(int vinc, void *paloffs, int cnt, unsigned int vplc, void *bufplc, void *p)
{
	unsigned char *pp;

	gbuf = (unsigned char *)bufplc;
	gpal = (unsigned char *)paloffs;
	pp = (unsigned char *)p;
	for(;cnt>=0;cnt--)
	{
		*pp = gpal[gbuf[vplc>>glogy]];
		pp += bpl;
		vplc += vinc;
	}
	return (int)vplc;
}

int prevlineasm1(int vinc, void *paloffs, int cnt, unsigned int vplc, void *bufplc, void *p)
{
	return vlineasm1(vinc, paloffs, cnt, vplc, bufplc, p);
}

/* Four columns at once, as the engine does with x86 assembly: each row one word to the
 * frame, where a byte at a time is four writes to the PSRAM, which does not come into
 * the cache for a write (os/ports/duke/prepare.py) */
extern int vplce[4], vince[4];
extern intptr_t palookupoffse[4], bufplce[4];

void vlineasm4(int cnt, void *p)
{
	const unsigned char *b0 = (const unsigned char *)bufplce[0], *b1 = (const unsigned char *)bufplce[1];
	const unsigned char *b2 = (const unsigned char *)bufplce[2], *b3 = (const unsigned char *)bufplce[3];
	const unsigned char *p0 = (const unsigned char *)palookupoffse[0], *p1 = (const unsigned char *)palookupoffse[1];
	const unsigned char *p2 = (const unsigned char *)palookupoffse[2], *p3 = (const unsigned char *)palookupoffse[3];
	unsigned int v0 = vplce[0], v1 = vplce[1], v2 = vplce[2], v3 = vplce[3];
	const int i0 = vince[0], i1 = vince[1], i2 = vince[2], i3 = vince[3], sh = glogy, step = bpl;
	unsigned char *pp = (unsigned char *)p;

	if (cnt > 0 && !(((intptr_t)pp | step) & 3) && p0 == p1 && p0 == p2 && p0 == p3)
	{
		struct wall4 w = { { v0, v1, v2, v3 }, { i0, i1, i2, i3 }, { b0, b1, b2, b3 }, sh, step };

		draw4_wall(cnt, pp, p0, &w);
		vplce[0] = w.v[0]; vplce[1] = w.v[1]; vplce[2] = w.v[2]; vplce[3] = w.v[3];
		return;
	}
	if (((intptr_t)pp | step) & 3)
	{
		for(;cnt>0;cnt--,pp+=step)
		{
			pp[0] = p0[b0[v0>>sh]]; v0 += i0;
			pp[1] = p1[b1[v1>>sh]]; v1 += i1;
			pp[2] = p2[b2[v2>>sh]]; v2 += i2;
			pp[3] = p3[b3[v3>>sh]]; v3 += i3;
		}
	}
	else
	{
		for(;cnt>0;cnt--,pp+=step)
		{
			unsigned int w = p0[b0[v0>>sh]];

			w |= (unsigned int)p1[b1[v1>>sh]] << 8;
			w |= (unsigned int)p2[b2[v2>>sh]] << 16;
			w |= (unsigned int)p3[b3[v3>>sh]] << 24;
			*(unsigned int *)pp = w;
			v0 += i0; v1 += i1; v2 += i2; v3 += i3;
		}
	}
	vplce[0] = v0; vplce[1] = v1; vplce[2] = v2; vplce[3] = v3;
}

/* The same with holes: a row with none is one word */
void mvlineasm4(int cnt, void *p)
{
	const unsigned char *b0 = (const unsigned char *)bufplce[0], *b1 = (const unsigned char *)bufplce[1];
	const unsigned char *b2 = (const unsigned char *)bufplce[2], *b3 = (const unsigned char *)bufplce[3];
	const unsigned char *p0 = (const unsigned char *)palookupoffse[0], *p1 = (const unsigned char *)palookupoffse[1];
	const unsigned char *p2 = (const unsigned char *)palookupoffse[2], *p3 = (const unsigned char *)palookupoffse[3];
	unsigned int v0 = vplce[0], v1 = vplce[1], v2 = vplce[2], v3 = vplce[3];
	const int i0 = vince[0], i1 = vince[1], i2 = vince[2], i3 = vince[3], sh = glogy, step = bpl;
	const int words = !(((intptr_t)p | step) & 3);
	unsigned char *pp = (unsigned char *)p;

	for(;cnt>0;cnt--,pp+=step)
	{
		unsigned int a = b0[v0>>sh], b = b1[v1>>sh], c = b2[v2>>sh], d = b3[v3>>sh];

		v0 += i0; v1 += i1; v2 += i2; v3 += i3;
		if (words && a != 255 && b != 255 && c != 255 && d != 255)
			*(unsigned int *)pp = p0[a] | (unsigned int)p1[b] << 8 | (unsigned int)p2[c] << 16 | (unsigned int)p3[d] << 24;
		else
		{
			if (a != 255) pp[0] = p0[a];
			if (b != 255) pp[1] = p1[b];
			if (c != 255) pp[2] = p2[c];
			if (d != 255) pp[3] = p3[d];
		}
	}
	vplce[0] = v0; vplce[1] = v1; vplce[2] = v2; vplce[3] = v3;
}
'''),
        ('''void mvlineasm1(int vinc, void *paloffs, int cnt, unsigned int vplc, void *bufplc, void *p)
{
	unsigned char ch, *pp;

	gbuf = (unsigned char *)bufplc;
	gpal = (unsigned char *)paloffs;
	pp = (unsigned char *)p;
	for(;cnt>=0;cnt--)
	{
		ch = gbuf[vplc>>glogy]; if (ch != 255) *pp = gpal[ch];
		pp += bpl;
		vplc += vinc;
	}
}
''', '''int mvlineasm1(int vinc, void *paloffs, int cnt, unsigned int vplc, void *bufplc, void *p)
{
	unsigned char ch, *pp;

	gbuf = (unsigned char *)bufplc;
	gpal = (unsigned char *)paloffs;
	pp = (unsigned char *)p;
	for(;cnt>=0;cnt--)
	{
		ch = gbuf[vplc>>glogy]; if (ch != 255) *pp = gpal[ch];
		pp += bpl;
		vplc += vinc;
	}
	return (int)vplc;
}
'''),
        ('''	if (!skiploadincs) { gbxinc = asm1; gbyinc = asm2; }
	for(;cnt>=0;cnt--)
	{
		*pp = palptr[gbuf[((bx>>(32-glogx))<<glogy)+(by>>(32-glogy))]];
		bx -= gbxinc;
		by -= gbyinc;
		pp--;
	}
}
''', '''	if (!skiploadincs) { gbxinc = asm1; gbyinc = asm2; }
	for(;cnt>=0&&((intptr_t)pp&3)!=3;cnt--)
	{
		*pp = palptr[gbuf[((bx>>(32-glogx))<<glogy)+(by>>(32-glogy))]];
		bx -= gbxinc;
		by -= gbyinc;
		pp--;
	}
	if (cnt>=3)
	{
		struct floor4 f = { bx, by, gbxinc, gbyinc, gbuf, palptr, 32-glogx, glogy, 32-glogy };
		int groups = (cnt+1)>>2;

		draw4_floor(groups, pp-3, &f);
		bx = f.bx; by = f.by;
		pp -= groups<<2;
		cnt -= groups<<2;
	}
	for(;cnt>=3;cnt-=4,pp-=4)
	{					/* a word to the frame, not four bytes */
		unsigned int w = (unsigned int)palptr[gbuf[((bx>>(32-glogx))<<glogy)+(by>>(32-glogy))]] << 24;

		bx -= gbxinc; by -= gbyinc;
		w |= (unsigned int)palptr[gbuf[((bx>>(32-glogx))<<glogy)+(by>>(32-glogy))]] << 16;
		bx -= gbxinc; by -= gbyinc;
		w |= (unsigned int)palptr[gbuf[((bx>>(32-glogx))<<glogy)+(by>>(32-glogy))]] << 8;
		bx -= gbxinc; by -= gbyinc;
		w |= palptr[gbuf[((bx>>(32-glogx))<<glogy)+(by>>(32-glogy))]];
		bx -= gbxinc; by -= gbyinc;
		*(unsigned int *)(pp-3) = w;
	}
	for(;cnt>=0;cnt--)
	{
		*pp = palptr[gbuf[((bx>>(32-glogx))<<glogy)+(by>>(32-glogy))]];
		bx -= gbxinc;
		by -= gbyinc;
		pp--;
	}
}
'''),
        ('''void tvlineasm1(int vinc, void *paloffs, int cnt, unsigned int vplc, void *bufplc, void *p)
{
	unsigned char ch, *pp;

	gbuf = (unsigned char *)bufplc;
	gpal = (unsigned char *)paloffs;
	pp = (unsigned char *)p;
	if (transmode)
	{
		for(;cnt>=0;cnt--)
		{
			ch = gbuf[vplc>>glogy];
			if (ch != 255) *pp = gtrans[(*pp)+(gpal[ch]<<8)];
			pp += bpl;
			vplc += vinc;
		}
	}
	else
	{
		for(;cnt>=0;cnt--)
		{
			ch = gbuf[vplc>>glogy];
			if (ch != 255) *pp = gtrans[((*pp)<<8)+gpal[ch]];
			pp += bpl;
			vplc += vinc;
		}
	}
}
''', '''/* See-through: every other pixel, as a chessboard. The phone's screen means two of the
 * frame's columns for each of its own, so this shows as half and half; a blend read the
 * frame and a table of 64 KB for each pixel (os/ports/duke/prepare.py) */
extern intptr_t frameplace;

static inline int chess_skip(const unsigned char *pp)
{
	int off = (int)(pp - (const unsigned char *)frameplace);

	return (off + off / bpl) & 1;
}

void tvlineasm1(int vinc, void *paloffs, int cnt, unsigned int vplc, void *bufplc, void *p)
{
	unsigned char ch, *pp;
	int step = bpl << 1, vstep = vinc << 1;

	gbuf = (unsigned char *)bufplc;
	gpal = (unsigned char *)paloffs;
	pp = (unsigned char *)p;
	if (chess_skip(pp))
	{
		pp += bpl;
		vplc += vinc;
		cnt--;
	}
	for(;cnt>=0;cnt-=2)
	{
		ch = gbuf[vplc>>glogy];
		if (ch != 255) *pp = gpal[ch];
		pp += step;
		vplc += vstep;
	}
}
'''),
        ('''	pp = (unsigned char *)p;
	if (transmode)
	{
		for(cntup16>>=16;cntup16>0;cntup16--)
		{
			ch = gbuf[((bx>>(32-glogx))<<glogy)+(by>>(32-glogy))];
			if (ch != 255) *pp = gtrans[(*pp)+(gpal[ch]<<8)];
			bx += asm1;
			by += asm2;
			pp++;
		}
	}
	else
	{
		for(cntup16>>=16;cntup16>0;cntup16--)
		{
			ch = gbuf[((bx>>(32-glogx))<<glogy)+(by>>(32-glogy))];
			if (ch != 255) *pp = gtrans[((*pp)<<8)+gpal[ch]];
			bx += asm1;
			by += asm2;
			pp++;
		}
	}
}
''', '''	pp = (unsigned char *)p;
	cntup16 >>= 16;
	if (chess_skip(pp))
	{
		bx += asm1;
		by += asm2;
		pp++;
		cntup16--;
	}
	for(;cntup16>0;cntup16-=2)
	{
		ch = gbuf[((bx>>(32-glogx))<<glogy)+(by>>(32-glogy))];
		if (ch != 255) *pp = gpal[ch];
		bx += asm1 << 1;
		by += asm2 << 1;
		pp += 2;
	}
}
'''),
        ('''	pp = (unsigned char *)p;
	if (transmode)
	{
		for(;cnt>1;cnt--)
		{
			ch = gbuf[(bx>>16)*glogy+(by>>16)];
			if (ch != 255) *pp = gtrans[(*pp)+(gpal[ch]<<8)];
			bx += gbxinc;
			by += gbyinc;
			pp += bpl;
		}
	}
	else
	{
		for(;cnt>1;cnt--)
		{
			ch = gbuf[(bx>>16)*glogy+(by>>16)];
			if (ch != 255) *pp = gtrans[((*pp)<<8)+gpal[ch]];
			bx += gbxinc;
			by += gbyinc;
			pp += bpl;
		}
	}
}
''', '''	pp = (unsigned char *)p;
	if (chess_skip(pp))
	{
		bx += gbxinc;
		by += gbyinc;
		pp += bpl;
		cnt--;
	}
	for(;cnt>1;cnt-=2)
	{
		ch = gbuf[(bx>>16)*glogy+(by>>16)];
		if (ch != 255) *pp = gpal[ch];
		bx += gbxinc << 1;
		by += gbyinc << 1;
		pp += bpl << 1;
	}
}
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


def copy(src, out):
    for path in src.iterdir():
        if not path.is_file() or path.suffix not in ('.c', '.h'):
            continue
        text = path.read_bytes().decode('latin-1').replace('\r\n', '\n')
        for old, new in CHANGES.get(path.name, []):
            if text.count(old) != 1:
                sys.exit(f'prepare.py: {path.name} is not what it was (looked for {old!r})')
            text = text.replace(old, new)
        dest = out / path.name
        dest.parent.mkdir(parents=True, exist_ok=True)
        if not dest.exists() or dest.read_bytes() != text.encode('latin-1'):
            dest.write_bytes(text.encode('latin-1'))


def main():
    build, game, out = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
    copy(build / 'include', out / 'build/include')
    copy(build / 'src', out / 'build/src')
    copy(game / 'src', out / 'game')
    (out / '.prepared').write_text('ok\n')


if __name__ == '__main__':
    main()
