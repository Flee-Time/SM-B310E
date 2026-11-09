/* B310E OS — loadable apps: the file format and the API the OS exports.
 *
 * An app is a file /b310e-os/apps/<name>.app built with os/sdk:
 *
 *   app_file_t        header, written by os/sdk/mkapp.py
 *   image             code, read-only data and data, linked at address 0;
 *                     an app_info_t at offset 0
 *   relocations       reloc_count little-endian uint32 offsets into the
 *                     image of words that hold an image address (each gets
 *                     the load address added)
 *
 * The loader reads it into the heap, applies the relocations, zeroes
 * bss_size bytes after the image, checks the CRC and calls info->start
 * with the API table. Apps call nothing in the OS directly: every service
 * is a pointer in os_api_t, so an app keeps working across OS builds as
 * long as the API's major version matches (minors only append members).
 *
 * Apps run in the UI thread: start() returns the app's first screen and
 * every callback of the app's screens runs there. Screens the app pushes
 * and memory it gets from api->malloc belong to it; when the last of its
 * screens leaves the stack (and its threads have ended), stop() runs and
 * the app and its memory are freed. Files it leaves open are not closed
 * for it. A thread of the app's (api->thread_create) counts as the app
 * too: its malloc, free, app_arg, threads and notifications are the
 * app's, and so is what it runs through ui_call.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef OS_APP_H
#define OS_APP_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "ff.h"
#include "os/alarm.h"
#include "os/audio.h"
#include "os/dsp.h"
#include "os/fs.h"
#include "os/kernel.h"
#include "os/media.h"
#include "os/notify.h"
#include "os/rtc.h"
#include "os/ui.h"

/* 2.0 (2026-11-07): the new UI. screen_t gained overlay, list_t icon and
 * toggle, so 1.x apps are refused; the table gained the drawing calls
 * after f_rename. */
/* 2.1 (2026-11-07): the multi-tap editor, label/value rows, graphs,
 * strrchr, icons by name and the card's state, after gfx_switch (the
 * Calculator and Notes moved out of the OS image). */
/* 2.2 (2026-11-07): the hardware and the services (threads, the clock, the
 * device, sound by category, WAV, the playback engine and play queue,
 * album art, the FM radio, alarms), appended after card_mounted; Music,
 * Recorder, FM radio and Alarm left the OS image for apps of their own. */
/* 2.3 (2026-11-07): app_info_t gained the app's own icon (the Apps list
 * reads it from the file, see app_peek_icon); the FM radio's speaker
 * (fm_set_speaker, audio_force_speaker) and ui_refresh (a redraw asked
 * from another thread), appended after alarm_days_text. */
/* 2.4 (2026-11-07): notifications (os/notify.h: post, remove, count, get),
 * shown in the quick panel; an app with one posted stays loaded. */
/* 2.5 (2026-11-07): ui_screen_mode (full-screen, raw-key, awake screens:
 * games) and ui_call (an app's thread runs code in the UI thread), after
 * notify_get; apps up to 2 MB (Doom). An app's thread that
 * ends lets the app be unloaded at once. */
/* 2.6 (2026-11-08): app_dir, after ui_call: the folder the app's .app is
 * in. Apps may sit in a folder of their own in APP_DIR, with their data
 * ("/b310e-os/apps/Doom/doom.app" and the WAD beside it). */
/* 2.7 (2026-11-08): app_reload, after app_dir: load the app again, fresh,
 * with an argument, once it has gone (an emulator: on to a ROM, back to
 * its list). */
/* 2.8 (2026-11-08): relocations may be unaligned (a pointer in a packed
 * struct: Wolf4SDL's states); nothing new in the table. */
#define OS_API_MAJOR 2
/* 2.9 (2026-11-08): ui_present, after app_reload: from a full-screen draw(),
 * a whole frame sent where it lies, no copy. */
/* 2.10 (2026-11-08): volume_register, after ui_present: a volume of the app's
 * own in the quick panel's Sound page (os/audio.h). Older apps get one
 * too: audio_play uses a tag under the app's name, and VOL_APP from an app
 * means its own. */
