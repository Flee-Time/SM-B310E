/* Blood on B310E OS: what the shared Build layer (os/ports/build/build_game.h) asks of
 * the game, and what its C++ needs without the C++ library.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdlib.h>

#include "compat.h"
#include "blood.h"
#include "gamemenu.h"
#include "keyboard.h"
#include "kit.h"
#include "build_game.h"

bool build_in_menu(void) { return gGameMenuMgr.m_bActive; }
void build_quit(void) { gQuitGame = true; }
void build_each_second(void) {}

/* the frame, the game's own mallocs (about 190 KB at its first level); its art, its
 * weapons' animations, palettes and sounds are in the cache */
extern "C" const size_t build_outside_cache;
const size_t build_outside_cache = 400 * 1024;
extern "C" const bool build_looped_vol_is_distance;
const bool build_looped_vol_is_distance = false;

/* C++ without its library: new and delete are malloc's */
void *operator new(size_t n) { return malloc(n ? n : 1); }
void *operator new[](size_t n) { return malloc(n ? n : 1); }
void operator delete(void *p) noexcept { free(p); }
void operator delete[](void *p) noexcept { free(p); }
void operator delete(void *p, size_t) noexcept { free(p); }
void operator delete[](void *p, size_t) noexcept { free(p); }
extern "C" void __cxa_pure_virtual(void) { abort(); }

/* NBlood's audiolib calls (EDuke32's), on the shared one (blood_audio.h) */
int FX_Init(int numvoices, int numchannels, int mixrate, void *initdata)
{
    int bits = 16;

    return FX_Init(0, numvoices, &numchannels, &bits, &mixrate, initdata);
}

int FX_Play(char *ptr, uint32_t length, int loopstart, int loopend, int pitchoffset, int vol,
            int left, int right, int priority, int32_t volume, intptr_t callbackval)
{
    if (loopstart >= 0 && loopend > loopstart)
        return FX_PlayLoopedAuto(ptr, length, loopstart, loopend, pitchoffset, vol, left, right,
                                 priority, (unsigned)callbackval);
    return FX_PlayAuto(ptr, length, pitchoffset, vol, left, right, priority, (unsigned)callbackval);
}

int FX_PlayRaw(char *ptr, uint32_t length, int rate, int pitchoffset, int vol, int left, int right,
               int priority, int32_t volume, intptr_t callbackval)
{
    return FX_PlayRaw(ptr, (unsigned)length, (unsigned)rate, pitchoffset, vol, left, right, priority,
                      (unsigned)callbackval);
}

int FX_PlayLoopedRaw(char *ptr, uint32_t length, char *loopstart, char *loopend, int rate,
                     int pitchoffset, int vol, int left, int right, int priority, int32_t volume,
                     intptr_t callbackval)
{
    return FX_PlayLoopedRaw(ptr, (unsigned)length, loopstart, loopend, (unsigned)rate, pitchoffset,
                            vol, left, right, priority, (unsigned)callbackval);
}

void FX_SetCallBack(void (*function)(intptr_t))
{
    FX_SetCallBack((void (*)(unsigned int))function);  /* (the same size here) */
}

int MUSIC_Init(int card) { return MUSIC_Init(card, NULL); }
