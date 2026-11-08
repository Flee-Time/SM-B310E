/* Wolf4SDL on B310E OS: the part of SDL_mixer 2 Wolf4SDL uses, on the game
 * kit's sound (sdl_kit.c): 8 channels of 8-bit mono samples (the
 * digitised sounds, panned), a music hook (Wolf4SDL's OPL music and AdLib
 * effects) and a post-mix callback (its PC speaker). Not SDL_mixer's
 * code; its names.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WOLF_SDL_MIXER_H
#define WOLF_SDL_MIXER_H

#include "SDL.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MIX_CHANNELS 8

typedef struct Mix_Chunk {
    int allocated;
    Uint8 *abuf;                        /* 8-bit signed mono at KEEP_RATE */
    Uint32 alen;                        /* bytes (samples) */
    Uint8 volume;
} Mix_Chunk;

int Mix_OpenAudioDevice(int frequency, Uint16 format, int channels, int chunksize,
                        const char *device, int allowed_changes);
int Mix_QuerySpec(int *frequency, Uint16 *format, int *channels);
const char *Mix_GetError(void);
int Mix_ReserveChannels(int num);
int Mix_GroupChannels(int from, int to, int tag);
int Mix_GroupAvailable(int tag);
int Mix_GroupOldest(int tag);
void Mix_HookMusic(void (*fn)(void *udata, Uint8 *stream, int len), void *arg);
void Mix_SetPostMix(void (*fn)(void *udata, Uint8 *stream, int len), void *arg);
void Mix_ChannelFinished(void (*fn)(int channel));
Mix_Chunk *Mix_LoadWAV_RW(SDL_RWops *src, int freesrc);
void Mix_FreeChunk(Mix_Chunk *chunk);
int Mix_PlayChannel(int channel, Mix_Chunk *chunk, int loops);
int Mix_HaltChannel(int channel);
int Mix_SetPanning(int channel, Uint8 left, Uint8 right);
int Mix_Playing(int channel);

#ifdef __cplusplus
}
#endif

#endif