/* 2.11 (2026-11-08): l1, after volume_register: layer 1 and the DSP
 * (os/dsp.h, l1_api_t) for the radio experiments in os/modules/radiolab. */
/* 2.12 (2026-11-09): l1_api_t: si_parse, cell_sync, cell_read, cell_idle,
 * rxlev_measure, cell_c1, cell_select, cell_stay, after probe (os/l1cell.h). */
/* 2.13 (2026-11-09): l1_api_t: cch_classify, cch_read (the common control
 * channel, receive only). */
/* 2.14 (2026-11-09): ui_is_dark, so a game can stop while the panel is asleep; ui_depth;
 * settings: awake_limit_s, player_idle_s; key_event_t.repeat. */
#define OS_API_MINOR 14

#define APP_MAGIC   0x50413342u         /* "B3AP" */
#define APP_DIR     "/b310e-os/apps"
#define APP_MAX_SIZE (2560u * 1024)     /* image + bss (512 KB before 2.5) */

typedef struct {
    uint32_t magic;
    uint16_t api_major, api_minor;      /* the API the app was built for */
    uint32_t header_size;               /* sizeof(app_file_t) */
    uint32_t image_size;
    uint32_t bss_size;
    uint32_t reloc_count;
    uint32_t crc32;                     /* image, then the relocations */
    char name[20];                      /* NUL-terminated */
} app_file_t;

typedef struct os_api os_api_t;

typedef struct {
    uint16_t api_major, api_minor;
    const char *name;
    screen_t *(*start)(const os_api_t *os);
    void (*stop)(void);                 /* may be NULL */
    const icon_t *icon;                 /* 2.3: in the Apps list; NULL: the default */
} app_info_t;
#define APP_INFO_ICON 16                /* its offset: the list reads it from the file */

struct os_api {
    uint16_t major, minor;
    uint32_t size;                      /* sizeof(os_api_t) of this OS */

    /* C library (memcpy, memset, memmove are in the SDK: the compiler
     * calls them itself) */
    int (*memcmp)(const void *a, const void *b, size_t n);
    size_t (*strlen)(const char *s);
    int (*strcmp)(const char *a, const char *b);
    char *(*strncpy)(char *dst, const char *src, size_t n);
    char *(*strchr)(const char *s, int c);
    int (*snprintf)(char *buf, size_t size, const char *fmt, ...);
    int (*vsnprintf)(char *buf, size_t size, const char *fmt, va_list ap);

    /* memory, freed with the app */
    void *(*malloc)(size_t size);
    void *(*zalloc)(size_t size);
    void (*free)(void *p);

    /* time, log, device */
    uint32_t (*ticks)(void);            /* ms since boot */
    void (*sleep)(uint32_t ms);         /* blocks the UI: keep it short */
    void (*klog)(const char *fmt, ...);
    void (*rtc_time)(int *h, int *m, int *s);
    int (*battery_mv)(void);
    int (*battery_percent)(int mv);
    bool (*charger_present)(void);

    /* drawing (ui/gfx.c) */
    void (*gfx_fill)(int x, int y, int w, int h, uint16_t c);
    void (*gfx_hline)(int x, int y, int w, uint16_t c);
    void (*gfx_rect)(int x, int y, int w, int h, uint16_t c);
    int (*gfx_text)(int x, int y, const char *s, uint16_t c, int font,
                    int scale, int max_w);
    int (*gfx_text_width)(const char *s, int font, int scale);
    int (*gfx_text_centered)(int y, const char *s, uint16_t c, int font,
                             int scale);

    /* screens and widgets (ui/ui.c, ui/widgets.c) */
    void (*ui_push)(screen_t *s);
    void (*ui_pop)(void);
    void (*ui_home)(void);
    void (*ui_redraw)(void);
    void (*list_draw)(list_t *l);
    bool (*list_key)(list_t *l, const key_event_t *ev);
    void (*ui_confirm)(const char *title, const char *message, void (*yes)(void));
    void (*ui_message)(const char *title, const char *message);
    void (*ui_text_view)(const char *title, const char *text);
    const ui_settings_t *settings;

