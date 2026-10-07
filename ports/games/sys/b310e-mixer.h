/* Lightweight sample mixer for the Doom and Build ports. */
#ifndef B310E_MIXER_H
#define B310E_MIXER_H
#include "b310e-audio.h"
#ifdef __cplusplus
extern "C" {
#endif
void b310e_mixer_init(void);
void b310e_mixer_hook(void (*render)(int16_t *, unsigned));
int b310e_sample_play(int channel, const uint8_t *data, unsigned bytes,
                      unsigned rate, int volume, int separation, int pitch);
int b310e_sample_file(int channel, const uint8_t *data, unsigned bytes, int volume, int separation);
void b310e_sample_stop(int channel);
bool b310e_sample_active(int channel);
void b310e_sample_volume(int channel, int volume, int separation);
#ifdef __cplusplus
}
#endif
#endif
