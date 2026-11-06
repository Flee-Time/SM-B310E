/* B310E OS — WAV playback and recording for the built-in audio screens.
 *
 * The DMA interrupt and the screen's tick share a ring buffer: the
 * interrupt takes (playback) or adds (recording) samples, the tick (every
 * 50 ms in the UI thread) reads or writes the file. 16-bit PCM only.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef APPS_WAVIO_H
#define APPS_WAVIO_H

#include <stdbool.h>
#include <stdint.h>

#define REC_DIR "/b310e-os/rec"

/* Playback of a file or a 1 kHz tone; NULL on success, else why not. */
const char *wav_play_file(const char *path);
const char *wav_play_tone(unsigned hz);
void wav_pump(void);                    /* from the screen's tick */
void wav_stop(void);                    /* playback or recording */
bool wav_playing(void);
bool wav_recording(void);
uint32_t wav_seconds(void);             /* played or recorded so far */
const char *wav_name(void);             /* the file playing or recording */

/* Recording to the next free REC_DIR/RECnnnn.WAV, mono. */
const char *wav_record(unsigned rate, int source);
uint32_t wav_dropped(void);             /* samples lost to a full ring */
const char *wav_last_recording(void);   /* "" when none this boot */

#endif
