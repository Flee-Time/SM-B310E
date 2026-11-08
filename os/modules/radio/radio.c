/* B310E OS — FM radio. The headset cable is the antenna; the sound goes to
 * the headphones (the speaker when none are plugged in).
 *
 * Keys: Left/Right tune 100 kHz, Up/Down seek (a channel per tick, so the
 * UI never waits; any key stops it), Center mute, * and # (or the headset's
 * - and +) its volume, 1..9 the presets, LSoft the menu: record
 * the radio to a WAV (32 kHz), the presets, save one, turn the radio off.
 * RSoft goes back and the radio plays on (a recording too).
 *
 * The last frequency and the presets are kept in /b310e-os/radio.txt.
 *
 * A loadable app (os/modules); it was built into the OS until 2026-11-07.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "app.h"
#include "icons.h"

#define OS_DIR      "/b310e-os"
#define CONF_FILE   OS_DIR "/radio.txt"
#define FM_MIN      87500
#define FM_MAX      108000
#define FM_STEP     100
#define PRESETS     9
#define REC_RATE    32000

static const char *state;
static bool muted, starting;
static int seek_dir, seek_start, seek_steps;  /* seeking: +1 / -1 */
static int rssi = -1;
static bool stereo;
static unsigned ticks_n;
static int saved_freq;                  /* from the file, 0 none */
static int presets[PRESETS];            /* kHz, 0 empty */
static char preset_text[PRESETS][20];
static const char *preset_names[PRESETS];
static const char *options[5];
enum { OPT_REC, OPT_PRESETS, OPT_SAVE, OPT_SPEAKER, OPT_OFF };

/* ---- the file -------------------------------------------------------------------- */

static int number(const char **p)
{
    int v = 0;

    while (**p >= '0' && **p <= '9')
        v = v * 10 + *(*p)++ - '0';
    return v;
}

static void conf_load(void)
{
    char buf[256];
    FIL f;
    UINT n = 0;
    const char *p;

    saved_freq = 0;
    memset(presets, 0, sizeof(presets));
    if (f_open(&f, CONF_FILE, FA_READ) != FR_OK)
        return;
    f_read(&f, buf, sizeof(buf) - 1, &n);
    f_close(&f);
    buf[n] = 0;
    for (p = buf; *p; ) {
        if (!memcmp(p, "freq=", 5)) {
            p += 5;
            saved_freq = number(&p);
        } else if (!memcmp(p, "speaker=", 8)) {
            p += 8;
            fm_set_speaker(number(&p) != 0);
        } else if (p[0] == 'p' && p[1] >= '1' && p[1] <= '9' && p[2] == '=') {
            int i = p[1] - '1';

            p += 3;
            presets[i] = number(&p);
        }
        while (*p && *p != '\n')
            p++;
        if (*p)
            p++;
    }
    if (saved_freq < FM_MIN || saved_freq > FM_MAX)
        saved_freq = 0;
    for (int i = 0; i < PRESETS; i++)
        if (presets[i] < FM_MIN || presets[i] > FM_MAX)
            presets[i] = 0;
}

static void conf_save(void)
{
    char buf[256];
    int len;
    FIL f;
    UINT n;

    if (!fm_is_on())
        return;
    saved_freq = fm_frequency();
    len = snprintf(buf, sizeof(buf), "freq=%d\nspeaker=%d\n", saved_freq,
                   fm_speaker() ? 1 : 0);
    for (int i = 0; i < PRESETS; i++)
        if (presets[i])
            len += snprintf(buf + len, sizeof(buf) - (size_t)len, "p%d=%d\n", i + 1, presets[i]);
    f_mkdir(OS_DIR);
    if (f_open(&f, CONF_FILE, FA_WRITE | FA_CREATE_ALWAYS) == FR_OK) {
        f_write(&f, buf, (UINT)len, &n);
        f_close(&f);
    }
}

