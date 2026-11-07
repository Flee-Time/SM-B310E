/* B310E OS — Recorder: the microphone to /b310e-os/rec/RECnnnn.WAV (16 kHz
 * mono), the elapsed time and a level meter, the mic gain, and the list of
 * recordings to play (at the apps' volume) or delete.
 *
 * The OS keeps the recording going (its UI loop writes the file), so it
 * goes on when the app is left; coming back shows it still running.
 *
 * Keys: Center (or the headset's button) records and stops, Left/Right the
 * mic gain, LSoft the recordings, RSoft back. In the list: Center plays or
 * stops, LSoft deletes.
 *
 * A loadable app (os/modules); it was part of the built-in Sound screen
 * until 2026-11-07.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "app.h"
#include "icons.h"

#define OS_DIR     "/b310e-os"
#define REC_DIR    OS_DIR "/rec"
#define CONF_FILE  OS_DIR "/recorder.txt"
#define REC_RATE   16000
#define MAX_RECS   64
#define GAIN_MAX   15

typedef struct {
    char name[40];
    uint32_t seconds;
} rec_t;

static rec_t *recs;
static int n_recs;
static list_t rlist;
static bool resumed;                    /* recording already when the app opened */
static unsigned level;                  /* the meter, with a slow fall */
static char playing_path[64];           /* the file we play, "" none */
static char to_delete[64];

static void path_of(const char *name, char *out, int n)
{
    snprintf(out, (size_t)n, REC_DIR "/%s", name);
}

static const char *base_name(const char *path)
{
    const char *s = strrchr(path, '/');

    return s ? s + 1 : path;
}

static bool ends_wav(const char *n)
{
    size_t l = strlen(n);

    return l > 4 && n[l - 4] == '.' && (n[l - 3] | 0x20) == 'w' && (n[l - 2] | 0x20) == 'a' &&
           (n[l - 1] | 0x20) == 'v';
}

static int rec_number(const char *n)            /* RECnnnn.WAV: nnnn, else 0 */
{
    int v = 0;

    if (strlen(n) != 11 || memcmp(n, "REC", 3) || strcmp(n + 7, ".WAV"))
        return 0;
    for (int i = 3; i < 7; i++) {
        if (n[i] < '0' || n[i] > '9')
            return 0;
        v = v * 10 + n[i] - '0';
    }
    return v;
}

/* newest recordings first, then other WAVs by name */
static bool before(const char *a, const char *b)
{
    int na = rec_number(a), nb = rec_number(b);

    if (na || nb)
        return na > nb;
    return strcmp(a, b) < 0;
}

