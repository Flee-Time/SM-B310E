/* B310E OS — FM radio. The headset cable is the antenna; the sound goes to
 * the headphones (the speaker when none are plugged in).
 *
 * Keys: Left/Right tune 100 kHz, Up/Down seek (a channel per tick, any key
 * stops it), Center mute, * and # (or
 * the headset's - and +) volume, LSoft records the radio to a WAV,
 * RSoft turns the radio off and goes back.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/audio.h"
#include "os/fs.h"
#include "os/kernel.h"
#include "os/lib.h"
#include "os/ui.h"
#include "apps/wavio.h"

static const char *state;
static bool muted;
static int seek_dir, seek_start, seek_steps;  /* seeking: +1 / -1 */
static int rssi = -1;
static bool stereo;

static void draw(screen_t *s)
{
    char buf[32];
    int f = fm_frequency(), vol = audio_get_volume(), x = UI_PAD, w = LCD_WIDTH - 2 * UI_PAD;

    snprintf(buf, sizeof(buf), "%d.%d", f / 1000, f % 1000 / 100);
    gfx_text_centered(UI_TOP + 10, buf, fm_is_on() ? C_TEXT : C_DIM, FONT_HUGE, 1);
    gfx_text_centered(UI_TOP + 34, "MHz", C_DIM, FONT_SMALL, 1);

    gfx_round(x, UI_TOP + 48, w, 36, UI_RADIUS, C_CARD);
    snprintf(buf, sizeof(buf), "signal %d", rssi < 0 ? 0 : rssi);
    gfx_text(x + 5, UI_TOP + 53, buf, C_TEXT, FONT_SMALL, 1, 0);
    if (fm_is_on()) {
        const char *tag = muted ? "muted" : stereo ? "stereo" : "mono";

        gfx_text(x + w - 5 - gfx_text_width(tag, FONT_SMALL, 1), UI_TOP + 53, tag,
                 muted ? C_WARN : C_ACCENT, FONT_SMALL, 1, 0);
    }
    gfx_bar(x + 5, UI_TOP + 64, w - 10, 3, rssi > 0 ? rssi : 0, 80, C_OK);
    snprintf(buf, sizeof(buf), "volume %d dB", vol / 10);
    gfx_text(x + 5, UI_TOP + 72, buf, C_DIM, FONT_SMALL, 1, 0);
    if (!audio_headphones())
        gfx_text(x + w - 5 - gfx_text_width("no antenna", FONT_SMALL, 1), UI_TOP + 72,
                 "no antenna", C_WARN, FONT_SMALL, 1, 0);

    if (wav_recording()) {
        snprintf(buf, sizeof(buf), "REC %lu s", (unsigned long)wav_seconds());
        gfx_fill((LCD_WIDTH - gfx_text_width(buf, FONT_BODY, 1)) / 2 - 8, UI_TOP + 97, 5, 5, C_BAD);
        gfx_text_centered(UI_TOP + 95, buf, C_BAD, FONT_BODY, 1);
    } else if (state) {
        gfx_text_centered(UI_TOP + 95, state, C_DIM, FONT_SMALL, 1);
    }
}

static void enter(screen_t *s)
{
    seek_dir = 0;
    muted = false;
    rssi = -1;
    state = "starting";
    ui_redraw();
}

static void tick(screen_t *s)
{
    static unsigned n;

    if (state && !strcmp(state, "starting")) {
        state = !fm_present() ? "no tuner found" : fm_on() ? "tuner did not start" : NULL;
        return;
    }
    wav_pump();
    if (seek_dir && fm_is_on()) {               /* one channel per tick */
        int f = fm_frequency() + seek_dir * 100;

        fm_tune(f > 108000 ? 87500 : f < 87500 ? 108000 : f);
        rssi = fm_rssi();
        if (fm_station(rssi)) {
            seek_dir = 0;
            state = NULL;
        } else if (++seek_steps > (108000 - 87500) / 100) {
            seek_dir = 0;
            fm_tune(seek_start);
            state = "no station found";
        }
        return;
    }
    if (fm_is_on() && ++n % 10 == 0) {          /* twice a second */
        rssi = fm_rssi();
        stereo = fm_stereo();
    }
}

static void key(screen_t *s, const key_event_t *ev)
{
    int vol = audio_get_volume();

    if (seek_dir) {                     /* any key stops a seek */
        seek_dir = 0;
        state = NULL;
        if (ev->code == KEY_UP || ev->code == KEY_DOWN)
            return;
    }
    switch (ev->code) {
    case KEY_LEFT:
    case KEY_RIGHT:
        fm_tune(fm_frequency() + (ev->code == KEY_RIGHT ? 100 : -100));
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
        muted = !muted;
        fm_mute(muted);
        break;
    case KEY_STAR:
    case KEY_HASH:
    case KEY_HS_PLUS:
    case KEY_HS_MINUS:
        vol += ev->code == KEY_HASH || ev->code == KEY_HS_PLUS ? 30 : -30;
        audio_set_volume(vol < -600 ? -600 : vol);
        break;
    case KEY_LSOFT:
        if (wav_recording())
            wav_stop();
        else if (fm_is_on())
            state = wav_record(32000, AUDIO_SRC_FM);
        s->left = wav_recording() ? "Stop" : "Rec";
        break;
    case KEY_RSOFT:
        ui_pop();
        return;
    }
    if (fm_is_on())
        rssi = fm_rssi();
}

static void leave(screen_t *s)
{
    wav_stop();
    fm_off();
}

static screen_t screen = {
    .title = "FM radio", .left = "Rec", .center = "Mute", .right = "Back",
    .enter = enter, .leave = leave, .draw = draw, .key = key, .tick = tick,
    .tick_ms = 50,
};

screen_t *app_radio(void)
{
    screen.left = "Rec";
    return &screen;
}