static int preset_of(int khz)           /* 1..9, 0 none */
{
    for (int i = 0; i < PRESETS; i++)
        if (presets[i] == khz)
            return i + 1;
    return 0;
}

static void preset_texts(void)
{
    for (int i = 0; i < PRESETS; i++) {
        if (presets[i])
            snprintf(preset_text[i], sizeof(preset_text[i]), "%d   %d.%d MHz", i + 1,
                     presets[i] / 1000, presets[i] % 1000 / 100);
        else
            snprintf(preset_text[i], sizeof(preset_text[i]), "%d   (empty)", i + 1);
        preset_names[i] = preset_text[i];
    }
}

/* ---- the radio ------------------------------------------------------------------ */

static void labels(screen_t *s)
{
    s->center = muted ? "Unmute" : "Mute";
}

static void stop_seek(bool back)
{
    if (seek_dir && back)
        fm_tune(seek_start);
    seek_dir = 0;
}

static void tune(int khz)
{
    stop_seek(false);
    fm_tune(khz);
    rssi = fm_rssi();
    state = NULL;
}

static int vol_cat;                     /* "FM radio" in the Sound page */

static void change_volume(int d)
{
    int v = volume_level(vol_cat) + d;

    if (v < 0 || v > VOL_LEVELS)
        return;
    volume_use(vol_cat);
    volume_set(vol_cat, v);
}

static void draw(screen_t *s)
{
    char buf[32];
    int f = fm_frequency(), x = UI_PAD, w = LCD_WIDTH - 2 * UI_PAD, p = preset_of(f);
    int vol = volume_level(vol_cat);

    labels(s);
    snprintf(buf, sizeof(buf), "%d.%d", f / 1000, f % 1000 / 100);
    gfx_text_centered(UI_TOP + 10, buf, fm_is_on() ? C_TEXT : C_DIM, FONT_HUGE, 1);
    if (p) {
        snprintf(buf, sizeof(buf), "MHz, preset %d", p);
        gfx_text_centered(UI_TOP + 34, buf, C_ACCENT, FONT_SMALL, 1);
    } else {
        gfx_text_centered(UI_TOP + 34, "MHz", C_DIM, FONT_SMALL, 1);
    }

    gfx_round(x, UI_TOP + 46, w, 40, UI_RADIUS, C_CARD);
    snprintf(buf, sizeof(buf), "signal %d", rssi < 0 ? 0 : rssi);
    gfx_text(x + 5, UI_TOP + 51, buf, C_TEXT, FONT_SMALL, 1, 0);
    if (fm_is_on()) {
        const char *tag = muted ? "muted" : stereo ? "stereo" : "mono";

        gfx_text(x + w - 5 - gfx_text_width(tag, FONT_SMALL, 1), UI_TOP + 51, tag,
                 muted ? C_WARN : C_ACCENT, FONT_SMALL, 1, 0);
    }
    gfx_bar(x + 5, UI_TOP + 62, w - 10, 3, rssi > 0 ? rssi : 0, 80, C_OK);
    if (vol)
        snprintf(buf, sizeof(buf), "volume %d", vol);
    else
        snprintf(buf, sizeof(buf), "volume off");
    gfx_text(x + 5, UI_TOP + 71, buf, C_DIM, FONT_SMALL, 1, 0);
    if (!audio_headphones())
        gfx_text(x + w - 5 - gfx_text_width("no antenna", FONT_SMALL, 1), UI_TOP + 71,
                 "no antenna", C_WARN, FONT_SMALL, 1, 0);
    else if (fm_speaker())
        gfx_text(x + w - 5 - gfx_text_width("speaker", FONT_SMALL, 1), UI_TOP + 71,
                 "speaker", C_ACCENT, FONT_SMALL, 1, 0);

    if (wav_recording()) {
        snprintf(buf, sizeof(buf), "REC %lu s", (unsigned long)wav_seconds());
        gfx_round((LCD_WIDTH - gfx_text_width(buf, FONT_BODY, 1)) / 2 - 9, UI_TOP + 97, 5, 5, 2,
                  C_BAD);
        gfx_text_centered(UI_TOP + 95, buf, C_BAD, FONT_BODY, 1);
    } else if (state) {
        gfx_text_centered(UI_TOP + 96, state, C_DIM, FONT_SMALL, 1);
    } else if (fm_is_on()) {
        gfx_text_centered(UI_TOP + 96, "Back: it plays on", C_DIM, FONT_SMALL, 1);
    }
}