static uint32_t le32(const uint8_t *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint32_t wav_length(const char *path, uint32_t size)
{
    uint8_t h[44];
    FIL f;
    UINT n = 0;
    uint32_t per_s;

    if (f_open(&f, path, FA_READ) != FR_OK)
        return 0;
    f_read(&f, h, sizeof(h), &n);
    f_close(&f);
    if (n < 44 || memcmp(h, "RIFF", 4) || memcmp(h + 12, "fmt ", 4))
        return 0;
    per_s = le32(h + 28);               /* bytes a second */
    return per_s ? (size > 44 ? size - 44 : 0) / per_s : 0;
}

static void load(void)
{
    DIR d;
    FILINFO fi;

    n_recs = 0;
    if (!recs)
        recs = malloc(sizeof(*recs) * MAX_RECS);
    if (recs && f_opendir(&d, REC_DIR) == FR_OK) {
        while (n_recs < MAX_RECS && f_readdir(&d, &fi) == FR_OK && fi.fname[0]) {
            int at;
            char path[64];

            if ((fi.fattrib & AM_DIR) || !ends_wav(fi.fname) ||
                strlen(fi.fname) >= sizeof(recs->name))
                continue;
            for (at = 0; at < n_recs && before(recs[at].name, fi.fname); at++)
                ;
            memmove(&recs[at + 1], &recs[at], sizeof(*recs) * (size_t)(n_recs - at));
            snprintf(recs[at].name, sizeof(recs->name), "%s", fi.fname);
            path_of(fi.fname, path, sizeof(path));
            recs[at].seconds = wav_length(path, (uint32_t)fi.fsize);
            n_recs++;
        }
        f_closedir(&d);
    }
    rlist.count = n_recs;
    if (rlist.sel >= n_recs)
        rlist.sel = n_recs ? n_recs - 1 : 0;
}

static void save_gain(void)
{
    char buf[16];
    FIL f;
    UINT n;
    int len = snprintf(buf, sizeof(buf), "gain=%d\n", audio_get_mic_gain());

    f_mkdir(OS_DIR);
    if (f_open(&f, CONF_FILE, FA_WRITE | FA_CREATE_ALWAYS) == FR_OK) {
        f_write(&f, buf, (UINT)len, &n);
        f_close(&f);
    }
}

static void load_gain(void)
{
    char buf[16];
    FIL f;
    UINT n = 0;
    int v = 0;

    if (f_open(&f, CONF_FILE, FA_READ) != FR_OK)
        return;
    f_read(&f, buf, sizeof(buf) - 1, &n);
    f_close(&f);
    buf[n] = 0;
    if (memcmp(buf, "gain=", 5))
        return;
    for (const char *p = buf + 5; *p >= '0' && *p <= '9'; p++)
        v = v * 10 + *p - '0';
    if (v <= GAIN_MAX)
        audio_set_mic_gain(v);
}

static bool we_play(void)
{
    return playing_path[0] && wav_playing() && !strcmp(wav_name(), playing_path);
}

static void stop_playing(void)
{
    if (we_play())
        wav_stop();
    playing_path[0] = 0;
}

static void clock_text(uint32_t s, char *buf, int n)
{
    if (s >= 3600)
        snprintf(buf, (size_t)n, "%lu:%02lu:%02lu", (unsigned long)(s / 3600),
                 (unsigned long)(s / 60 % 60), (unsigned long)(s % 60));
    else
        snprintf(buf, (size_t)n, "%02lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
}

/* ---- the recordings ----------------------------------------------------------- */

static void rec_item(void *ctx, int i, char *text, int tn, char *detail, int dn)
{
    char path[64];

    snprintf(text, (size_t)tn, "%s", recs[i].name);
    path_of(recs[i].name, path, sizeof(path));
    if (wav_recording() && !strcmp(wav_name(), path))
        snprintf(detail, (size_t)dn, "rec");
    else if (we_play() && !strcmp(playing_path, path))
        snprintf(detail, (size_t)dn, "%lu s", (unsigned long)wav_seconds());
    else
        snprintf(detail, (size_t)dn, "%lu:%02lu", (unsigned long)(recs[i].seconds / 60),
                 (unsigned long)(recs[i].seconds % 60));
}

static const icon_t *rec_icon(void *ctx, int i)
{
    char path[64];

    path_of(recs[i].name, path, sizeof(path));
    if (wav_recording() && !strcmp(wav_name(), path))
        return ui_icon("record");
    return we_play() && !strcmp(playing_path, path) ? ui_icon("play") : ui_icon("mic");
}

static void list_labels(screen_t *s)
{
    char path[64];

    s->center = "Play";
    if (n_recs) {
        path_of(recs[rlist.sel].name, path, sizeof(path));
        if (we_play() && !strcmp(playing_path, path))
            s->center = "Stop";
    }
    s->left = n_recs ? "Delete" : NULL;
}

static void list_draw_(screen_t *s)
{
    if (n_recs)
        list_draw(&rlist);
    else
        gfx_text_centered(UI_TOP + 46, "No recordings yet", C_DIM, FONT_BODY, 1);
    list_labels(s);
}

static void delete_yes(void)
{
    if (we_play() && !strcmp(playing_path, to_delete))
        stop_playing();
    if (f_unlink(to_delete) == FR_OK)
        klog("recorder: deleted %s", to_delete);
    else
        ui_message("Recorder", "Could not delete the file.");
    load();
}

static void list_key_(screen_t *s, const key_event_t *ev)
{
    char path[64];
    const char *err;

    if (list_key(&rlist, ev))
        return;
    if (ev->code == KEY_RSOFT) {
        ui_pop();
        return;
    }
    if (!n_recs)
        return;
    path_of(recs[rlist.sel].name, path, sizeof(path));
    if (ev->code == KEY_CENTER || ev->code == KEY_HS_CENTER) {
        if (we_play() && !strcmp(playing_path, path)) {
            stop_playing();
        } else if (wav_recording()) {
            ui_message("Recorder", "Stop the recording first.");
        } else if ((err = wav_play_file(path))) {
            playing_path[0] = 0;
            ui_message(recs[rlist.sel].name, err);
        } else {
            snprintf(playing_path, sizeof(playing_path), "%s", path);
        }
    } else if (ev->code == KEY_LSOFT) {
        char msg[64];

        if (wav_recording() && !strcmp(wav_name(), path)) {
            ui_message("Recorder", "This one is still recording.");
            return;
        }
        snprintf(to_delete, sizeof(to_delete), "%s", path);
        snprintf(msg, sizeof(msg), "Delete %s?", recs[rlist.sel].name);
        ui_confirm("Delete", msg, delete_yes);
    }
}

static void list_enter(screen_t *s)
{
    load();
}

static void list_tick(screen_t *s)
{
    if (playing_path[0] && !wav_playing())
        playing_path[0] = 0;            /* played to the end */
}

static screen_t list_screen = {
    .title = "Recordings", .left = "Delete", .center = "Play", .right = "Back",
    .enter = list_enter, .draw = list_draw_, .key = list_key_, .tick = list_tick,
    .tick_ms = 250,
};

/* ---- the recorder ------------------------------------------------------------- */

static void labels(screen_t *s)
{
    s->center = wav_recording() ? "Stop" : "Rec";
}

static void draw(screen_t *s)
{
    char buf[48];
    int x = UI_PAD, w = LCD_WIDTH - 2 * UI_PAD, y;
    bool rec = wav_recording(), play = we_play();
    int gain = audio_get_mic_gain();

    labels(s);
    /* the state */
    y = UI_TOP + 6;
    if (rec) {
        snprintf(buf, sizeof(buf), "REC  %s", base_name(wav_name()));
        int tw = gfx_text_width(buf, FONT_SMALL, 1), tx = (LCD_WIDTH - tw) / 2 + 4;

        gfx_round(tx - 9, y, 5, 5, 2, (ticks() / 500) % 2 ? C_CARD_HI : C_BAD);
        gfx_text(tx, y, buf, C_BAD, FONT_SMALL, 1, 0);
    } else if (play) {
        snprintf(buf, sizeof(buf), "Playing %s", base_name(playing_path));
        gfx_text_centered(y, buf, C_OK, FONT_SMALL, 1);
    } else {
        gfx_text_centered(y, "Ready", C_DIM, FONT_SMALL, 1);
    }
    /* the time */
    clock_text(rec || play ? wav_seconds() : 0, buf, sizeof(buf));
    gfx_text_centered(UI_TOP + 20, buf, rec ? C_TEXT : C_DIM, FONT_HUGE, 1);
    /* the level */
    y = UI_TOP + 52;
    gfx_bar(x + 6, y, w - 12, 5, rec || play ? (int)level : 0, 32768,
            level > 30000 ? C_BAD : rec ? C_ACCENT : C_OK);
    /* the mic gain */
    y = UI_TOP + 63;
    gfx_round(x, y, w, 26, UI_RADIUS, C_CARD);
    snprintf(buf, sizeof(buf), "%d", gain);
    ui_kv(x + 6, y + 4, w - 12, "Mic gain  < >", buf, C_TEXT);
    gfx_bar(x + 6, y + 16, w - 12, 3, gain, GAIN_MAX, C_ACCENT);
    /* a note */
    y = UI_TOP + 96;
    if (rec && resumed) {
        gfx_text_centered(y, "Recorded while away", C_ACCENT, FONT_SMALL, 1);
    } else if (rec && wav_dropped()) {
        snprintf(buf, sizeof(buf), "%lu samples lost", (unsigned long)wav_dropped());
        gfx_text_centered(y, buf, C_WARN, FONT_SMALL, 1);
    } else if (!card_mounted()) {
        gfx_text_centered(y, "no card", C_BAD, FONT_SMALL, 1);
    } else {
        snprintf(buf, sizeof(buf), rec ? "Leave: it goes on" : "%d recording%s", n_recs,
                 n_recs == 1 ? "" : "s");
        gfx_text_centered(y, buf, C_DIM, FONT_SMALL, 1);
    }
}

static void key(screen_t *s, const key_event_t *ev)
{
    const char *err;

    switch (ev->code) {
    case KEY_CENTER:
    case KEY_HS_CENTER:
        if (wav_recording()) {
            wav_stop();
            resumed = false;
            load();
        } else {
            stop_playing();
            if ((err = wav_record(REC_RATE, AUDIO_SRC_MIC)))
                ui_message("Recorder", err);
            resumed = false;
            level = 0;
        }
        break;
    case KEY_LEFT:
    case KEY_RIGHT: {
        int g = audio_get_mic_gain() + (ev->code == KEY_RIGHT ? 1 : -1);

        if (g >= 0 && g <= GAIN_MAX) {
            audio_set_mic_gain(g);
            save_gain();
        }
        break;
    }
    case KEY_LSOFT:
        ui_push(&list_screen);
        break;
    case KEY_RSOFT:
        ui_pop();
        return;
    }
    labels(s);
}

static void tick(screen_t *s)
{
    unsigned p = audio_peak();

    if (wav_recording() || we_play())
        level = p > level ? p : level * 3 / 4;
    else
        level = 0;
    if (playing_path[0] && !wav_playing())
        playing_path[0] = 0;
}

static void enter(screen_t *s)
{
    load();
}

static screen_t screen = {
    .title = "Recorder", .left = "List", .center = "Rec", .right = "Back",
    .enter = enter, .draw = draw, .key = key, .tick = tick, .tick_ms = 100,
};

static screen_t *start(void)
{
    resumed = wav_recording();
    level = 0;
    playing_path[0] = 0;
    rlist.sel = rlist.top = 0;
    rlist.item = rec_item;
    rlist.icon = rec_icon;
    load_gain();
    if (resumed)
        klog("recorder: back, %s still recording", wav_name());
    return &screen;
}

/* Leaving stops what we play; a recording goes on. */
static void stop(void)
{
    stop_playing();
    free(recs);
    recs = NULL;
}

static const icon_t app_icon = APP_ICON_MIC;
APP("Recorder", start, stop, &app_icon);
