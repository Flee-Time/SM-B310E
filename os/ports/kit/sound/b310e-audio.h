/* The game kit's sound: what the shared mixer and MUS/MIDI synth
 * (b310e-mixer.c, b310e-music.c; first written for the boot menu's
 * games) need, over the kit's sound thread (kit.h) instead of the codec's
 * registers. The mixer renders in that thread; the game changes its
 * state only with interrupts off (kit_irq_save), so a render never sees
 * half a change.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef B310E_AUDIO_H
#define B310E_AUDIO_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <limits.h>
#include "kit.h"
#ifndef MIN
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#define MAX(a,b) ((a) > (b) ? (a) : (b))
#endif
#define B310E_RATE 22050                /* the mixer's and the synth's */
#define b310e_irq_save kit_irq_save
#define b310e_irq_restore kit_irq_restore
static inline void b310e_audio_init(unsigned rate) { (void)rate; }
static inline void b310e_audio_close(void) { kit_audio_stop(); }
static inline void b310e_audio_poll(void) {}
static inline void b310e_audio_render(void (*render)(int16_t *, unsigned))
{
    if (kit_audio_start(B310E_RATE, render))
        kit_log("no sound (the codec does not take 22050 Hz)");
}
#endif
