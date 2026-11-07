/* B310E OS — Sound: test tone, play a WAV, record the microphone; volume,
 * mic gain, the output in use and the headset.
 *
 * Keys: Up/Down choose, Center starts or stops, Left/Right change the
 * value on the Volume and Mic gain rows; headset buttons: + and - volume,
 * centre starts or stops the selected row.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/audio.h"
#include "os/fs.h"
#include "os/kernel.h"
#include "os/lib.h"
#include "os/ui.h"
#include "apps/wavio.h"

#define MAX_FILES 32

enum { ROW_TONE, ROW_PLAY, ROW_LAST, ROW_REC, ROW_GAIN, ROW_VOL, ROW_OUT, ROWS };

static list_t rows;
static char files[MAX_FILES][48];
static int n_files;
static const char *error;

static void row_text(void *ctx, int i, char *text, int tn, char *detail, int dn)
{
    switch (i) {
    case ROW_TONE:
        snprintf(text, (size_t)tn, "Test tone 1 kHz");
        snprintf(detail, (size_t)dn, "%s", wav_playing() && !strcmp(wav_name(), "1000 Hz tone") ? "on" : "");
        break;
    case ROW_PLAY:
        snprintf(text, (size_t)tn, "Play a WAV file");
        break;
    case ROW_LAST:
        snprintf(text, (size_t)tn, "Play last recording");
        break;
    case ROW_REC:
        snprintf(text, (size_t)tn, "%s", wav_recording() ? "Stop recording" : "Record mic");
        if (wav_recording())
            snprintf(detail, (size_t)dn, "%lu s", (unsigned long)wav_seconds());
        break;
    case ROW_GAIN:
        snprintf(text, (size_t)tn, "Mic gain");
        snprintf(detail, (size_t)dn, "%d", audio_get_mic_gain());
        break;
    case ROW_VOL: {
        int vol = audio_get_volume();

        snprintf(text, (size_t)tn, "Volume");
        if (vol <= -1000)
            snprintf(detail, (size_t)dn, "mute");
        else
            snprintf(detail, (size_t)dn, "%s%d dB", vol < 0 && vol > -10 ? "-" : "", vol / 10);
        break;
    }
    case ROW_OUT: {
        audio_headset_info_t hs;

        audio_headset_info(&hs);
        snprintf(text, (size_t)tn, "%s", audio_speaker() ? "Speaker" : "Headphones");
        snprintf(detail, (size_t)dn, "%s", !hs.inserted ? "" : hs.buttons ? "4-pole" : "3-pole");
        break;
    }
    }
    if (i == ROW_TONE || i == ROW_PLAY || i == ROW_LAST) {
        if (wav_playing() && ((i == ROW_TONE) == !strcmp(wav_name(), "1000 Hz tone")))
            snprintf(detail, (size_t)dn, "%lu s", (unsigned long)wav_seconds());
    }
}

static const icon_t *row_icon(void *ctx, int i)
{
    static const icon_t *const icons[ROWS] = {
        &icon_music, &icon_play, &icon_play, &icon_record, &icon_mic, &icon_sun,
        &icon_vibrate,
    };

    return icons[i];
}

/* ---- file chooser ------------------------------------------------------- */
static list_t pick;

static void pick_text(void *ctx, int i, char *text, int tn, char *detail, int dn)
{
    const char *slash = strchr(files[i] + 1, '/');

    snprintf(text, (size_t)tn, "%s", slash && strchr(slash + 1, '/') ?
             strchr(slash + 1, '/') + 1 : files[i] + 1);
}

static bool is_wav(const char *n)
{
    size_t l = strlen(n);

    return l > 4 && (!strcmp(n + l - 4, ".wav") || !strcmp(n + l - 4, ".WAV"));
}

static void scan_dir(const char *dir)
{
    DIR d;
    FILINFO fi;

    if (f_opendir(&d, dir) != FR_OK)
        return;
    while (n_files < MAX_FILES && f_readdir(&d, &fi) == FR_OK && fi.fname[0])
        if (!(fi.fattrib & AM_DIR) && is_wav(fi.fname))
            snprintf(files[n_files++], sizeof(files[0]), "%s/%s",
                     dir[1] ? dir : "", fi.fname);
    f_closedir(&d);
}

