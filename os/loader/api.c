/* B310E OS — the API table apps get (os/app.h). Version 2.0: append new
 * members at the end and raise OS_API_MINOR; anything else is a new major.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/app.h"
#include "os/drivers.h"
#include "os/fs.h"
#include "os/kernel.h"
#include "os/lib.h"
#include "os/art.h"
#include "apps/wavio.h"

void app_cpufreq_hold(void);
void app_cpufreq_release(void);

/* PCM playback for apps: each at its own volume */
static int app_audio_play(unsigned rate, audio_fill_fn fill, void *ctx)
{
    volume_use(app_volume_cat());
    return audio_play(rate, fill, ctx);
}

/* VOL_APP from an app is its own (so its keys never move the others') */
static int own_cat(int cat)
{
    return cat == VOL_APP ? app_volume_cat() : cat;
}

static int app_volume_level(int cat)
{
    return volume_level(own_cat(cat));
}

static void app_volume_set(int cat, int level)
{
    volume_set(own_cat(cat), level);
}

static void app_volume_use(int cat)
{
    cat = own_cat(cat);
    app_volume_choose(cat);
    volume_use(cat);
}

void *app_malloc(size_t size);
void *app_zalloc(size_t size);
void app_free(void *p);

/* the list icons by name, for apps (2.1) */
static const icon_t *icon_by_name(const char *name)
{
    static const struct { const char *name; const icon_t *icon; } icons[] = {
        { "apps", &icon_apps }, { "folder", &icon_folder }, { "file", &icon_file },
        { "music", &icon_music }, { "radio", &icon_radio }, { "tools", &icon_tools },
        { "chart", &icon_chart }, { "gear", &icon_gear }, { "power", &icon_power },
        { "torch", &icon_torch }, { "vibrate", &icon_vibrate }, { "sim", &icon_sim },
        { "usb", &icon_usb }, { "clock", &icon_clock }, { "sun", &icon_sun },
        { "screen", &icon_screen }, { "cpu", &icon_cpu }, { "info", &icon_info },
        { "play", &icon_play }, { "record", &icon_record }, { "mic", &icon_mic },
        { "restart", &icon_restart }, { "calc", &icon_calc }, { "notes", &icon_notes },
        { "alarm", &icon_alarm },
    };

    for (unsigned i = 0; i < ARRAY_SIZE(icons); i++)
        if (!strcmp(icons[i].name, name))
            return icons[i].icon;
    return NULL;
}

static bool card_mounted(void)
{
    return fs_info()->mounted;
}

/* ---- the store for apps (2.17): everything under "app/<the app's name>/" ---- */

static int ns(const char *name, char *out, size_t n)
{
    app_t *a = app_self();

    if (!a || !name || (size_t)snprintf(out, n, "app/%s/%s", app_name(a), name) >= n)
        return 0;
    return 1;
}

static bool own(db_table_t *t)
{
    char prefix[DB_NAME_MAX];
    const char *name = db_table_name(t);
    size_t n;

    if (!ns("", prefix, sizeof(prefix)))
        return false;
    n = strlen(prefix);
    return strlen(name) > n && !memcmp(name, prefix, n);
}

static int app_db_get_int(const char *key, int def)
{
    char k[DB_KEY_MAX];

    return ns(key, k, sizeof(k)) ? db_get_int(k, def) : def;
}

static bool app_db_get_bool(const char *key, bool def)
{
    char k[DB_KEY_MAX];

    return ns(key, k, sizeof(k)) ? db_get_bool(k, def) : def;
}

static int app_db_get_str(const char *key, char *out, size_t n, const char *def)
{
    char k[DB_KEY_MAX];

    return db_get_str(ns(key, k, sizeof(k)) ? k : NULL, out, n, def);
}

static int app_db_get_blob(const char *key, void *out, size_t n)
{
    char k[DB_KEY_MAX];

    return ns(key, k, sizeof(k)) ? db_get_blob(k, out, n) : DB_EINVAL;
}

static int app_db_set_int(const char *key, int v)
{
    char k[DB_KEY_MAX];

    return ns(key, k, sizeof(k)) ? db_set_int(k, v) : DB_EINVAL;
}

static int app_db_set_bool(const char *key, bool v)
{
    char k[DB_KEY_MAX];

    return ns(key, k, sizeof(k)) ? db_set_bool(k, v) : DB_EINVAL;
}

static int app_db_set_str(const char *key, const char *s)
{
    char k[DB_KEY_MAX];

    return ns(key, k, sizeof(k)) ? db_set_str(k, s) : DB_EINVAL;
}

static int app_db_set_blob(const char *key, const void *p, size_t n)
{
    char k[DB_KEY_MAX];

    return ns(key, k, sizeof(k)) ? db_set_blob(k, p, n) : DB_EINVAL;
}

static int app_db_delete(const char *key)
{
    char k[DB_KEY_MAX];

    return ns(key, k, sizeof(k)) ? db_delete(k) : DB_EINVAL;
}

