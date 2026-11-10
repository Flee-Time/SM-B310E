#!/usr/bin/env python3
"""Quake for the phone: id's WinQuake (build/quake, python os/ports/sources.py quake),
the software renderer, copied to the build with these changes (each checked):

 1. It starts with the memory there is: id's minimum was 5.3 MB (a DOS PC's).
 2. A pak's directory is read into the heap, not a 128 KB array on the stack.
 3. A map is read a lump at a time as it is parsed (Mod_Lump), not the whole file
    into the hunk first: that was as much memory again as the level.
 4. Textures are read one at a time from the file (the lump was 531 KB at once), a
    wall's without its two largest mip levels (15/16 of it): the renderer starts at
    the third (d_mipcap 2); at a view of 160 x 100 the second was seldom picked.
    Skies and water keep theirs.
 5. 450 entities, not 600 (each takes 876 bytes, and 180 more on the client's side;
    id's maps keep up to 418). One player's room, not four (a phone hosts no one);
    512 particles, not 2048.
 6. The renderer's edges, surfaces and spans are static, not 176 KB of the stack.
 7. The 3D view is drawn at half the screen's size (160 x 100: the LCD shows no more)
    into its own buffer, and doubled into the screen; the menus, the status bar and
    the text stay at 320 x 200. A quarter of the drawing, of the z-buffer and of the
    surface cache. No underwater warping (it draws to the screen).
 8. Room for 160 models, not 256 (a map's 131 submodels at most, and its precaches);
    1500 spans (the view is half as tall); a 32 KB zone.
 9. A model's file is read through int pointers, and the ARM926 reads an int at an
    odd address from the word below (the phone: a 1.1 GB Hunk_Alloc; QEMU faults on
    the LDM instead). The stack buffers small files go into are word-aligned, and
    after a skin or a sprite frame of an odd size the rest of the file is moved down
    to the word (Mod_Align).

Where it stands (QEMU): it starts, runs its console and loads maps; a game does not
fit yet. E1M1 with the server's progs.dat (403 KB) and entities (394 KB) is about
0.5 MB more than the 2.3 MB hunk, before models and sounds.

  python os/ports/quake/prepare.py build/quake/WinQuake <out>

SPDX-License-Identifier: GPL-2.0-or-later
"""
from pathlib import Path
import sys

