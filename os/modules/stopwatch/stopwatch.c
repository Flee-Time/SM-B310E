/* Stopwatch — the first loadable B310E OS app (an example for os/sdk).
 * Center starts and stops, LSoft takes a lap while running and resets
 * while stopped, RSoft closes. Shows tenths; keeps the last laps.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "app.h"

#define MAX_LAPS 6

static struct {
    bool running;
    uint32_t started;                   /* ticks() when started */
    uint32_t banked;                    /* ms before the last start */
    uint32_t laps[MAX_LAPS];            /* newest first */
    int n_laps, lap_no;
} sw;

static uint32_t elapsed(void)
{
    return sw.banked + (sw.running ? ticks() - sw.started : 0);
}

static void format(uint32_t ms, char *buf, size_t n)
{
    uint32_t t = ms / 100;

    if (t >= 36000)
        snprintf(buf, n, "%lu:%02lu:%02lu.%lu", (unsigned long)(t / 36000),
                 (unsigned long)(t / 600 % 60), (unsigned long)(t / 10 % 60),
                 (unsigned long)(t % 10));
    else
        snprintf(buf, n, "%02lu:%02lu.%lu", (unsigned long)(t / 600),
                 (unsigned long)(t / 10 % 60), (unsigned long)(t % 10));
}

static void labels(screen_t *s)
{
    s->center = sw.running ? "Stop" : "Start";
    s->left = sw.running ? "Lap" : (elapsed() ? "Reset" : NULL);
}

static void draw(screen_t *s)
{
    char buf[24];

    format(elapsed(), buf, sizeof(buf));
    gfx_text_centered(UI_TOP + 14, buf, sw.running ? C_TEXT : C_ACCENT,
                      FONT_LARGE, 2);
    for (int i = 0; i < sw.n_laps; i++) {
        char t[16];
        int y = UI_TOP + 56 + i * 11;

        format(sw.laps[i], t, sizeof(t));
        snprintf(buf, sizeof(buf), "lap %d", sw.lap_no - i);
        gfx_text(16, y, buf, C_DIM, FONT_SMALL, 1, 0);
        gfx_text(LCD_WIDTH - 16 - gfx_text_width(t, FONT_SMALL, 1), y, t,
                 C_TEXT, FONT_SMALL, 1, 0);
    }
}

static void key(screen_t *s, const key_event_t *ev)
{
    if (ev->code == KEY_CENTER) {
        if (sw.running)
            sw.banked = elapsed();
        else
            sw.started = ticks();
        sw.running = !sw.running;
    } else if (ev->code == KEY_LSOFT) {
        if (sw.running) {
            for (int i = MAX_LAPS - 1; i > 0; i--)
                sw.laps[i] = sw.laps[i - 1];
            sw.laps[0] = elapsed();
            if (sw.n_laps < MAX_LAPS)
                sw.n_laps++;
            sw.lap_no++;
        } else {
            memset(&sw, 0, sizeof(sw));
        }
    } else if (ev->code == KEY_RSOFT) {
        ui_pop();
        return;
    }
    labels(s);
}

static void tick(screen_t *s)
{
    /* redrawn by the UI after every tick; nothing else to do */
}

static screen_t screen = {
    .title = "Stopwatch", .right = "Close",
    .draw = draw, .key = key, .tick = tick, .tick_ms = 100,
};

static screen_t *start(void)
{
    memset(&sw, 0, sizeof(sw));
    labels(&screen);
    klog("stopwatch: started");
    return &screen;
}

APP("Stopwatch", start, NULL);