static db_table_t *app_db_table_open(const db_schema_t *s)
{
    char name[DB_NAME_MAX];
    db_schema_t mine;

    if (!s || !ns(s->name, name, sizeof(name)))
        return NULL;
    mine = *s;
    mine.name = name;                   /* copied by the store */
    return db_table_open(&mine);
}

static uint32_t app_db_insert(db_table_t *t, const db_value_t *v, int n)
{
    return own(t) ? db_insert(t, v, n) : 0;
}

static int app_db_get(db_table_t *t, uint32_t id, db_value_t *v, int n, void *buf, size_t size)
{
    return own(t) ? db_get(t, id, v, n, buf, size) : DB_ENOENT;
}

static int app_db_find(db_table_t *t, int field, const db_value_t *value, uint32_t *ids, int max,
                       uint32_t before)
{
    return own(t) ? db_find(t, field, value, ids, max, before) : 0;
}

static int app_db_update(db_table_t *t, uint32_t id, const db_value_t *v, int n)
{
    return own(t) ? db_update(t, id, v, n) : DB_ENOENT;
}

static int app_db_update_field(db_table_t *t, uint32_t id, int field, const db_value_t *v)
{
    return own(t) ? db_update_field(t, id, field, v) : DB_ENOENT;
}

static int app_db_remove(db_table_t *t, uint32_t id)
{
    return own(t) ? db_remove(t, id) : DB_ENOENT;
}

static int app_db_count(db_table_t *t)
{
    return own(t) ? db_count(t) : 0;
}

