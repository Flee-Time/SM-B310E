/* B310E OS — the playback engine (os/media/): compressed and PCM audio
 * files through codec modules on the card (os/codec.h).
 *
 * One track at a time. media_play() hands the path to the engine's
 * decoder thread (below the UI's priority) and returns at once; the thread
 * picks a codec by the file's extension or its first bytes, loads it from
 * /b310e-os/codecs/, reads the track's tags, and decodes into a PCM ring
 * of about 1.4 s that the audio DMA interrupt plays from (os/audio.h). The
 * calls here are for the UI thread (or an app); none blocks for long.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef OS_MEDIA_H
#define OS_MEDIA_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    MEDIA_IDLE,         /* nothing asked for yet, or stopped by media_stop() */
    MEDIA_LOADING,      /* finding the codec, reading the tags, filling the ring */
    MEDIA_PLAYING,
    MEDIA_PAUSED,
    MEDIA_ENDED,        /* played to the end */
    MEDIA_ERROR,        /* could not play it: media_error() says why */
    MEDIA_STOPPED,      /* something else took the audio (a tone, the alarm) */
} media_state_t;

typedef struct {
    char path[160];
    char title[64], artist[64], album[64];      /* UTF-8, "" when untagged */
    char codec[12];                             /* "MP3", "FLAC" ... */
    uint32_t length_ms;                         /* 0: unknown */
    uint32_t rate;                              /* the stream's Hz */
    uint32_t out_rate;                          /* played at (resampled if not the same) */
    uint32_t bitrate;                           /* kbit/s, 0: unknown */
    /* the embedded cover, if the tags have one (os/media/art.c reads it) */
    uint32_t art_type;                          /* MEDIA_ART_* */
    uint32_t art_flags;
    uint32_t art_pos, art_size;
} media_info_t;

enum { MEDIA_ART_NONE, MEDIA_ART_BMP, MEDIA_ART_PNG, MEDIA_ART_JPG };
#define MEDIA_ART_UNSYNC  1u            /* ID3 unsynchronisation (FF 00 -> FF) */
#define MEDIA_ART_BASE64  2u            /* a Vorbis comment: base64 in Ogg pages */

typedef struct {
    uint32_t underruns;         /* DMA banks played as silence: the ring ran dry */
    uint32_t decoded_ms;        /* audio decoded this track */
    uint32_t decode_cycles;     /* 26 MHz cycles the decoder thread ran this track */
    uint32_t ring_ms;           /* audio in the ring now */
} media_stats_t;

/* true when a codec on the card claims the file's extension */
bool media_supported(const char *path);
/* "mp3 mp2 flac ...": every extension the card's codecs play */
const char *media_extensions(void);
/* Start a track (stops the one playing); 0 when the request was taken. */
int media_play(const char *path);
void media_stop(void);
void media_pause(bool pause);
void media_seek(uint32_t ms);
media_state_t media_state(void);
uint32_t media_position(void);  /* ms, what is being heard */
uint32_t media_duration(void);  /* ms, 0: unknown */
/* the current track's info; false while none is known */
bool media_info(media_info_t *out);
const char *media_error(void);  /* why MEDIA_ERROR */
void media_stats(media_stats_t *out);

/* ---- the play queue (os/media/queue.c) ----------------------------------------- */
/* Tracks played one after the other, in the background; the UI loop runs
 * media_queue_tick(). */
enum { MEDIA_REPEAT_ALL, MEDIA_REPEAT_ONE, MEDIA_REPEAT_OFF };
void media_queue_clear(void);           /* and stops what the queue plays */
bool media_queue_add(const char *path);
int media_queue_add_folder(const char *dir, int depth);  /* tracks added, sorted */
int media_queue_add_m3u(const char *path);
int media_queue_count(void);
const char *media_queue_path(int i);
int media_queue_find(const char *path); /* its index, or -1 */
int media_queue_current(void);          /* the track playing or last played, -1 */
bool media_queue_active(void);          /* the queue owns the playback */
int media_queue_play(int i);            /* 0 when it starts */
void media_queue_step(int d);           /* +1 next, -1 previous, in play order */
void media_queue_toggle(void);          /* pause / resume, or start */
void media_queue_set_shuffle(bool on);
bool media_queue_shuffle(void);
void media_queue_set_repeat(int mode);  /* MEDIA_REPEAT_* */
int media_queue_repeat(void);
const char *media_queue_source(void);   /* "/music", "list.m3u" ... */
void media_queue_set_source(const char *s);
const char *media_queue_error(void);    /* the last track's error, or NULL */
const char *media_queue_now(void);      /* the file name playing, or NULL */
void media_queue_tick(void);            /* the UI loop, every 50 ms */

#endif
