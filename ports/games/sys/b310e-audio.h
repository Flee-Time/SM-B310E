/* Shared game PCM output. SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef B310E_AUDIO_H
#define B310E_AUDIO_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <limits.h>
#ifdef __cplusplus
extern "C" {
#endif
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#define MAX(a,b) ((a) > (b) ? (a) : (b))
#define HW_NUM_FREQ 11
#define HW_FREQ_DEFAULT 5
extern const unsigned hw_freq_sampr[HW_NUM_FREQ];
int b310e_irq_save(void);
void b310e_irq_restore(int);
#define disable_irq_save b310e_irq_save
#define restore_irq b310e_irq_restore
void sys_wait_us(uint32_t);
#define udelay sys_wait_us
void audiohw_init(void);
void audiohw_close(void);
void audiohw_set_frequency(int);
void audiohw_set_volume(int, int);
void audiohw_enable_speaker(bool);
void pcm_set_master_volume(int, int);
void b310e_audio_init(unsigned rate);
void b310e_audio_close(void);
void b310e_audio_irq(void);
void b310e_audio_poll(void);
void b310e_audio_volume(int direction);
void b310e_audio_submit(const int16_t *stereo, unsigned frames);
void b310e_audio_u8(const uint8_t *, unsigned bytes, bool stereo);
void b310e_audio_render(void (*render)(int16_t *, unsigned));
unsigned b310e_audio_queued(void);
#include "b310e-registers.h"
#ifdef __cplusplus
}
#endif
#endif