const os_api_t os_api = {
    .major = OS_API_MAJOR, .minor = OS_API_MINOR, .size = sizeof(os_api_t),

    .memcmp = memcmp, .strlen = strlen, .strcmp = strcmp, .strncpy = strncpy,
    .strchr = strchr, .snprintf = snprintf, .vsnprintf = vsnprintf,

    .malloc = app_malloc, .zalloc = app_zalloc, .free = app_free,

    .ticks = kernel_ticks, .sleep = thread_sleep, .klog = klog,
    .rtc_time = rtc_time, .battery_mv = battery_mv,
    .battery_percent = battery_percent, .charger_present = charger_present,

    .gfx_fill = gfx_fill, .gfx_hline = gfx_hline, .gfx_rect = gfx_rect,
    .gfx_text = gfx_text, .gfx_text_width = gfx_text_width,
    .gfx_text_centered = gfx_text_centered,

    .ui_push = ui_push, .ui_pop = ui_pop, .ui_home = ui_home,
    .ui_redraw = ui_redraw, .list_draw = list_draw, .list_key = list_key,
    .ui_confirm = ui_confirm, .ui_message = ui_message,
    .ui_text_view = ui_text_view, .settings = &ui_settings,

    .f_open = f_open, .f_close = f_close, .f_read = f_read,
    .f_write = f_write, .f_lseek = f_lseek, .f_sync = f_sync,
    .f_opendir = f_opendir, .f_closedir = f_closedir, .f_readdir = f_readdir,
    .f_stat = f_stat, .f_mkdir = f_mkdir, .f_unlink = f_unlink,
    .f_rename = f_rename,

    .gfx_text_height = gfx_text_height, .gfx_round = gfx_round,
    .gfx_round_alpha = gfx_round_alpha, .gfx_shade = gfx_shade,
    .gfx_icon = gfx_icon, .gfx_bar = gfx_bar, .gfx_switch = gfx_switch,

    .ui_text_edit = ui_text_edit, .ui_kv = ui_kv, .gfx_graph = gfx_graph,
    .strrchr = strrchr, .icon = icon_by_name, .card_mounted = card_mounted,

    .app_arg = app_arg,
    .app_launch = app_launch,
    .thread_create = app_thread,
    .thread_yield = thread_yield,
    .sem_init = sem_init,
    .sem_wait = sem_wait,
    .sem_post = sem_post,
    .mutex_init = mutex_init,
    .mutex_lock = mutex_lock,
    .mutex_unlock = mutex_unlock,
    .queue_init = queue_init,
    .queue_send = queue_send,
    .queue_recv = queue_recv,
    .ui_choose = ui_choose,
    .gfx_blit = gfx_blit,
    .ui_wake = ui_wake,
    .ui_settings_save = ui_settings_save,
    .rtc_get = rtc_get,
    .rtc_set = rtc_set,
    .rtc_weekday = rtc_weekday,
    .torch_set = torch_set,
    .torch_get = torch_get,
    .vibrator_set = vibrator_set,
    .vibrate = ui_buzz,
    .backlight_set = backlight_set,
    .keylight_set = keylight_set,
    .charge_state = charge_state,
    .adi_read = adi_read,
    .adi_write = adi_write,
    .heap_free = heap_free_bytes,
    .cpu_idle = kernel_idle_permille,
    .cpu_mhz = clock_mhz,
    .cpufreq_hold = app_cpufreq_hold,
    .cpufreq_release = app_cpufreq_release,
    .power_off = ui_power_off,
    .reboot = ui_reboot,
    .fs_info = fs_info,
    .volume_level = app_volume_level,
    .volume_set = app_volume_set,
    .volume_use = app_volume_use,
    .audio_play = app_audio_play,
    .audio_play_stop = audio_play_stop,
    .audio_playing = audio_playing,
    .audio_record = audio_record,
    .audio_record_stop = audio_record_stop,
    .audio_recording = audio_recording,
    .audio_peak = audio_peak,
    .audio_headphones = audio_headphones,
    .audio_speaker = audio_speaker,
    .audio_set_mic_gain = audio_set_mic_gain,
    .audio_get_mic_gain = audio_get_mic_gain,
    .audio_rate_ok = audio_rate_ok,
    .audio_headset_info = audio_headset_info,
    .wav_play_file = wav_play_file,
    .wav_play_tone = wav_play_tone,
    .wav_stop = wav_stop,
    .wav_playing = wav_playing,
    .wav_recording = wav_recording,
    .wav_seconds = wav_seconds,
    .wav_name = wav_name,
    .wav_record = wav_record,
    .wav_dropped = wav_dropped,
    .wav_last_recording = wav_last_recording,
    .media_supported = media_supported,
    .media_extensions = media_extensions,
    .media_play = media_play,
    .media_stop = media_stop,
    .media_pause = media_pause,
    .media_seek = media_seek,
    .media_state = media_state,
    .media_position = media_position,
    .media_duration = media_duration,
    .media_info = media_info,
    .media_error = media_error,
    .media_queue_clear = media_queue_clear,
    .media_queue_add = media_queue_add,
    .media_queue_add_folder = media_queue_add_folder,
    .media_queue_add_m3u = media_queue_add_m3u,
    .media_queue_count = media_queue_count,
    .media_queue_path = media_queue_path,
    .media_queue_find = media_queue_find,
    .media_queue_current = media_queue_current,
    .media_queue_active = media_queue_active,
    .media_queue_play = media_queue_play,
    .media_queue_step = media_queue_step,
    .media_queue_toggle = media_queue_toggle,
    .media_queue_set_shuffle = media_queue_set_shuffle,
    .media_queue_shuffle = media_queue_shuffle,
    .media_queue_set_repeat = media_queue_set_repeat,
    .media_queue_repeat = media_queue_repeat,
    .media_queue_source = media_queue_source,
    .media_queue_set_source = media_queue_set_source,
    .media_queue_error = media_queue_error,
    .media_queue_now = media_queue_now,
    .art_request = art_request,
    .art_get = art_get,
    .fm_present = fm_present,
    .fm_on = fm_on,
    .fm_off = fm_off,
    .fm_is_on = fm_is_on,
    .fm_tune = fm_tune,
    .fm_frequency = fm_frequency,
    .fm_station = fm_station,
    .fm_rssi = fm_rssi,
    .fm_stereo = fm_stereo,
    .fm_mute = fm_mute,
    .alarm_count = alarm_count,
    .alarm_get = alarm_get,
    .alarm_set = alarm_set,
    .alarm_delete = alarm_delete,
    .alarm_next = alarm_next,
    .alarm_days_text = alarm_days_text,
    .audio_force_speaker = audio_force_speaker,
    .audio_forced_speaker = audio_forced_speaker,
    .fm_set_speaker = fm_set_speaker,
    .fm_speaker = fm_speaker,
    .ui_refresh = ui_refresh,
    .notify_post = notify_post,
    .notify_remove = notify_remove,
    .notify_count = notify_count,
    .notify_get = notify_get,
    .ui_screen_mode = ui_screen_mode,
    .ui_call = ui_call,
    .app_dir = app_dir,
    .app_reload = app_reload,
    .ui_present = ui_present,
    .volume_register = app_volume_register,
    .l1 = &l1_api,
    .ui_is_dark = ui_is_dark,
    .ui_depth = ui_depth,
    .app_on_fault = app_on_fault,
    .db_get_int = app_db_get_int,
    .db_get_bool = app_db_get_bool,
    .db_get_str = app_db_get_str,
    .db_get_blob = app_db_get_blob,
    .db_set_int = app_db_set_int,
    .db_set_bool = app_db_set_bool,
    .db_set_str = app_db_set_str,
    .db_set_blob = app_db_set_blob,
    .db_delete = app_db_delete,
    .db_table_open = app_db_table_open,
    .db_insert = app_db_insert,
    .db_get = app_db_get,
    .db_find = app_db_find,
    .db_update = app_db_update,
    .db_update_field = app_db_update_field,
    .db_remove = app_db_remove,
    .db_count = app_db_count,
    .db_table_gen = db_table_gen,
    .utf8_valid = utf8_valid,
    .utf8_cut = utf8_cut,
    .utf8_from_latin1 = utf8_from_latin1,
};
