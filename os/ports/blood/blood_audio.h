/* Blood on B310E OS: the audiolib calls NBlood makes (EDuke32's), on the shared one
 * (os/ports/build/sound.c, JonoF's names): overloads where the two differ.
 * blood_game.cpp has them. Included by the game's compat.h (prepare.py).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef BLOOD_AUDIO_H
#define BLOOD_AUDIO_H

#include <malloc.h>
#include <stdint.h>

#include "fx_man.h"
#include "music.h"

int FX_Init(int numvoices, int numchannels, int mixrate, void *initdata);
int FX_Play(char *ptr, uint32_t length, int loopstart, int loopend, int pitchoffset, int vol,
            int left, int right, int priority, int32_t volume, intptr_t callbackval);
int FX_PlayRaw(char *ptr, uint32_t length, int rate, int pitchoffset, int vol, int left, int right,
               int priority, int32_t volume, intptr_t callbackval);
int FX_PlayLoopedRaw(char *ptr, uint32_t length, char *loopstart, char *loopend, int rate,
                     int pitchoffset, int vol, int left, int right, int priority, int32_t volume,
                     intptr_t callbackval);
void FX_SetCallBack(void (*function)(intptr_t));
int MUSIC_Init(int card);
static inline int MIDI_GetDevice(void) { return 0; }

#define Xaligned_alloc(alignment, size) memalign((alignment), (size))
#define ALIGNED_FREE_AND_NULL(p) do { free(p); (p) = NULL; } while (0)

#endif
