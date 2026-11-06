/* B310E OS — audio: codec, playback, recording, FM radio, headset.
 *
 * Playback and recording run on DMA through the voice band codec in banks
 * of 160 frames; the callbacks run in the DMA interrupt, so they only copy
 * (from or into a ring buffer a thread fills or empties). Output follows
 * the jack: headphones when plugged in, the speaker otherwise. Recording
 * is mono (the SC6530C's ADC R does not work), from the phone's mic, a
 * 4-pole headset's mic, or the FM radio. All on the ARM: no DSP.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef OS_AUDIO_H
#define OS_AUDIO_H

#include <stdbool.h>
#include <stdint.h>

/* ---- codec (drivers/audio.c) -------------------------------------------- */
void audio_power(bool on);          /* off only when nothing uses it */
bool audio_rate_ok(unsigned rate);  /* 8000 .. 96000, the codec's list */
void audio_set_rate(unsigned rate);
void audio_set_volume(int db10);    /* tenths of a dB, -1000 (mute) .. 240 */
int audio_get_volume(void);
void audio_mute(bool mute);
bool audio_headphones(void);        /* the jack */
bool audio_speaker(void);           /* the output in use */
void audio_set_mic_gain(int gain);  /* 0..15 */
int audio_get_mic_gain(void);
void audio_fm_route(bool on);       /* FM line-in straight to the output */

enum { AUDIO_SRC_MIC, AUDIO_SRC_FM };
int audio_record_route(int source); /* returns the mic path: 0xc0 phone, 0x0c headset */
void audio_record_route_off(void);
bool audio_recording(void);
int audio_mic_path(void);
int audio_headset_mic_raw(void);    /* headset mic line at the last start */

/* headset buttons, polled by the keypad's END thread every 20 ms */
#define AUDIO_HS_CENTER  1u
#define AUDIO_HS_PLUS    2u
#define AUDIO_HS_MINUS   4u
unsigned audio_headset_tick(bool hook, uint32_t now);
typedef struct {
    bool inserted, buttons;         /* plugged, buttons enabled (4-pole) */
    int idle, raw;                  /* mic line, raw ADC */
    unsigned held;                  /* AUDIO_HS_* */
} audio_headset_info_t;
void audio_headset_info(audio_headset_info_t *out);

/* ---- playback and recording (drivers/pcm.c) ----------------------------- */
/* Fill up to frames stereo frames (L, R interleaved); return how many were
 * written, 0 at the end. Runs in the DMA interrupt. */
typedef unsigned (*audio_fill_fn)(int16_t *lr, unsigned frames, void *ctx);
/* Take frames mono samples. Runs in the DMA interrupt. */
typedef void (*audio_sink_fn)(const int16_t *mono, unsigned frames, void *ctx);

int audio_play(unsigned rate, audio_fill_fn fill, void *ctx);
void audio_play_stop(void);
bool audio_playing(void);
int audio_record(unsigned rate, int source, audio_sink_fn sink, void *ctx);
void audio_record_stop(void);
unsigned audio_peak(void);          /* largest sample since the last call */

/* ---- FM radio, RTC6211N on I2C 1 (drivers/fm.c) ------------------------- */
bool fm_present(void);              /* probed once; powers the tuner briefly */
int fm_on(void);                    /* power up, route, unmute; 0 ok */
void fm_off(void);
bool fm_is_on(void);
void fm_tune(int khz);              /* 87500 .. 108000, 100 kHz steps */
int fm_frequency(void);             /* kHz */
bool fm_station(int rssi);          /* strong enough to stop a seek */
int fm_rssi(void);
bool fm_stereo(void);
void fm_mute(bool mute);

#endif