    /* files: FatFs R0.15a with os/fs/ffconf.h (UTF-8 paths) */
    FRESULT (*f_open)(FIL *fp, const TCHAR *path, BYTE mode);
    FRESULT (*f_close)(FIL *fp);
    FRESULT (*f_read)(FIL *fp, void *buff, UINT btr, UINT *br);
    FRESULT (*f_write)(FIL *fp, const void *buff, UINT btw, UINT *bw);
    FRESULT (*f_lseek)(FIL *fp, FSIZE_t ofs);
    FRESULT (*f_sync)(FIL *fp);
    FRESULT (*f_opendir)(DIR *dp, const TCHAR *path);
    FRESULT (*f_closedir)(DIR *dp);
    FRESULT (*f_readdir)(DIR *dp, FILINFO *fno);
    FRESULT (*f_stat)(const TCHAR *path, FILINFO *fno);
    FRESULT (*f_mkdir)(const TCHAR *path);
    FRESULT (*f_unlink)(const TCHAR *path);
    FRESULT (*f_rename)(const TCHAR *old_name, const TCHAR *new_name);

    /* the UI's look (2.0) */
    int (*gfx_text_height)(int font, int scale);
    void (*gfx_round)(int x, int y, int w, int h, int r, uint16_t c);
    void (*gfx_round_alpha)(int x, int y, int w, int h, int r, uint16_t c, int alpha);
    void (*gfx_shade)(int x, int y, int w, int h, uint16_t c, int alpha);
    void (*gfx_icon)(int x, int y, const icon_t *icon, uint16_t c);
    void (*gfx_bar)(int x, int y, int w, int h, int value, int max, uint16_t c);
    void (*gfx_switch)(int x, int y, bool on);

    /* 2.1 */
    void (*ui_text_edit)(const char *title, const char *initial, int max, bool multiline,
                         void (*done)(const char *text, void *ctx), void *ctx);
    void (*ui_kv)(int x, int y, int w, const char *label, const char *value, uint16_t c);
    int (*gfx_graph)(int x, int y, int w, int h, const uint16_t *v, int n, int head,
                     int max, uint16_t c);
    char *(*strrchr)(const char *s, int c);
    const icon_t *(*icon)(const char *name);    /* "file", "notes" ...; NULL if none */
    bool (*card_mounted)(void);

