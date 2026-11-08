#ifndef B310E_MUSIC_H
#define B310E_MUSIC_H
#include "b310e-audio.h"
#ifdef __cplusplus
extern "C" {
#endif
bool b310e_music_start(const void *data, unsigned size, bool loop);
bool b310e_music_start_borrowed(const void *data, unsigned size, bool loop);
void b310e_music_stop(void);
void b310e_music_pause(bool pause);
void b310e_music_volume(int volume);
bool b310e_music_playing(void);
void b310e_music_render(int *out, unsigned frames);   /* added to out */
#ifdef __cplusplus
}
#endif
#endif