static void tick(screen_t *s)
{
    if (starting) {
        bool was_on = fm_is_on();

        starting = false;
        volume_use(vol_cat);
        state = !fm_present() ? "no tuner found" : fm_on() ? "tuner did not start" : NULL;
        if (!state && !was_on && saved_freq)
            fm_tune(saved_freq);
        if (!state) {
            rssi = fm_rssi();
            stereo = fm_stereo();
        }
        return;
    }
    if (seek_dir && fm_is_on()) {               /* one channel per tick */
        int f = fm_frequency() + seek_dir * FM_STEP;

        fm_tune(f > FM_MAX ? FM_MIN : f < FM_MIN ? FM_MAX : f);
        rssi = fm_rssi();
        if (fm_station(rssi)) {
            seek_dir = 0;
            state = NULL;
            conf_save();
        } else if (++seek_steps > (FM_MAX - FM_MIN) / FM_STEP) {
            stop_seek(true);
            state = "no station found";
        }
        return;
    }
    if (fm_is_on() && ++ticks_n % 10 == 0) {   /* twice a second */
        rssi = fm_rssi();
        stereo = fm_stereo();
    }
}

static void preset_picked(int i, void *ctx)
{
    if (!presets[i]) {
        ui_message("Presets", "Nothing saved there yet: Menu, Save preset.");
        return;
    }
    tune(presets[i]);
    conf_save();
}

static void save_picked(int i, void *ctx)
{
    presets[i] = fm_frequency();
    klog("radio: preset %d = %d kHz", i + 1, presets[i]);
    conf_save();
}

static const icon_t app_icon;
static notify_t note;

static bool note_update(notify_t *n)
{
    int f = fm_frequency(), p;

    if (!fm_is_on())
        return false;
    p = preset_of(f);
    if (p)
        snprintf(n->text, sizeof(n->text), "%d.%d MHz, preset %d", f / 1000, f % 1000 / 100, p);
    else
        snprintf(n->text, sizeof(n->text), "%d.%d MHz", f / 1000, f % 1000 / 100);
    n->action[0] = muted ? "Unmute" : "Mute";
    return true;
}

static void off_now(void);

static void note_act(notify_t *n, int a)
{
    if (a == 0) {
        muted = !muted;
        fm_mute(muted);
    } else {
        off_now();
    }
}

static void note_ensure(void)
{
    for (int i = 0; i < notify_count(); i++)
        if (notify_get(i) == &note)
            return;
    if (!fm_is_on())
        return;
    note.icon = &app_icon;
    snprintf(note.title, sizeof(note.title), "FM radio");
    note.action[1] = "Turn off";
    note.act = note_act;
    note.update = note_update;
    note_update(&note);
    notify_post(&note);
}

/* off, with no screen of the radio's open (the notification's button) */
static void off_now(void)
{
    stop_seek(true);
    conf_save();
    if (wav_recording())
        wav_stop();
    fm_off();
    notify_remove(&note);
    klog("radio: turned off");
}

static void turn_off(void)
{
    off_now();
    ui_pop();
}