    /* 2.2: the hardware and the OS services, each through a wrapper */
    /* apps, threads (an app's thread keeps it loaded while it runs; below the UI) */
    const char *(*app_arg)(void);
    const char *(*app_launch)(const char *name, const char *arg);
    thread_t *(*thread_create)(const char *name, int prio, size_t stack, void (*fn)(void *), void *arg);
    void (*thread_yield)(void);
    void (*sem_init)(sem_t *s, int count);
    int (*sem_wait)(sem_t *s, uint32_t timeout_ms);
    void (*sem_post)(sem_t *s);
    void (*mutex_init)(mutex_t *m);
    int (*mutex_lock)(mutex_t *m, uint32_t timeout_ms);
    void (*mutex_unlock)(mutex_t *m);
    int (*queue_init)(queue_t *q, size_t msg_size, unsigned capacity);
    int (*queue_send)(queue_t *q, const void *msg, uint32_t timeout_ms);
    int (*queue_recv)(queue_t *q, void *msg, uint32_t timeout_ms);
    /* the UI */
    void (*ui_choose)(const char *title, const char *const *options, int n, int current, void (*picked)(int i, void *ctx), void *ctx);
    void (*gfx_blit)(int x, int y, int w, int h, const uint16_t *px);
    void (*ui_wake)(void);
    void (*ui_settings_save)(void);
    /* the clock */
    int (*rtc_get)(rtc_datetime_t *t);
    int (*rtc_set)(const rtc_datetime_t *t);
    int (*rtc_weekday)(int year, int month, int day);
    /* the device (adi_*: the analog die's registers, raw) */
    void (*torch_set)(bool on);
    bool (*torch_get)(void);
    void (*vibrator_set)(int level);
    void (*vibrate)(int level, uint32_t ms);
    void (*backlight_set)(int percent);
    void (*keylight_set)(bool on);
    int (*charge_state)(void);
    uint32_t (*adi_read)(uint32_t addr);
    bool (*adi_write)(uint32_t addr, uint32_t value);
    /* the system */
    size_t (*heap_free)(void);
    uint32_t (*cpu_idle)(void);
    int (*cpu_mhz)(void);
    void (*cpufreq_hold)(void);
    void (*cpufreq_release)(void);
    void (*power_off)(void);
    void (*reboot)(void);
    const fs_info_t *(*fs_info)(void);
    /* sound: the volume categories (VOL_*), PCM playback and recording (the fill and sink callbacks run in the DMA interrupt: copy only) */
    int (*volume_level)(int cat);
    void (*volume_set)(int cat, int level);
    void (*volume_use)(int cat);
    int (*audio_play)(unsigned rate, audio_fill_fn fill, void *ctx);
    void (*audio_play_stop)(void);
    bool (*audio_playing)(void);
    int (*audio_record)(unsigned rate, int source, audio_sink_fn sink, void *ctx);
    void (*audio_record_stop)(void);
    bool (*audio_recording)(void);
    unsigned (*audio_peak)(void);
    bool (*audio_headphones)(void);
    bool (*audio_speaker)(void);
    void (*audio_set_mic_gain)(int gain);
    int (*audio_get_mic_gain)(void);
    bool (*audio_rate_ok)(unsigned rate);
    void (*audio_headset_info)(audio_headset_info_t *out);
    /* WAV files (/b310e-os/rec for recordings); the UI loop keeps them going */
    const char *(*wav_play_file)(const char *path);
    const char *(*wav_play_tone)(unsigned hz);
    void (*wav_stop)(void);
    bool (*wav_playing)(void);
    bool (*wav_recording)(void);
    uint32_t (*wav_seconds)(void);
    const char *(*wav_name)(void);
    const char *(*wav_record)(unsigned rate, int source);
    uint32_t (*wav_dropped)(void);
    const char *(*wav_last_recording)(void);
    /* the playback engine (every format a codec module plays) */
    bool (*media_supported)(const char *path);
    const char *(*media_extensions)(void);
    int (*media_play)(const char *path);
    void (*media_stop)(void);
    void (*media_pause)(bool pause);
    void (*media_seek)(uint32_t ms);
    media_state_t (*media_state)(void);
    uint32_t (*media_position)(void);
    uint32_t (*media_duration)(void);
    bool (*media_info)(media_info_t *out);
    const char *(*media_error)(void);
    /* the play queue: plays on in the background */
    void (*media_queue_clear)(void);
    bool (*media_queue_add)(const char *path);
    int (*media_queue_add_folder)(const char *dir, int depth);
    int (*media_queue_add_m3u)(const char *path);
    int (*media_queue_count)(void);
    const char *(*media_queue_path)(int i);
    int (*media_queue_find)(const char *path);
    int (*media_queue_current)(void);
    bool (*media_queue_active)(void);
    int (*media_queue_play)(int i);
    void (*media_queue_step)(int d);
    void (*media_queue_toggle)(void);
    void (*media_queue_set_shuffle)(bool on);
    bool (*media_queue_shuffle)(void);
    void (*media_queue_set_repeat)(int mode);
    int (*media_queue_repeat)(void);
    const char *(*media_queue_source)(void);
    void (*media_queue_set_source)(const char *s);
    const char *(*media_queue_error)(void);
    const char *(*media_queue_now)(void);
    void (*art_request)(const char *path, const media_info_t *info, int side);
    const uint16_t *(*art_get)(const char *path, int side, int *state);
    /* the FM radio */
    bool (*fm_present)(void);
    int (*fm_on)(void);
    void (*fm_off)(void);
    bool (*fm_is_on)(void);
    void (*fm_tune)(int khz);
    int (*fm_frequency)(void);
    bool (*fm_station)(int rssi);
    int (*fm_rssi)(void);
    bool (*fm_stereo)(void);
    void (*fm_mute)(bool mute);
    /* alarms (the OS rings them) */
    int (*alarm_count)(void);
    bool (*alarm_get)(int i, alarm_t *out);
    int (*alarm_set)(int i, const alarm_t *a);
    bool (*alarm_delete)(int i);
    int (*alarm_next)(uint32_t *minutes_until);
    void (*alarm_days_text)(uint8_t days, char *out, int n);

