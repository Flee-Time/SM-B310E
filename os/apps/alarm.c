/* B310E OS — Alarm clock.
 *
 * One daily alarm, kept in the settings (alarm_on, alarm_min). The UI
 * thread calls alarm_check() once a second with the RTC's time; when the
 * minute comes the screen wakes and the alarm_ringing screen beeps (a tone
 * through the speaker or headphones, 0.4 s on, 0.4 s off) and buzzes.
 * Snooze (LSoft) rings again in 5 minutes; Stop (Center or RSoft) or END
 * ends it; it stops by itself after 3 minutes.
 *
 * It rings only while the OS runs (the screen dark is fine). Powering up
 * the phone for it would take the RTC's alarm registers (+0x20..) and the
 * power-on path, which are not worked out yet.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/drivers.h"
#include "os/fs.h"
#include "os/kernel.h"
#include "os/lib.h"
#include "os/ui.h"
#include "wavio.h"

#define SNOOZE_MIN   5
#define RING_MS      (3 * 60 * 1000)
#define BEEP_MS      400
#define BEEP_HZ      2000

static int alarm_snooze_min = -1;             /* minute of day to ring again, -1 none */
static int fired_min = -1;              /* the minute that rang (once each) */
static bool alarm_ringing;

/* ---- the alarm_ringing screen ------------------------------------------------- */

static uint32_t ring_start;
static int ring_min;                    /* the minute it rang at */
static bool beep_on;

static void ring_stop(void)
{
    if (beep_on) {
        wav_stop();
        vibrator_set(0);
        beep_on = false;
    }
}

static void ring_tick(screen_t *s)
{
    uint32_t t = kernel_ticks() - ring_start;
    bool want = (t / BEEP_MS) % 2 == 0;

    if (t >= RING_MS) {
        ui_pop();                       /* leave() stops it */
        return;
    }
    if (want == beep_on)
        return;
    beep_on = want;
    if (want) {
        wav_play_tone(BEEP_HZ);
        vibrator_set(5);
    } else {
        wav_stop();
        vibrator_set(0);
    }
}

static void ring_draw(screen_t *s)
{
    char buf[8];

    snprintf(buf, sizeof(buf), "%02d:%02d", ring_min / 60, ring_min % 60);
    gfx_text_centered(UI_TOP + 24, buf, C_ACCENT, FONT_HUGE, 1);
    gfx_text_centered(UI_TOP + 62, "Alarm", C_TEXT, FONT_LARGE, 1);
    gfx_text_centered(UI_TOP + 80, "Snooze: 5 minutes", C_DIM, FONT_SMALL, 1);
}

static void ring_key(screen_t *s, const key_event_t *ev)
{
    if (ev->code == KEY_LSOFT) {
        int h, m, sec;

        rtc_time(&h, &m, &sec);
        alarm_snooze_min = (h * 60 + m + SNOOZE_MIN) % (24 * 60);
        ui_pop();
    } else if (ev->code == KEY_CENTER || ev->code == KEY_RSOFT) {
        alarm_snooze_min = -1;
        ui_pop();
    }
}

static void ring_leave(screen_t *s)
{
    ring_stop();
    alarm_ringing = false;
}

static screen_t ring_screen = {
    .title = "Alarm", .left = "Snooze", .center = "Stop", .right = "Stop",
    .draw = ring_draw, .key = ring_key, .tick = ring_tick, .leave = ring_leave,
    .tick_ms = 100,
};

void alarm_check(int h, int m)
{
    int now = h * 60 + m;

    if (now != fired_min)
        fired_min = -1;
    if (alarm_ringing || fired_min == now)
        return;
    if ((ui_settings.alarm_on && now == ui_settings.alarm_min) || now == alarm_snooze_min) {
        fired_min = now;
        if (now == alarm_snooze_min)
            alarm_snooze_min = -1;
        alarm_ringing = true;
        ring_min = now;
        ring_start = kernel_ticks();
        beep_on = false;
        klog("alarm: ringing at %02d:%02d", h, m);
        ui_wake();
        ui_push(&ring_screen);
    }
}

/* ---- setting it --------------------------------------------------------- */

enum { R_ON, R_TIME, R_COUNT };

static list_t list;
static bool editing;
static int edit_min, field;             /* field 0 hours, 1 minutes */

