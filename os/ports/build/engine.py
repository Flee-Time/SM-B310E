#!/usr/bin/env python3
"""The Build engine (jfbuild) for a game of os/ports on the phone: upstream's files
copied to the build with these changes, each checked. Used by each game's prepare.py
(os/ports/duke, os/ports/sw), which adds its own.

 1. Maps: the walls, sprites and tiles a game needs (its prepare.py says how many),
    1024 sectors. A map beyond them is refused by the engine as it loads.
 2. The cache's objects: 2048 (it holds a few hundred). Its error says what it is in a
    line (the list of all its objects flooded the card's log, and the error was lost),
    and the game ends with an error.
 3. The screen is 320 x 200: tables sized by it shrink (one of them from 737 KB), and
    so do those of the walls in view (2048, as fpdoom's: about 100 KB less).
    320 rows, not 200: the tilted view (dying) draws to a tile of 320 x 320.
 4. No voxels: their tables go, and distrecip (256 KB, voxels only).
 5. Two players' room (no network here), or the few a game's own tables assume:
    their input queues shrink.
 6. Most of the console's text goes.
 7. See-through effects as a chessboard, not a blend for each pixel: less than half the
    work where it hurts most, and the screen's own halving of the columns blends them.
 8. Walls and sprites four columns at a time (upstream's way with x86 assembly, off in its
    C code), floors the same: a word to the frame for each four pixels. The PSRAM takes
    each write by itself: the D-cache does not fetch a line to write to it. The two
    loops that draw most of the screen are in ARM code (os/ports/build/draw4.S, checked
    pixel for pixel against the C); a compiler keeps a third more on the stack.

 9. krecip, a reciprocal through a float's exponent and mantissa, makes the float's
    bits with integers (CLZ and the conversion's rounding): an int to a float is a
    library call without an FPU (in Shadow Warrior's sloped floors, 10% of the time).

10. Sloped floors and ceilings (slopevlin) take 1/z every 8 pixels and step u and v
    straight between, as Ken's assembly did (BITSOFPRECISION 3); the C took it for
    each pixel.

SPDX-License-Identifier: GPL-2.0-or-later
"""
from pathlib import Path
import sys


