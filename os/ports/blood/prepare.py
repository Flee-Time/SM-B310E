#!/usr/bin/env python3
"""Blood for the phone: NBlood's game code (build/nblood, an upstream commit) on the
shared Build engine (jfbuild, os/ports/build/engine.py), the way fpdoom does it. Copied
to the build with:

 1. fpdoom's adaptation of NBlood to jfbuild (build/fpdoom: fpbuild/NBlood.patch and its
    eduke32/ headers, public domain, unchanged), as fpdoom applies it.
 2. The parts of fpdoom's jfbuild patch Blood needs (the hunks ENGINE_HUNKS lists):
    the C++ names of sprites' fields, Blood's own palette, sprite lists, palookups,
    animations, map loader and art files, and the 1996 engine's clipmove.
 3. The shared engine's own changes (engine.py), sized for Blood v1.21 and Cryptic
    Passage: 4864 tiles (TILES017.ART ends at 4607), 8192 walls (CP09 has 8002), 64
    voxels (VOXEL.DAT names 0-63; the 8 the other games have left Blood's past the end).
 4. Ours (CHANGES), for its sound on the kit and for a cache of about 900 KB, where
    fpdoom's leaves the sound out:
    - its sounds: the shared audiolib (os/ports/build/sound.c) under EDuke32's names
      (blood_audio.h); the kit's synth plays the songs, no OPL timbres;
    - the weapons' animations are locked in the cache while used, not all 125 from the
      start (221 KB); an ambient loop is locked and played while it is heard, not all
      of them for the whole level; a level's start does not load every sound (26 MB).
    - NBlood's locks that were never undone: a one-shot 3D sound locked its sample
      twice (sfx.cpp), a 2D one its SFX header (sound.cpp). Each sound played stayed
      in the cache for good, and after a minute or so of play the game stopped with
      "CACHE SPACE ALL LOCKED UP!" (on the phone; the PC's cache had room for them).
    - a voxel is one block in the cache that may go (fpdoom's qloadvoxel, lock 199);
      going, only its first pointer is cleared, and its smaller mips' pointers were
      drawn from: a fault. Such a voxel is left out of that one frame.
    - a sound the cache has no room for is not played (build_cache_full, blood_game.cpp:
      Resource::Lock gives NULL and each player of sounds looks); anything else that
      does not fit still stops the game, with what is locked in the log.

  python os/ports/blood/prepare.py build/jfbuild build/nblood/source/blood <out>

SPDX-License-Identifier: GPL-2.0-or-later
"""
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'build'))
import engine                                   # noqa: E402
import unidiff                                  # noqa: E402

# fpdoom's jfbuild.patch: which hunks (by number, as unidiff.read numbers them)
ENGINE_HUNKS = {
    'include/build.h': [1, 2, 3, 7],            # the fields' C++ names; picanm is C++'s own
    'include/compat.h': [2],                    # read32_le and the like
    'src/engine.c': [8, 9, 10, 28, 29, 34, 36, 41, 42, 44, 47, 48, 49],
}