static void item(void *ctx, int i, char *text, int tn, char *detail, int dn)
{
    if (i == R_ON) {
        snprintf(text, (size_t)tn, "Alarm");
    } else {
        snprintf(text, (size_t)tn, "Time");
        snprintf(detail, (size_t)dn, "%02d:%02d", ui_settings.alarm_min / 60,
                 ui_settings.alarm_min % 60);
    }
}

static const icon_t *item_icon(void *ctx, int i)
{
    return i == R_ON ? &icon_alarm : &icon_clock;
}

static int item_toggle(void *ctx, int i)
{
    return i == R_ON ? ui_settings.alarm_on : -1;
}

static void draw(screen_t *s)
{
    char hh[4], mm[4];
    int wh, wc, x, y = UI_TOP + 30;

    if (!editing) {
        list_draw(&list);
        if (alarm_snooze_min >= 0) {
            char buf[24];

            snprintf(buf, sizeof(buf), "snoozed until %02d:%02d", alarm_snooze_min / 60,
                     alarm_snooze_min % 60);
            gfx_text_centered(UI_BOTTOM - 12, buf, C_DIM, FONT_SMALL, 1);
        }
        return;
    }
    /* HH:MM, the field being changed in amber with a bar under it */
    snprintf(hh, sizeof(hh), "%02d", edit_min / 60);
    snprintf(mm, sizeof(mm), "%02d", edit_min % 60);
    wh = gfx_text_width(hh, FONT_HUGE, 1);
    wc = gfx_text_width(":", FONT_HUGE, 1);
    x = (LCD_WIDTH - (2 * wh + wc + 4)) / 2;
    gfx_text(x, y, hh, field == 0 ? C_ACCENT : C_TEXT, FONT_HUGE, 1, 0);
    gfx_text(x + wh + 2, y, ":", C_DIM, FONT_HUGE, 1, 0);
    gfx_text(x + wh + wc + 4, y, mm, field == 1 ? C_ACCENT : C_TEXT, FONT_HUGE, 1, 0);
    gfx_fill(field == 0 ? x : x + wh + wc + 4, y + gfx_text_height(FONT_HUGE, 1) + 4,
             wh, 2, C_ACCENT);
    gfx_text_centered(UI_TOP + 76, "Up/Down change", C_DIM, FONT_SMALL, 1);
    gfx_text_centered(UI_TOP + 88, "Left/Right hours, minutes", C_DIM, FONT_SMALL, 1);
}

static void edit_key(const key_event_t *ev)
{
    int h = edit_min / 60, m = edit_min % 60;

    switch (ev->code) {
    case KEY_UP:
    case KEY_DOWN: {
        int d = ev->code == KEY_UP ? 1 : -1;

        if (field == 0)
            h = (h + d + 24) % 24;
        else
            m = (m + d + 60) % 60;
        edit_min = h * 60 + m;
        break;
    }
    case KEY_LEFT:
    case KEY_RIGHT:
        field ^= 1;
        break;
    case KEY_CENTER:
    case KEY_LSOFT:
        ui_settings.alarm_min = edit_min;
        ui_settings.alarm_on = 1;       /* setting a time means wanting it */
        alarm_snooze_min = -1;
        ui_settings_save();
        editing = false;
        break;
    case KEY_RSOFT:
        editing = false;
        break;
    }
    ui_redraw();
}

static void key(screen_t *s, const key_event_t *ev)
{
    if (editing) {
        edit_key(ev);
        s->left = editing ? "Save" : "Select";
        return;
    }
    if (list_key(&list, ev))
        return;
    if (ev->code == KEY_CENTER || ev->code == KEY_LSOFT) {
        if (list.sel == R_ON) {
            ui_settings.alarm_on = !ui_settings.alarm_on;
            if (!ui_settings.alarm_on)
                alarm_snooze_min = -1;
            ui_settings_save();
        } else {
            editing = true;
            edit_min = ui_settings.alarm_min;
            field = 0;
            s->left = "Save";
        }
    } else if (ev->code == KEY_RSOFT) {
        ui_pop();
    }
}

static void leave(screen_t *s)
{
    editing = false;
    s->left = "Select";
}

static screen_t screen = {
    .title = "Alarm", .left = "Select", .center = "OK", .right = "Back",
    .draw = draw, .key = key, .leave = leave,
};

screen_t *app_alarm(void)
{
    list.count = R_COUNT;
    list.item = item;
    list.icon = item_icon;
    list.toggle = item_toggle;
    return &screen;
}