def changes(tiles, walls=6144, sprites=2048, players=2, voxels=8):
    """The engine's changes, as {file: [(old, new)]}. voxels: one more than the game's
    highest voxel number (voxoff[] is indexed by it, unchecked)"""
    return {
    'build.h': [
        ('#define MAXSECTORS MAXSECTORSV8', '#define MAXSECTORS MAXSECTORSV7'),
        ('#define MAXWALLS MAXWALLSV8', f'#define MAXWALLS {walls}'),
        ('#define MAXSPRITES MAXSPRITESV8', f'#define MAXSPRITES {sprites}'),
        ('#define MAXTILES 9216', f'#define MAXTILES {tiles}'),
        ('#define MAXVOXELS 4096', f'#define MAXVOXELS {voxels}'),
        ('#define MAXXDIM 2880', '#define MAXXDIM 320'),
        ('#define MAXYDIM 1800', '#define MAXYDIM 320'),
        ('#define MAXSPRITESONSCREEN 2048', '#define MAXSPRITESONSCREEN 512'),
        ('#define MAXPLAYERS 16', f'#define MAXPLAYERS {players}'),
    ],
    'mmulti.h': [
        ('#define MAXMULTIPLAYERS 16', f'#define MAXMULTIPLAYERS {players}'),
    ],
    'engine_priv.h': [
        ('#define MAXPERMS 1024', '#define MAXPERMS 256'),
        ('#define MAXWALLSB 4096', '#define MAXWALLSB 2048'),
    ],
    'engine.c': [
        ('\tfloat f = (float)i; i = *(int *)&f;\n',
         '\t// (B310E) the float\'s bits made with integers (see 9): an int to a float is a\n'
         '\t// library call on this FPU-less CPU (krecip was 10% of SW\'s time with it)\n'
         '\tunsigned a = i < 0 ? -(unsigned)i : (unsigned)i, bits = 0;\n'
         '\n'
         '\tif (a) {\n'
         '\t\tint e = 31 - __builtin_clz(a);\n'
         '\t\tunsigned m;\n'
         '\n'
         '\t\tif (e <= 23)\n'
         '\t\t\tm = a << (23 - e);\n'
         '\t\telse {\t// rounded to nearest, ties to even, as the conversion does\n'
         '\t\t\tint sh = e - 23;\n'
         '\t\t\tunsigned rem = a & ((1u << sh) - 1), half = 1u << (sh - 1);\n'
         '\n'
         '\t\t\tm = a >> sh;\n'
         '\t\t\tif (rem > half || (rem == half && (m & 1)))\n'
         '\t\t\t\tif (++m >> 24) {\n'
         '\t\t\t\t\tm >>= 1;\n'
         '\t\t\t\t\te++;\n'
         '\t\t\t\t}\n'
         '\t\t}\n'
         '\t\tbits = (i < 0 ? 0x80000000u : 0) | (unsigned)(e + 127) << 23 | (m & 0x7fffff);\n'
         '\t}\n'
         '\ti = (int)bits;\n'),
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
        ('\tif (bestval == SIZE_MAX)\n\t\treportandexit("CACHE SPACE ALL LOCKED UP!");\n',
         '\tif (bestval == SIZE_MAX)\n\t{\n'
         '\t\t/* (B310E) the game may go on without it (a sound): *newhandle stays as it is */\n'
         '\t\textern int build_cache_full(unsigned char *lock, size_t bytes) __attribute__((weak));\n'
         '\t\tif (build_cache_full && build_cache_full(newlockptr, newbytes)) return;\n'
         '\t\treportandexit("CACHE SPACE ALL LOCKED UP!");\n\t}\n'),
        ('#define MAXCACHEOBJECTS 9216', '#define MAXCACHEOBJECTS 2048'),
        ('static void reportandexit(char *errormessage)\n{\n    int i;\n    size_t j;\n\n    j = 0;\n    for(i=0;i<cacnum;i++)\n    {\n        buildprintf("%d- ",i);\n        if (cac[i].hand) {\n            buildprintf("ptr: 0x%p, ",*cac[i].hand);\n        } else {\n            buildprintf("ptr: NULL, ");\n        }\n        buildprintf("leng: %zu, ",cac[i].leng);\n        if (cac[i].lock) {\n            buildprintf("lock: %d\\n",*cac[i].lock);\n        } else {\n            buildprintf("lock: NULL\\n");\n        }\n        j += cac[i].leng;\n    }\n\tbuildprintf("Cachesize = %zu\\n",cachesize);\n\tbuildprintf("Cacnum = %d\\n",cacnum);\n\tbuildprintf("Cache length sum = %zu\\n",j);\n\tbuildprintf("ERROR: %s\\n",errormessage);\n\texit(0);\n}\n',
         'void build_cache_name(char *lock, char *out, int n) __attribute__((weak));  /* (B310E) */\nstatic void reportandexit(char *errormessage)\n{\n    int i;\n    size_t j = 0, locked = 0, largest = 0;\n\n    for(i=0;i<cacnum;i++)\n    {\n        j += cac[i].leng;\n        if (cac[i].lock && *cac[i].lock >= 200) locked += cac[i].leng;\n        if (cac[i].leng > largest) largest = cac[i].leng;\n    }\n\tbuildprintf("ERROR: %s (the cache: %u KB, %u KB of it locked, %d objects, the largest %u KB)\\n",\n\t\terrormessage, (unsigned)(cachesize >> 10), (unsigned)(locked >> 10), cacnum, (unsigned)(largest >> 10));\n\tif (build_cache_name) {  /* (B310E) what is locked: the game names it */\n\t\tchar name[24];\n\t\tfor (i = 0; i < cacnum; i++)\n\t\t\tif (cac[i].lock && *cac[i].lock >= 200 && cac[i].leng >= 1024) {\n\t\t\t\tbuild_cache_name(cac[i].lock, name, sizeof(name));\n\t\t\t\tbuildprintf("locked: %s, %u KB, lock %d\\n", name, (unsigned)(cac[i].leng >> 10), *cac[i].lock);\n\t\t\t}\n\t}\n\texit(1);\n}\n'),
        ('\tbuildprintf("initcache(): Initialised with %zu bytes\\n", cachesize);\n',
         '\tbuildprintf("initcache(): Initialised with %u KB\\n", (unsigned)(cachesize >> 10));\n'),
    ],
    'osd.c': [
        ('#define TEXTSIZE 16384', '#define TEXTSIZE 4096'),
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
        ('\tfor(;cnt>0;cnt--)\n\t{\n\t\ti = krecip(bz>>6); bz += bzinc;\n'
         '\t\tu = bx+globalx3*i;\n\t\tv = by+globaly3*i;\n'
         '\t\t*pp = *(unsigned char *)(slopalptr[0]+gbuf[((u>>(32-glogx))<<glogy)+(v>>(32-glogy))]);\n'
         '\t\tslopalptr--;\n\t\tpp += gpinc;\n\t}\n',
         '\t// (B310E) 1/z every 8 pixels and u, v straight between, as Ken\'s own assembly\n'
         '\t// (BITSOFPRECISION) did: a reciprocal for each pixel was this loop\'s most (see 10)\n'
         '\tfor(;cnt>0;)\n'
         '\t{\n'
         '\t\tint n = cnt < 8 ? cnt : 8, k, du, dv;\n'
         '\t\tunsigned int u1, v1;\n'
         '\n'
         '\t\ti = krecip(bz>>6);\n'
         '\t\tu = bx+globalx3*i;\n'
         '\t\tv = by+globaly3*i;\n'
         '\t\tbz += bzinc*n;\n'
         '\t\ti = krecip(bz>>6);\n'
         '\t\tu1 = bx+globalx3*i;\n'
         '\t\tv1 = by+globaly3*i;\n'
         '\t\tdu = n == 8 ? (int)(u1-u)>>3 : (int)(u1-u)/n;\n'
         '\t\tdv = n == 8 ? (int)(v1-v)>>3 : (int)(v1-v)/n;\n'
         '\t\tfor(k=n;k>0;k--)\n'
         '\t\t{\n'
         '\t\t\t*pp = *(unsigned char *)(slopalptr[0]+gbuf[((u>>(32-glogx))<<glogy)+(v>>(32-glogy))]);\n'
         '\t\t\tslopalptr--;\n'
         '\t\t\tpp += gpinc;\n'
         '\t\t\tu += du;\n'
         '\t\t\tv += dv;\n'
         '\t\t}\n'
         '\t\tcnt -= n;\n'
         '\t}\n'),
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
    }


def copy(src, out, changes):
    for path in src.iterdir():
        if not path.is_file() or path.suffix not in ('.c', '.h'):
            continue
        text = path.read_bytes().decode('latin-1').replace('\r\n', '\n')
        for old, new in changes.get(path.name, []):
            if text.count(old) != 1:
                sys.exit(f'prepare.py: {path.name} is not what it was (looked for {old!r})')
            text = text.replace(old, new)
        dest = out / path.name
        dest.parent.mkdir(parents=True, exist_ok=True)
        if not dest.exists() or dest.read_bytes() != text.encode('latin-1'):
            dest.write_bytes(text.encode('latin-1'))


def prepare(build, game, out, game_changes, tiles, walls=6144, sprites=2048, players=2):
    """Copy the engine and the game's source to out with the changes"""
    engine = changes(tiles, walls, sprites, players)
    both = dict(engine)
    for name, pairs in game_changes.items():
        both[name] = both.get(name, []) + pairs
    copy(build / 'include', out / 'build/include', both)
    copy(build / 'src', out / 'build/src', both)
    copy(game, out / 'game', both)
    (out / '.prepared').write_text('ok\n')