    /* 2.3: the output */
    void (*audio_force_speaker)(bool on);
    bool (*audio_forced_speaker)(void);
    void (*fm_set_speaker)(bool on);
    bool (*fm_speaker)(void);
    void (*ui_refresh)(void);

    /* 2.4: notifications */
    bool (*notify_post)(notify_t *n);
    void (*notify_remove)(notify_t *n);
    int (*notify_count)(void);
    notify_t *(*notify_get)(int i);

    /* 2.5: screens of their own (games); run in the UI thread from another */
    void (*ui_screen_mode)(screen_t *s, unsigned flags);
    bool (*ui_call)(void (*fn)(void *arg), void *arg, uint32_t timeout_ms);

    /* 2.6: the folder the app's .app is in (APP_DIR or a folder of its own) */
    const char *(*app_dir)(void);

    /* 2.7: load the app again, fresh, with arg, once it has gone */
    bool (*app_reload)(const char *arg);
    bool (*ui_present)(const uint16_t *px);

    /* 2.10: a volume of the app's own under this name, listed from now on
     * and used for its audio_play; returns the category for volume_level,
     * volume_set and volume_use. A second name is a second tag: volume_use
     * picks which audio_play takes. Not needed just to have one. */
    int (*volume_register)(const char *name);

    /* 2.11: layer 1 and the DSP */
    const l1_api_t *l1;

    /* 2.14 */
    bool (*ui_is_dark)(void);
    int (*ui_depth)(void);
};

/* ---- the OS side (os/loader) -------------------------------------------- */
typedef struct app app_t;

extern const os_api_t os_api;

/* Load and start an app; NULL on success, else why it failed. */
const char *app_open(const char *path);
/* The same with an argument (app_arg() in it); NULL: none. */
const char *app_open_arg(const char *path, const char *arg);
/* The file itself, or an error if an app of that name is still loaded. */
const char *app_open_fresh(const char *path, const char *arg);
app_t *app_of_file(const char *path);   /* the loaded app that file would replace */
/* An app by its name or file name, with an argument (app_arg() in it). */
const char *app_launch(const char *name, const char *arg);
/* Its path, in APP_DIR or in a folder of its own there; false if none. */
bool app_find(const char *name, char *path, size_t size);
const char *app_dir(void);
bool app_reload(const char *arg);
const char *app_arg(void);
thread_t *app_thread(const char *name, int prio, size_t stack, void (*fn)(void *), void *arg);
/* Read only the header (for listings); 0, or -1 when it is not an app. */
int app_peek(const char *path, app_file_t *out);
const char *app_name(const app_t *a);
int app_volume_cat(void);               /* the volume the calling app plays at */
int app_volume_register(const char *name);
void app_volume_choose(int cat);        /* a tag: its next audio_play takes it */
/* The icon an app declared (API 2.3 and later), read from its file. */
bool app_peek_icon(const char *path, const app_file_t *h, icon_t *out);
/* Unload the apps that have no screen on the stack (the UI loop calls it). */
void app_collect(screen_t *const *stack, int depth);
bool app_is_loaded(const app_t *a);
void app_kill(app_t *a);                /* its threads end now (END held); the UI's */
bool app_reloading(void);               /* between app_reload and the fresh app */
int app_count(void);

/* The app whose code is running (the UI sets it around callbacks). */
extern app_t *app_current;
/* The app a call is for: an app thread's own (app_thread), else
 * app_current. What the OS tags as an app's (memory, threads,
 * notifications, screens, app_arg) goes by this. */
app_t *app_self(void);
void app_forget(app_t *a, void *p);     /* p was freed by the UI */

screen_t *app_apps(void);               /* the Apps menu entry */

#endif