# ours, after fpdoom's and the engine's: {path in the tree: [(old, new)]}
CHANGES = {
    'build/src/engine.c': [                     # a voxel the cache has let go: not this frame
        ('\tif (novoxmips) i = 0;\n\tdavoxptr = (unsigned char *)voxoff[daindex][i];\n',
         '\tif (novoxmips) i = 0;\n'
         '\tif (!voxoff[daindex][0]) return;  /* (B310E) its block went from the cache (lock 199,\n'
         '\t                                   * tile.cpp): [1..] still point into it. Blood loads\n'
         '\t                                   * it again for the next frame (view.cpp) */\n'
         '\tdavoxptr = (unsigned char *)voxoff[daindex][i];\n'),
        ('\t\tlongptr = (int *)voxoff[vtilenum][0];\n',
         '\t\tlongptr = (int *)voxoff[vtilenum][0];\n'
         '\t\tif (!longptr) return;  /* (B310E) the same: gone from the cache this frame */\n'),
    ],
    'game/asound.cpp': [                        # an ambient loop locked while it is heard
        ('        if (pChannel->at0 > 0)\n            FX_SetPan(pChannel->at0, pChannel->at4, pChannel->at4, pChannel->at4);\n        else\n        {\n',
         '#ifdef EMBEDDED\n        if (pChannel->at4 <= 0)         /* (B310E) out of hearing: its voice and memory go */\n        {\n            if (pChannel->at0 > 0)\n            {\n                FX_EndLooping(pChannel->at0);\n                FX_StopSound(pChannel->at0);\n                pChannel->at0 = 0;\n            }\n            if (pChannel->at10)\n            {\n                gSoundRes.Unlock(pChannel->atc);\n                pChannel->at10 = NULL;\n            }\n        }\n        else\n#endif\n        if (pChannel->at0 > 0)\n            FX_SetPan(pChannel->at0, pChannel->at4, pChannel->at4, pChannel->at4);\n        else\n        {\n#ifdef EMBEDDED\n            if (!pChannel->at10)\n                pChannel->at10 = (char*)gSoundRes.Lock(pChannel->atc);\n#endif\n'),
        ('        if (pChannel->atc)\n        {\n            gSoundRes.Unlock(pChannel->atc);\n',
         '        if (pChannel->atc && pChannel->at10)\n        {\n            gSoundRes.Unlock(pChannel->atc);\n'),
        ('                pChannel->at10 = (char*)gSoundRes.Lock(pRAWNode);\n',
         '#ifdef EMBEDDED\n                pChannel->at10 = NULL;          /* (B310E) locked when heard */\n#else\n'
         '                pChannel->at10 = (char*)gSoundRes.Lock(pRAWNode);\n#endif\n'),
        ('                pChannel->at10 = (char*)gSoundRes.Lock(pChannel->atc);\n#endif\n'
         '            int end = ClipLow(pChannel->at14-1, 0);\n'
         '            pChannel->at0 = FX_PlayLoopedRaw(',
         '                pChannel->at10 = (char*)gSoundRes.Lock(pChannel->atc);\n#endif\n'
         '            int end = ClipLow(pChannel->at14-1, 0);\n'
         '            if (pChannel->at10)          /* (B310E) no room in the cache: not this time */\n'
         '            pChannel->at0 = FX_PlayLoopedRaw('),
    ],
    'game/blood.cpp': [
        ('    gSysRes.PrecacheSounds();\n    gSoundRes.PrecacheSounds();\n',
         '#ifndef EMBEDDED                        /* (B310E) all 26 MB of them: as they play */\n'
         '    gSysRes.PrecacheSounds();\n    gSoundRes.PrecacheSounds();\n#endif\n'),
        ('# ifndef GEKKO\n#  include <sys/ioctl.h>',
         '# if !defined GEKKO && !defined EMBEDDED  /* (B310E) */\n#  include <sys/ioctl.h>'),
    ],
    'game/menu.cpp': [                          # the kit's synth plays the songs, whatever is set
        ('int nMusicDeviceValues[] = {\n',
         '#ifdef EMBEDDED                         /* (B310E) jfaudiolib has neither */\n'
         'enum { ASS_OPL3 = ASS_NumSoundCards, ASS_SF2 };\n'
         'static char SF2_BankFile[BMAX_PATH];\n'
         '#endif\n'
         'int nMusicDeviceValues[] = {\n'),
    ],
    'game/sfx.cpp': [                           # one Lock a sound, as it has one Unlock
        ('    char *pData = (char*)gSoundRes.Lock(hRes);\n    Calc3DValues(pBonkle);\n',
         '    char *pData = (char*)gSoundRes.Lock(hRes);\n    if (!pData) { pBonkle->hSnd = NULL; pBonkle->lChan = pBonkle->rChan = 0; return; }  /* (B310E) no room */\n    Calc3DValues(pBonkle);\n'),
        ('    char *pData = (char*)gSoundRes.Lock(hRes);\n    if (loopStart >= 0)\n',
         '    char *pData = (char*)gSoundRes.Lock(hRes);\n    if (!pData) { pBonkle->hSnd = NULL; pBonkle->lChan = pBonkle->rChan = 0; return; }  /* (B310E) no room */\n    if (loopStart >= 0)\n'),
        ('    char* pData = (char*)gSoundRes.Lock(hRes);\n    if (loopStart >= 0)\n',
         '    char* pData = (char*)gSoundRes.Lock(hRes);\n    if (!pData) { pBonkle->hSnd = NULL; pBonkle->lChan = pBonkle->rChan = 0; return; }  /* (B310E) no room */\n    if (loopStart >= 0)\n'),
        ('        pData = (char*)gSoundRes.Lock(pBonkle->hSnd);\n        if (gStereo)\n        {\n'
         '            pBonkle->lChan = FX_PlayRaw(pData + lPhase, size - lPhase, lPitch, 0, lVol, lVol, 0, priority, fix16_one, (intptr_t)&pBonkle->lChan);\n',
         '        /* (B310E) locked above: a second Lock here was never unlocked */\n        if (gStereo)\n        {\n'
         '            pBonkle->lChan = FX_PlayRaw(pData + lPhase, size - lPhase, lPitch, 0, lVol, lVol, 0, priority, fix16_one, (intptr_t)&pBonkle->lChan);\n'),
        ('        pData = (char*)gSoundRes.Lock(pBonkle->hSnd);\n        if (gStereo)\n        {\n'
         '            pBonkle->lChan = FX_PlayRaw(pData + lPhase, size - lPhase, lPitch, 0, lVol, lVol, 0, priority, fix16_one, (intptr_t)& pBonkle->lChan);\n',
         '        /* (B310E) locked above: a second Lock here was never unlocked */\n        if (gStereo)\n        {\n'
         '            pBonkle->lChan = FX_PlayRaw(pData + lPhase, size - lPhase, lPitch, 0, lVol, lVol, 0, priority, fix16_one, (intptr_t)& pBonkle->lChan);\n'),
    ],
    'game/resource.cpp': [                      # a Lock with no room for it (a sound): NULL
        ('    h->oldLock = 210;\n    if (!h->ptr) {\n'
         '//if (h->size > 4000) printf("Lock(%p, %s.%s, %d)\\n", &h->ptr, h->name, h->type, h->size);\n'
         '        allocache(&h->ptr, h->size, &h->oldLock);\n'
         '//printf("allocache h=%p ptr=%p\\n", h, h->ptr);\n'
         '        Read(h);\n',
         '    h->oldLock = 210;\n    if (!h->ptr) {\n'
         '        allocache(&h->ptr, h->size, &h->oldLock);\n'
         '        if (!h->ptr)                    /* (B310E) the cache let it go without it */\n'
         '        {\n'
         '            h->oldLock = 0;\n'
         '            return NULL;\n'
         '        }\n'
         '        Read(h);\n'),
    ],
    'game/sound.cpp': [                         # no AdLib (OPL) synth here: no timbres for it
        ('    char *pData = (char*)gSoundRes.Lock(pChannel->at5);\n    pChannel->hVoice = FX_PlayRaw(pData, nSize, sndGetRate(1)',
         '    char *pData = (char*)gSoundRes.Lock(pChannel->at5);\n    if (!pData) { pChannel->at5 = 0; return; }  /* (B310E) no room in the cache: not played */\n    pChannel->hVoice = FX_PlayRaw(pData, nSize, sndGetRate(1)'),
        ('    char *pData = (char*)gSoundRes.Lock(pChannel->at5);\n    if (nChannel < 0)\n',
         '    char *pData = (char*)gSoundRes.Lock(pChannel->at5);\n    if (!pData) { pChannel->at5 = 0; return; }  /* (B310E) no room in the cache: not played */\n    if (nChannel < 0)\n'),
        ('    char *pData = (char*)gSoundRes.Lock(pChannel->at5);\n    pChannel->hVoice = FX_Play(',
         '    char *pData = (char*)gSoundRes.Lock(pChannel->at5);\n    if (!pData) { pChannel->at5 = 0; return; }  /* (B310E) no room in the cache: not played */\n    pChannel->hVoice = FX_Play('),
        ('#include "al_midi.h"\n', '#ifndef EMBEDDED\n#include "al_midi.h"\n#endif\n'),
        ('        AL_RegisterTimbreBank(timbre);\n',
         '#ifndef EMBEDDED\n        AL_RegisterTimbreBank(timbre);\n#endif\n'),
        ('    SFX *pEffect = (SFX*)gSoundRes.Lock(hSfx);\n    dassert(pEffect != NULL);\n',
         '    SFX effect_ = *(SFX*)gSoundRes.Load(hSfx);  /* (B310E) a copy: its header was locked\n'
         '                                                 * and never unlocked */\n'
         '    SFX *pEffect = &effect_;\n'),
    ],
    'game/weapon.cpp': [                        # weapons' animations in the cache as used
        ('QAV *weaponQAV[kQAVEnd];\n', "/* (B310E) a weapon's animation is in the cache while it is used, not all of them locked\n * from the start (221 KB of the cache): locked when it starts or is drawn, let go when\n * it has not been for two seconds */\nstatic DICTNODE *weaponQAVNode[kQAVEnd];\nstatic QAV *weaponQAVLocked[kQAVEnd];\nstatic int weaponQAVUsed[kQAVEnd];\n\nstatic QAV *weaponQAVGet(int n)\n{\n    if (!weaponQAVLocked[n])\n    {\n        weaponQAVLocked[n] = (QAV*)gSysRes.Lock(weaponQAVNode[n]);\n        weaponQAVLocked[n]->nSprite = -1;\n    }\n    weaponQAVUsed[n] = (int)totalclock;\n    return weaponQAVLocked[n];\n}\n\nstatic void weaponQAVRelease(void)\n{\n    for (int i = 0; i < kQAVEnd; i++)\n        if (weaponQAVLocked[i] && (unsigned)((int)totalclock - weaponQAVUsed[i]) > 240)\n        {\n            gSysRes.Unlock(weaponQAVNode[i]);\n            weaponQAVLocked[i] = NULL;\n        }\n}\n\nstatic struct { QAV *operator[](int n) const { return weaponQAVGet(n); } } weaponQAV;\n"),
        ('        weaponQAV[i] = (QAV*)gSysRes.Lock(hRes);\n        weaponQAV[i]->nSprite = -1;\n',
         '        weaponQAVNode[i] = hRes;\n'),
        ('        if (weaponQAV[i])\n            weaponQAV[i]->Precache();\n',
         '        if (weaponQAVLocked[i])          /* (B310E) only those in use */\n'
         '            weaponQAVLocked[i]->Precache();\n'),
        ('    dassert(nWeaponQAV < kQAVEnd);\n    pPlayer->weaponQav = nWeaponQAV;\n',
         '    dassert(nWeaponQAV < kQAVEnd);\n    weaponQAVRelease();\n    pPlayer->weaponQav = nWeaponQAV;\n'),
    ],
    'game/weapon.h': [
        ('extern QAV* weaponQAV[];\n', '// extern QAV* weaponQAV[];  (B310E: weapon.cpp\'s own)\n'),
    ],
    'game/compat.h': [
        ('#include "../jfbuild/include/compat.h"', '#include "../build/include/compat.h"'),
        ('#define FX_PlayRaw(...) (-1)\n#define FX_PlayLoopedRaw(...) (-1)\n',
         '#include "blood_audio.h"                /* (B310E) its sounds, on the kit\'s mixer */\n'),
    ],
}