static void pick_draw(screen_t *s)
{
    if (pick.count)
        list_draw(&pick);
    else
        gfx_text_centered(UI_TOP + 40, "no .wav files", C_DIM, FONT_SMALL, 1);
}

static void pick_key(screen_t *s, const key_event_t *ev)
{
    if (list_key(&pick, ev))
        return;
    if ((ev->code == KEY_CENTER || ev->code == KEY_LSOFT) && pick.count) {
        error = wav_play_file(files[pick.sel]);
        ui_pop();
    } else if (ev->code == KEY_RSOFT) {
        ui_pop();
    }
}

static screen_t picker = {
    .title = "Play", .left = "Play", .center = "OK", .right = "Back",
    .draw = pick_draw, .key = pick_key,
};

static const icon_t *pick_icon(void *ctx, int i)
{
    return &icon_music;
}

static void open_picker(void)
{
    n_files = 0;
    scan_dir("/");
    scan_dir(REC_DIR);
    pick.count = n_files;
    pick.sel = pick.top = 0;
    pick.item = pick_text;
    pick.icon = pick_icon;
    ui_push(&picker);
}

/* ---- the screen --------------------------------------------------------- */

static void draw(screen_t *s)
{
    unsigned pk = audio_peak();

    list_draw(&rows);
    if (wav_playing() || wav_recording()) {     /* a level meter by the title */
        int w = 40, x = LCD_WIDTH - UI_PAD - w;

        gfx_bar(x, UI_STATUS_H + (UI_TITLE_H - 4) / 2, w, 4, (int)pk, 32768,
                pk > 30000 ? C_BAD : wav_recording() ? C_BAD : C_OK);
    }
}

static void show_error(void)
{
    if (error)
        ui_message("Sound", error);
    error = NULL;
}

static void toggle(int row)
{
    error = NULL;
    switch (row) {
    case ROW_TONE:
        if (wav_playing())
            wav_stop();
        else
            error = wav_play_tone(1000);
        break;
    case ROW_PLAY:
        if (wav_playing())
            wav_stop();
        else
            open_picker();
        break;
    case ROW_LAST:
        if (wav_playing())
            wav_stop();
        else if (wav_last_recording()[0])
            error = wav_play_file(wav_last_recording());
        else
            error = "nothing recorded yet";
        break;
    case ROW_REC:
        if (wav_recording())
            wav_stop();
        else
            error = wav_record(16000, AUDIO_SRC_MIC);
        break;
    }
}

static void key(screen_t *s, const key_event_t *ev)
{
    int vol = audio_get_volume();

    if (list_key(&rows, ev))
        return;
    switch (ev->code) {
    case KEY_CENTER:
    case KEY_LSOFT:
    case KEY_HS_CENTER:
        toggle(rows.sel);
        show_error();
        break;
    case KEY_LEFT:
    case KEY_RIGHT:
        if (rows.sel == ROW_GAIN) {
            audio_set_mic_gain(audio_get_mic_gain() + (ev->code == KEY_RIGHT ? 1 : -1));
            break;
        }
        if (rows.sel != ROW_VOL)
            break;
        /* fall through */
    case KEY_HS_PLUS:
    case KEY_HS_MINUS:
        vol += ev->code == KEY_RIGHT || ev->code == KEY_HS_PLUS ? 30 : -30;
        if (vol < -600)
            vol = -1000;
        else if (vol < -570)
            vol = -570;
        audio_set_volume(vol);
        break;
    case KEY_RSOFT:
        ui_pop();
        break;
    }
}

static void tick(screen_t *s)
{
    wav_pump();
}

static void leave(screen_t *s)
{
    wav_stop();
}

static screen_t screen = {
    .title = "Sound", .left = "Start", .center = "OK", .right = "Back",
    .draw = draw, .key = key, .tick = tick, .leave = leave, .tick_ms = 50,
};

screen_t *app_sound(void)
{
    rows.count = ROWS;
    rows.item = row_text;
    rows.icon = row_icon;
    error = NULL;
    return &screen;
}