CHANGES = {
    'view.c': [
        ('\telse\n\t{\n\t\tR_RenderView ();\n\t}\n',
         '\telse\n\t{\n\t\tR_RenderView ();\n\t\tB310E_ViewToScreen ();\t// (B310E)\n\t}\n'),
    ],
    'd_sky.c': [
        ('\twu = 8192.0 * (float)(u-((int)vid.width>>1)) / temp;\n\twv = 8192.0 * (float)(((int)vid.height>>1)-v) / temp;\n',
         "\twu = 8192.0 * (float)(u-(VIEW_W>>1)) / temp;\t// (B310E) the view's middle\n\twv = 8192.0 * (float)((VIEW_H>>1)-v) / temp;\n"),
    ],
    'd_modech.c': [
        ('\t\trowbytes = vid.rowbytes;\n',
         "\t\trowbytes = VIEW_W;\t// (B310E) the view's, not the screen's\n"),
        ('\td_zrowbytes = vid.width * 2;\n\td_zwidth = vid.width;\n',
         '\td_zrowbytes = VIEW_W * 2;\n\td_zwidth = VIEW_W;\n'),
        ('\t\tfor (i=0 ; i<vid.height; i++)\n',
         '\t\tfor (i=0 ; i<VIEW_H; i++)\n'),
    ],
    'r_misc.c': [
        ('\tr_dowarp = r_waterwarp.value && (r_viewleaf->contents <= CONTENTS_WATER);\n',
         '\tr_dowarp = false;\t// (B310E) no warping: it draws to the screen, not the view\n'),
        ('\t\telse\n\t\t{\n\t\t\tvrect.x = 0;\n\t\t\tvrect.y = 0;\n\t\t\tvrect.width = vid.width;\n\t\t\tvrect.height = vid.height;\n\n\t\t\tR_ViewChanged (&vrect, sb_lines, vid.aspect);\n',
         '\t\telse\n\t\t{\n\t\t\tvrect.x = 0;\n\t\t\tvrect.y = 0;\n\t\t\tvrect.width = VIEW_W;\t// (B310E)\n\t\t\tvrect.height = VIEW_H;\n\n\t\t\tR_ViewChanged (&vrect, sb_lines/2, vid.aspect);\n'),
    ],
    'screen.c': [
        ('// notify the refresh of the change\n\tR_ViewChanged (&vrect, sb_lines, vid.aspect);\n',
         '// notify the refresh of the change\n\tvrect.width = VIEW_W;\t// (B310E) the view at half size\n\tvrect.height = VIEW_H;\n\tR_ViewChanged (&vrect, sb_lines/2, vid.aspect);\n'),
    ],
    'common.c': [
        ('\tdpackfile_t             info[MAX_FILES_IN_PACK];\n',
         '\tdpackfile_t             *info;\t// (B310E) its directory in the heap, not the stack\n'),
        ('\tSys_FileSeek (packhandle, header.dirofs);\n\tSys_FileRead (packhandle, (void *)info, header.dirlen);\n',
         '\tinfo = malloc (header.dirlen);\n\tif (!info)\n\t\tSys_Error ("no memory for %s\'s directory", packfile);\n'
         '\tSys_FileSeek (packhandle, header.dirofs);\n\tSys_FileRead (packhandle, (void *)info, header.dirlen);\n'),
        ('\tpack = Hunk_Alloc (sizeof (pack_t));\n',
         '\tfree (info);\n\tpack = Hunk_Alloc (sizeof (pack_t));\n'),
    ],
    'model.c': [
        ('\tbyte\tstackbuf[1024];\t\t// avoid dirtying the cache heap\n',
         '\tbyte\tstackbuf[1024] __attribute__((aligned(8)));\t// (B310E) (see 9)\n'),
        ('#define\tMAX_MOD_KNOWN\t256\n', '#define\tMAX_MOD_KNOWN\t160\t// (B310E) a map\'s and its precaches: 131 + less than 30\n'),
        ('\t\tloadmodel->textures = NULL;\n\t\treturn;\n\t}\n', '\t\tloadmodel->textures = NULL;\n\t\treturn;\n\t}\n\tif (mod_file)\t// (B310E) a texture at a time from the file, not the lump at once\n\t{\n\t\tint\t\t*ofs;\n\t\tmiptex_t\tmth;\n\n\t\tfseek (mod_file, mod_fileofs + l->fileofs, SEEK_SET);\n\t\tfread (&num, 4, 1, mod_file);\n\t\tnum = LittleLong (num);\n\t\tofs = Hunk_TempAlloc (num * 4);\n\t\tfread (ofs, 4, num, mod_file);\n\t\tloadmodel->numtextures = num;\n\t\tloadmodel->textures = Hunk_AllocName (num * sizeof(*loadmodel->textures) , loadname);\n\t\tfor (i=0 ; i<num ; i++)\n\t\t{\n\t\t\tint\t\to = LittleLong (ofs[i]);\n\n\t\t\tif (o == -1)\n\t\t\t\tcontinue;\n\t\t\tfseek (mod_file, mod_fileofs + l->fileofs + o, SEEK_SET);\n\t\t\tfread (&mth, sizeof(mth), 1, mod_file);\n\t\t\tmth.width = LittleLong (mth.width);\n\t\t\tmth.height = LittleLong (mth.height);\n\t\t\tfor (j=0 ; j<MIPLEVELS ; j++)\n\t\t\t\tmth.offsets[j] = LittleLong (mth.offsets[j]);\n\t\t\tif ( (mth.width & 15) || (mth.height & 15) )\n\t\t\t\tSys_Error ("Texture %s is not 16 aligned", mth.name);\n\t\t\tpixels = mth.width*mth.height/64*85;\n\t\t\tskip = (mth.name[0] != \'*\' && Q_strncmp (mth.name, "sky", 3)) ? mth.width*mth.height/16*20 : 0;\n\t\t\ttx = Hunk_AllocName (sizeof(texture_t) +pixels - skip, loadname );\n\t\t\tloadmodel->textures[i] = tx;\n\t\t\tmemcpy (tx->name, mth.name, sizeof(tx->name));\n\t\t\ttx->width = mth.width;\n\t\t\ttx->height = mth.height;\n\t\t\tfor (j=0 ; j<MIPLEVELS ; j++)\n\t\t\t\ttx->offsets[j] = mth.offsets[j] + sizeof(texture_t) - sizeof(miptex_t) - skip;\n\t\t\tif (skip)\n\t\t\t\ttx->offsets[0] = tx->offsets[1] = tx->offsets[2];\n\t\t\tfseek (mod_file, mod_fileofs + l->fileofs + o + mth.offsets[0] + skip, SEEK_SET);\n\t\t\tfread (tx+1, 1, pixels - skip, mod_file);\n\t\t\tif (!Q_strncmp(mth.name,"sky",3))\n\t\t\t\tR_InitSky (tx);\n\t\t}\n\t\tgoto sequence;\n\t}\n'),
        ('//\n// sequence the animations\n//\n\tfor (i=0 ; i<m->nummiptex ; i++)\n',
         'sequence:\n//\n// sequence the animations\n//\n\tfor (i=0 ; i<loadmodel->numtextures ; i++)\n'),
        ('\t\tfor (j=i+1 ; j<m->nummiptex ; j++)\n', '\t\tfor (j=i+1 ; j<loadmodel->numtextures ; j++)\n'),
        ('\t\tpixels = mt->width*mt->height/64*85;\n\t\ttx = Hunk_AllocName (sizeof(texture_t) +pixels, loadname );\n',
         '\t\tpixels = mt->width*mt->height/64*85;\n'
         '\t\t// (B310E) a wall\'s largest mip level stays in the file: the renderer starts at\n'
         '\t\t// the third (d_mipcap 2). Skies and water read theirs, and keep it\n'
         '\t\tskip = (mt->name[0] != \'*\' && Q_strncmp (mt->name, "sky", 3)) ? mt->width*mt->height/16*20 : 0;\n'
         '\t\ttx = Hunk_AllocName (sizeof(texture_t) +pixels - skip, loadname );\n'),
        ('\t\t\ttx->offsets[j] = mt->offsets[j] + sizeof(texture_t) - sizeof(miptex_t);\n\t\t// the pixels immediately follow the structures\n\t\tmemcpy ( tx+1, mt+1, pixels);\n',
         '\t\t\ttx->offsets[j] = mt->offsets[j] + sizeof(texture_t) - sizeof(miptex_t) - skip;\n'
         '\t\tif (skip)\n\t\t\ttx->offsets[0] = tx->offsets[1] = tx->offsets[2];\n'
         '\t\t// the pixels immediately follow the structures\n\t\tmemcpy ( tx+1, (byte *)(mt+1) + skip, pixels - skip);\n'),
        ('\tint\t\ti, j, pixels, num, max, altmax;\n', '\tint\t\ti, j, pixels, num, max, altmax, skip;\n'),
        ('model_t *Mod_LoadModel (model_t *mod, qboolean crash);\n',
         'model_t *Mod_LoadModel (model_t *mod, qboolean crash);\n'
         'static FILE\t*mod_file;\t\t// (B310E) the map being read (Mod_Lump)\n'
         'static int\tmod_fileofs;\nstatic int\tmod_lumpmark = -1;\n'),
        ('byte\t*mod_base;\n', 'byte\t*mod_base;\n\n// (B310E) a map is read a lump at a time, each into the top of the hunk until it is\n// parsed: the whole file there first (id\'s way) was as much memory again as the level\nstatic byte *Mod_Lump (lump_t *l)\n{\n\tbyte\t*data;\n\n\tif (!mod_file)\n\t\treturn mod_base;\n\tif (mod_lumpmark >= 0)\n\t\tHunk_FreeToHighMark (mod_lumpmark);\n\tmod_lumpmark = Hunk_HighMark ();\n\tdata = Hunk_HighAllocName (l->filelen + 4, "lump");\n\tfseek (mod_file, mod_fileofs + l->fileofs, SEEK_SET);\n\tif (fread (data, 1, l->filelen, mod_file) != l->filelen)\n\t\tSys_Error ("%s: a lump is short", loadmodel->name);\n\treturn data - l->fileofs;\t// where mod_base would be\n}\n'),
        ('mod_base + l->fileofs', '(Mod_Lump (l) + l->fileofs)', 15),
        ('\tbuf = (unsigned *)COM_LoadStackFile (mod->name, stackbuf, sizeof(stackbuf));\n', '\t{\t// (B310E) a map: its header now, its lumps as they are parsed (Mod_Lump)\n\t\tstatic dheader_t\theader;\n\t\tFILE\t*f;\n\t\tint\t\tlen = COM_FOpenFile (mod->name, &f);\n\n\t\tif (len >= (int)sizeof(header))\n\t\t{\n\t\t\tint\t\tbase = ftell (f);\n\n\t\t\tfread (&header, 1, sizeof(header), f);\n\t\t\tif (LittleLong (header.version) == BSPVERSION)\n\t\t\t{\n\t\t\t\tCOM_FileBase (mod->name, loadname);\n\t\t\t\tloadmodel = mod;\n\t\t\t\tmod->needload = NL_PRESENT;\n\t\t\t\tmod_file = f;\n\t\t\t\tmod_fileofs = base;\n\t\t\t\tMod_LoadBrushModel (mod, &header);\n\t\t\t\tif (mod_lumpmark >= 0)\n\t\t\t\t\tHunk_FreeToHighMark (mod_lumpmark);\n\t\t\t\tmod_lumpmark = -1;\n\t\t\t\tmod_file = NULL;\n\t\t\t\tfclose (f);\n\t\t\t\treturn mod;\n\t\t\t}\n\t\t}\n\t\tif (len >= 0)\n\t\t\tfclose (f);\n\t}\n\tbuf = (unsigned *)COM_LoadStackFile (mod->name, stackbuf, sizeof(stackbuf));\n'),
        ('\tbuf = (unsigned *)COM_LoadStackFile (mod->name, stackbuf, sizeof(stackbuf));\n',
         '\tbuf = (unsigned *)COM_LoadStackFile (mod->name, stackbuf, sizeof(stackbuf));\n'
         '\tmod_fileend = buf ? (byte *)buf + com_filesize : NULL;\t// (B310E) Mod_Align\n'),
        ('void * Mod_LoadAliasSkin (void * pin, int *pskinindex, int skinsize,\n',
         '// (B310E) A skin or a sprite frame of an odd size leaves what follows it off the\n'
         '// word: the rest of the file goes down to the word below (what it moves over is\n'
         '// read already), as the ARM926 reads no int from an odd address (see 9)\n'
         'static void *Mod_Align (void *p)\n'
         '{\n'
         '\tbyte\t*from = p, *to = (byte *)((unsigned long)p & ~3ul);\n'
         '\n'
         '\tif (from == to || !mod_fileend || from > mod_fileend)\n'
         '\t\treturn p;\n'
         '\tmemmove (to, from, mod_fileend - from);\n'
         '\tmod_fileend -= from - to;\n'
         '\treturn to;\n'
         '}\n'
         '\n'
         'void * Mod_LoadAliasSkin (void * pin, int *pskinindex, int skinsize,\n'),
        ('\treturn ((void *)pinskin);\n', '\treturn Mod_Align (pinskin);\t// (B310E)\n'),
        ('model_t\t*loadmodel;\n', 'model_t\t*loadmodel;\nstatic byte\t*mod_fileend;\t// (B310E) the end of a model file (Mod_Align)\n'),
        ('\treturn (void *)((byte *)pinframe + sizeof (dspriteframe_t) + size);\n',
         '\treturn Mod_Align ((byte *)pinframe + sizeof (dspriteframe_t) + size);\t// (B310E)\n'),
    ],
    'd_init.c': [
        ('\t\td_viewbuffer = (void *)(byte *)vid.buffer;\n',
         '\t\td_viewbuffer = (void *)b310e_view;\t// (B310E)\n'),
        ('\t\tscreenwidth = vid.rowbytes;\n',
         '\t\tscreenwidth = VIEW_W;\n'),
        ('cvar_t\td_mipcap = {"d_mipcap", "0"};', 'cvar_t\td_mipcap = {"d_mipcap", "2"};\t// (B310E) model.c'),
    ],
    'host.c': [
        ('\tif (svs.maxclientslimit < 4)\n\t\tsvs.maxclientslimit = 4;\n',
         '\t// (B310E) one player: room for four was 115 KB (their sockets and slots)\n'),
    ],
    'r_part.c': [
        ('#define MAX_PARTICLES\t\t\t2048', '#define MAX_PARTICLES\t\t\t512\t// (B310E) 2048 were 90 KB'),
    ],
    'r_main.c': [
        ('\tedge_t\tledges[NUMSTACKEDGES +\n', '\tstatic edge_t\tledges[NUMSTACKEDGES +\t// (B310E) static: not 128 KB of stack\n'),
        ('\tsurf_t\tlsurfs[NUMSTACKSURFACES +\n', '\tstatic surf_t\tlsurfs[NUMSTACKSURFACES +\n'),
    ],
    'r_edge.c': [
        ('\tbyte\tbasespans[MAXSPANS*sizeof(espan_t)+CACHE_SIZE];\n',
         '\tstatic byte\tbasespans[MAXSPANS*sizeof(espan_t)+CACHE_SIZE];\t// (B310E) not 48 KB of stack\n'),
    ],
    'r_shared.h': [
        ('#define\tMAXSPANS\t\t\t3000\n', '#define\tMAXSPANS\t\t\t1500\t// (B310E) the view is half as tall\n'),
    ],
    'snd_mem.c': [
        ('\tbyte\tstackbuf[1*1024];\t\t// avoid dirtying the cache heap\n',
         '\tbyte\tstackbuf[1*1024] __attribute__((aligned(8)));\t// (B310E) (see 9)\n'),
    ],
    'zone.c': [
        ('#define\tDYNAMIC_SIZE\t0xc000\n', '#define\tDYNAMIC_SIZE\t0x8000\t// (B310E)\n'),
    ],
    'quakedef.h': [
        ('#define	MAX_EDICTS		600			// FIXME: ouch! ouch! ouch!',
         '#define	MAX_EDICTS		450			// (B310E) ids maps keep under it'),
        ('#define\tMAX_EDICTS\t\t450',
         "#define\tVIEW_W\t\t\t160\t\t// (B310E) the 3D view: half the screen each way,\n#define\tVIEW_H\t\t\t100\t\t// doubled into it (quake_sys.c); the 2D is the screen's\nextern unsigned char\tb310e_view[VIEW_W*VIEW_H];\nvoid B310E_ViewToScreen (void);\n#define\tMAX_EDICTS\t\t450"),
        ('#define	MINIMUM_MEMORY			0x550000',
         '#define	MINIMUM_MEMORY			0x100000	// (B310E) the phone\'s'),
    ],
}


def main():
    src, out = Path(sys.argv[1]), Path(sys.argv[2])
    stamp = out / '.prepared'
    if stamp.exists():
        stamp.unlink()
    out.mkdir(parents=True, exist_ok=True)
    for path in sorted(src.iterdir()):
        if not path.is_file() or path.suffix not in ('.c', '.h', '.q1', '.q2'):
            continue
        text = path.read_bytes().decode('latin-1').replace('\r\n', '\n')
        for old, new, *times in CHANGES.get(path.name, []):
            if text.count(old) != (times[0] if times else 1):
                sys.exit(f'prepare.py: {path.name} is not what it was (looked for {old!r})')
            text = text.replace(old, new)
        data = text.encode('latin-1')
        dest = out / path.name
        if not dest.exists() or dest.read_bytes() != data:
            dest.write_bytes(data)
    stamp.write_text('ok\n')


if __name__ == '__main__':
    main()