def put(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    data = text.encode('latin-1')
    if not path.exists() or path.read_bytes() != data:
        path.write_bytes(data)


def change(rel, text):
    for old, new in CHANGES.get(rel, []):
        if text.count(old) != 1:
            sys.exit(f'prepare.py: {rel} is not what it was (looked for {old!r})')
        text = text.replace(old, new)
    return text


def main():
    jfbuild, game, out = (Path(a) for a in sys.argv[1:4])
    fpdoom = jfbuild.parent / 'fpdoom' / 'fpbuild'
    stamp = out / '.prepared'
    if stamp.exists():
        stamp.unlink()

    # the engine: fpdoom's hunks, then engine.py's changes and ours, by file name
    patch = unidiff.read((fpdoom / 'jfbuild.patch').read_bytes().decode('latin-1'))
    ours = engine.changes(tiles=4864, walls=8192, sprites=2048, players=2, voxels=64)
    for sub in ('include', 'src'):
        for path in sorted((jfbuild / sub).iterdir()):
            if not path.is_file() or path.suffix not in ('.c', '.h'):
                continue
            rel = f'{sub}/{path.name}'
            text = path.read_bytes().decode('latin-1').replace('\r\n', '\n')
            if rel in ENGINE_HUNKS:
                text = unidiff.apply(text, [patch[rel][i] for i in ENGINE_HUNKS[rel]], rel)
            for old, new in ours.get(path.name, []):
                if text.count(old) != 1:
                    sys.exit(f'prepare.py: {rel} is not what it was (looked for {old!r})')
                text = text.replace(old, new)
            put(out / 'build' / sub / path.name, change(f'build/{sub}/{path.name}', text))

    # the game: NBlood's, fpdoom's patch, its eduke32/ headers, then ours
    patch = unidiff.read((fpdoom / 'NBlood.patch').read_bytes().decode('latin-1'))
    files = {}
    for path in sorted((game / 'src').iterdir()):
        if path.is_file() and path.suffix in ('.cpp', '.h'):
            files[path.name] = path.read_bytes().decode('latin-1').replace('\r\n', '\n')
    for rel, hunks in patch.items():
        name = rel.split('/')[-1]
        if name not in files:
            sys.exit(f'prepare.py: NBlood has no {rel}')
        files[name] = unidiff.apply(files[name], hunks, rel)
    for path in sorted((fpdoom / 'eduke32').iterdir()):
        files[path.name] = path.read_bytes().decode('latin-1').replace('\r\n', '\n')
    for name, text in files.items():
        put(out / 'game' / name, change(f'game/{name}', text))

    stamp.write_text('ok\n')


if __name__ == '__main__':
    main()