static void option_picked(int i, void *ctx)
{
    const char *err;
    int p = preset_of(fm_frequency());

    switch (i) {
    case OPT_REC:
        if (wav_recording()) {
            wav_stop();
        } else if (fm_is_on()) {
            if ((err = wav_record(REC_RATE, AUDIO_SRC_FM)))
                ui_message("Record", err);
        } else {
            ui_message("Record", "The radio is off.");
        }
        break;
    case OPT_PRESETS:
        preset_texts();
        ui_choose("Presets", preset_names, PRESETS, p - 1, preset_picked, NULL);
        break;
    case OPT_SAVE:
        if (!fm_is_on())
            break;
        preset_texts();
        ui_choose("Save to", preset_names, PRESETS, p - 1, save_picked, NULL);
        break;
    case OPT_SPEAKER:                   /* the headset stays in: the antenna */
        fm_set_speaker(!fm_speaker());
        klog("radio: %s", fm_speaker() ? "on the speaker" : "on the headphones");
        conf_save();
        break;
    case OPT_OFF:
        turn_off();
        break;
    }
}

static void open_options(void)
{
    options[OPT_REC] = wav_recording() ? "Stop recording" : "Record";
    options[OPT_PRESETS] = "Presets";
    options[OPT_SAVE] = "Save preset";
    options[OPT_SPEAKER] = fm_speaker() ? "Use headphones" : "Use speaker";
    options[OPT_OFF] = "Turn off";
    ui_choose("Options", options, 5, -1, option_picked, NULL);
}

static void key(screen_t *s, const key_event_t *ev)
{
    if (seek_dir) {                     /* any key stops a seek */
        seek_dir = 0;
        state = NULL;
        conf_save();
        if (ev->code == KEY_UP || ev->code == KEY_DOWN)
            return;
    }
    switch (ev->code) {
    case KEY_LEFT:
    case KEY_RIGHT:
        if (fm_is_on()) {
            tune(fm_frequency() + (ev->code == KEY_RIGHT ? FM_STEP : -FM_STEP));
            conf_save();
        }
        break;
    case KEY_UP:
    case KEY_DOWN:
        if (fm_is_on()) {
            seek_dir = ev->code == KEY_UP ? 1 : -1;
            seek_start = fm_frequency();
            seek_steps = 0;
            state = "seeking (any key stops)";
        }
        break;
    case KEY_CENTER:
    case KEY_HS_CENTER:
        if (fm_is_on()) {
            muted = !muted;
            fm_mute(muted);
        }
        break;
    case KEY_STAR:
    case KEY_HS_MINUS:
        change_volume(-1);
        break;
    case KEY_HASH:
    case KEY_HS_PLUS:
        change_volume(1);
        break;
    case KEY_1: case KEY_2: case KEY_3: case KEY_4: case KEY_5:
    case KEY_6: case KEY_7: case KEY_8: case KEY_9:
        if (fm_is_on() && presets[ev->code - KEY_1]) {
            tune(presets[ev->code - KEY_1]);
            conf_save();
        }
        break;
    case KEY_LSOFT:
        open_options();
        return;
    case KEY_RSOFT:
        ui_pop();                       /* the radio plays on */
        return;
    }
    if (fm_is_on())
        rssi = fm_rssi();
    labels(s);
}

static void leave(screen_t *s)
{
    stop_seek(true);
    conf_save();
    note_ensure();                      /* it plays on: its buttons stay */
}

static screen_t screen = {
    .title = "FM radio", .left = "Menu", .center = "Mute", .right = "Back",
    .leave = leave, .draw = draw, .key = key, .tick = tick, .tick_ms = 50,
};

static screen_t *start(void)
{
    conf_load();
    vol_cat = volume_register("FM radio");
    seek_dir = 0;
    muted = false;
    rssi = -1;
    ticks_n = 0;
    starting = true;
    state = fm_is_on() ? NULL : "starting";
    if (fm_is_on())
        fm_mute(false);                 /* the screen starts unmuted */
    labels(&screen);
    return &screen;
}

static const icon_t app_icon = APP_ICON_RADIO;  /* declared above */
APP("FM radio", start, NULL, &app_icon);
