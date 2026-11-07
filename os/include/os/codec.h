/* B310E OS — codec modules: the file format and the API between the
 * playback engine (os/media/) and a decoder.
 *
 * A codec is a file /b310e-os/codecs/<name>.cdc built from Rockbox's own
 * decoder sources (lib/rbcodec/codecs, GPLv2 or later) with the shim in
 * os/media/rockbox/. It has the .app layout (os/app.h: header, image
 * linked at 0, relocations; os/sdk/mkapp.py --magic) with CODEC_MAGIC,
 * and a codec_module_t at offset 0 of the image instead of an app_info_t.
 *
 * The engine reads the module into the heap and relocates it, then calls
 * probe() for a track (Rockbox's metadata parser for the format, compiled
 * into the module), entry(CODEC_LOAD) once, run() for each track and
 * entry(CODEC_UNLOAD) before the module is freed. All of it runs in the
 * engine's decoder thread, never in an interrupt. The decoder talks to
 * the engine only through struct codec_api: Rockbox's codec_api (the same
 * member names, so the codec sources build unchanged, minus the
 * multi-core, profiling and recording parts) and a few B310E additions at
 * the end.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef OS_CODEC_H
#define OS_CODEC_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CODEC_MAGIC      0x44433342u    /* "B3CD" */
#define CODEC_ABI_MAJOR  1
#define CODEC_ABI_MINOR  1           /* 1.1: the embedded cover in codec_track_t */
#define CODEC_DIR        "/b310e-os/codecs"
#define CODEC_MAX_SIZE   (1024u * 1024) /* image + bss */

struct mp3entry;                        /* Rockbox's track metadata: opaque here */
struct dsp_config;

enum codec_entry_call_reason {
    CODEC_LOAD = 0,
    CODEC_UNLOAD
};

enum codec_status {
    CODEC_OK = 0,
    CODEC_ERROR = -1,
};

enum codec_command_action {
    CODEC_ACTION_HALT = -1,
    CODEC_ACTION_NULL = 0,
    CODEC_ACTION_SEEK_TIME = 1,
    CODEC_ACTION_MIN = -0x7fffffffL - 1,
    CODEC_ACTION_MAX = 0x7fffffffL,
};

/* configure() settings (Rockbox dsp_core.h / dsp_misc.h names) */
enum {
    DSP_RESET = 1,
    DSP_SET_FREQUENCY,                  /* Hz */
    DSP_SET_SAMPLE_DEPTH,               /* 16: int16 samples; more: int32 with that many fraction bits */
    DSP_SET_STEREO_MODE,                /* STEREO_* */
    DSP_FLUSH,
    DSP_SET_PITCH,
    DSP_SET_OUT_FREQUENCY,
    DSP_GET_OUT_FREQUENCY,
    REPLAYGAIN_SET_GAINS = 0x100,       /* ignored: no DSP stage here */
};

enum {
    STEREO_INTERLEAVED,
    STEREO_NONINTERLEAVED,
    STEREO_MONO,
    STEREO_NUM_MODES,
};

struct codec_api {
    long filesize;                      /* total file length */
    long curpos;                        /* the decoder's position in the file */
    struct mp3entry *id3;               /* set by the module (its own copy) */
    int audio_hid;
    struct dsp_config *dsp;

    void *(*codec_get_buffer)(size_t *size);    /* the malloc arena */
    /* PCM out: count frames; ch2 is ignored for interleaved and mono */
    void (*pcmbuf_insert)(const void *ch1, const void *ch2, int count);
    void (*set_elapsed)(unsigned long ms);

    size_t (*read_filebuf)(void *ptr, size_t size);
    void *(*request_buffer)(size_t *realsize, size_t reqsize);
    void (*advance_buffer)(size_t amount);
    bool (*seek_buffer)(size_t newpos);
    void (*seek_complete)(void);
    void (*set_offset)(size_t value);
    void (*configure)(int setting, intptr_t value);
    long (*get_command)(intptr_t *param);
    bool (*loop_track)(void);
    void (*strip_filesize)(long value);

    unsigned (*sleep)(unsigned ticks);  /* Rockbox ticks: 10 ms */
    void (*yield)(void);

    void (*commit_dcache)(void);
    void (*commit_discard_dcache)(void);
    void (*commit_discard_idcache)(void);

    char *(*strcpy)(char *dst, const char *src);
    size_t (*strlen)(const char *str);
    int (*strcmp)(const char *a, const char *b);
    char *(*strcat)(char *s1, const char *s2);
    void *(*memset)(void *dst, int c, size_t length);
    void *(*memcpy)(void *out, const void *in, size_t n);
    void *(*memmove)(void *out, const void *in, size_t n);
    int (*memcmp)(const void *s1, const void *s2, size_t n);
    void *(*memchr)(const void *s1, int c, size_t n);

    void (*debugf)(const char *fmt, ...);
    void (*logf)(const char *fmt, ...);
    void (*qsort)(void *base, size_t nmemb, size_t size,
                  int (*compar)(const void *, const void *));
    void (*panicf)(const char *msg, ...);

    /* ---- B310E additions -------------------------------------------- */
    /* the track's file for the metadata parser (fd 0); whence: SEEK_* */
    long (*fd_read)(int fd, void *buf, size_t n);
    long (*fd_lseek)(int fd, long offset, int whence);
    long (*fd_size)(int fd);
    int (*vsnprintf)(char *buf, size_t size, const char *fmt, va_list ap);
    void (*klog)(const char *fmt, ...);
};

/* What probe() found, copied out of the module's mp3entry (UTF-8). */
typedef struct {
    char title[64], artist[64], album[64];
    uint32_t length_ms;                 /* 0: unknown */
    uint32_t frequency;                 /* Hz */
    uint32_t bitrate;                   /* kbit/s, 0: unknown */
    uint32_t codectype;                 /* Rockbox AFMT_* */
    char codec[12];                     /* "MP3", "FLAC" ... */
    /* 1.1: the first embedded picture, as Rockbox's parsers find it */
    uint32_t art_type;                  /* MEDIA_ART_* (0: none) */
    uint32_t art_flags;                 /* MEDIA_ART_UNSYNC, MEDIA_ART_BASE64 */
    uint32_t art_pos, art_size;         /* in the file */
} codec_track_t;

typedef struct {
    uint16_t abi_major, abi_minor;
    const char *name;                   /* the file's name: "mpa" (mkapp reads it) */
    const char *extensions;             /* "mp3 mp2 mp1 mpa", lower case */
    const char *description;            /* "MPEG audio (libmad)" */
    uint32_t arena_size;                /* bytes codec_get_buffer() should give */
    /* parse the track open as fd 0; 0 when this module plays it */
    int (*probe)(struct codec_api *api, int fd, codec_track_t *out);
    /* the next run() starts at ms (a seek after the decoder ended) */
    void (*start_at)(unsigned long ms);
    enum codec_status (*entry)(enum codec_entry_call_reason reason);
    enum codec_status (*run)(void);
} codec_module_t;

#endif
