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
    int f = fm_frequency(), vol = audio_get_volume();

    snprintf(buf, sizeof(buf), "%d.%d", f / 1000, f % 1000 / 100);
    gfx_text_centered(UI_TOP + 10, buf, fm_is_on() ? C_TEXT : C_DIM, FONT_LARGE, 3);
    gfx_text_centered(UI_TOP + 60, "MHz", C_DIM, FONT_SMALL, 1);
    if (fm_is_on()) {
        snprintf(buf, sizeof(buf), "signal %d%s%s", rssi, stereo ? "  stereo" : "",
                 muted ? "  muted" : "");
        gfx_text_centered(UI_TOP + 74, buf, C_TEXT, FONT_SMALL, 1);
        gfx_fill(14, UI_TOP + 86, rssi > 0 ? rssi * (LCD_WIDTH - 28) / 80 : 0, 3, C_OK);
    }
    snprintf(buf, sizeof(buf), "volume %d dB, %s", vol / 10,
             audio_headphones() ? "headphones" : "no antenna!");
    gfx_text_centered(UI_TOP + 94, buf, audio_headphones() ? C_DIM : C_WARN, FONT_SMALL, 1);
    if (wav_recording()) {
        snprintf(buf, sizeof(buf), "REC %lu s", (unsigned long)wav_seconds());
        gfx_text_centered(UI_TOP + 104, buf, C_BAD, FONT_SMALL, 1);
    } else if (state) {
        gfx_text_centered(UI_TOP + 104, state, C_DIM, FONT_SMALL, 1);
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
