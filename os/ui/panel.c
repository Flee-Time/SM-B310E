/* B310E OS — the quick panel: Center held on Home or the Menu.
 *
 * The notifications first (newest on top; Left/Right picks a button,
 * Center presses it), then the controls: the volume of each kind of
 * sound, tactile feedback, brightness (Left/Right) and the torch (Center).
 * The changes are the same as Settings' and kept the same way.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/audio.h"
#include "os/drivers.h"
#include "os/kernel.h"
#include "os/lib.h"
#include "os/notify.h"
#include "os/ui.h"

enum { P_MUSIC, P_APPS, P_ALARM, P_CALLS, P_TACTILE, P_BRIGHT, P_TORCH, P_COUNT };

static const struct {
    const char *name;
    const icon_t *icon;
    int vol;                            /* VOL_*, -1 not a volume */
} controls[P_COUNT] = {
    { "Music", &icon_music, VOL_MUSIC },
    { "Apps", &icon_apps, VOL_APP },
    { "Alarm", &icon_alarm, VOL_ALARM },
    { "Calls", &icon_speaker, VOL_CALL },
    { "Tactile", &icon_vibrate, VOL_TACTILE },
    { "Brightness", &icon_sun, -1 },
    { "Torch", &icon_torch, -1 },
};
static const char *const tactile_names[] = { "Off", "Light", "Medium", "Strong" };

#define NOTE_H    44                    /* with buttons; 28 without */
#define CTRL_H    UI_ROW_H
#define EMPTY_H   16                    /* "No notifications" */

static int sel, act_sel, scroll;
static const notify_t *sel_note;        /* act_sel belongs to it */

static int notes(void)
{
    return notify_count();
}

static int n_actions(const notify_t *n)
{
    int c = 0;

    for (int i = 0; i < NOTIFY_ACTIONS; i++)
        if (n->action[i])
            c++;
    return c;
}

static int note_h(const notify_t *n)
{
    return n_actions(n) ? NOTE_H : 28;
}

/* the row's top and height, in the panel's own coordinates */
static int row_top(int r, int *h)
{
    int y = 0, n = notes();

    if (!n)
        y = EMPTY_H;
    for (int i = 0; i < n; i++) {
        int nh = note_h(notify_get(i));

        if (i == r) {
            *h = nh;
            return y;
        }
        y += nh;
    }
    *h = CTRL_H;
    return y + (r - n) * CTRL_H;
}

static int rows(void)
{
    return notes() + P_COUNT;
}

/* the middle button of three (Music's Pause), else the first there is */
static int first_action(const notify_t *n)
{
    if (n->action[1] && n->action[0] && n->action[2])
        return 1;
    for (int i = 0; i < NOTIFY_ACTIONS; i++)
        if (n->action[i])
            return i;
    return -1;
}

static void follow_sel(void)
{
    int h, top, view = UI_CONTENT_H - 4;
    notify_t *n;

    if (sel >= rows())
        sel = rows() - 1;
    if (sel < 0)
        sel = 0;
    n = sel < notes() ? notify_get(sel) : NULL;
    if (n != sel_note) {
        sel_note = n;
        act_sel = n ? first_action(n) : -1;
    }
    top = row_top(sel, &h);
    if (sel == 0)
        top = 0;                        /* the "No notifications" line with it */
    if (top < scroll)
        scroll = top;
    if (top + h > scroll + view)
        scroll = top + h - view;
}

static void draw_note(int y, notify_t *n, bool selected)
{
    int x = UI_PAD, w = LCD_WIDTH - 2 * UI_PAD, h = note_h(n) - 2, na = n_actions(n);

    gfx_round(x, y, w, h, UI_RADIUS, selected ? C_CARD_HI : C_CARD);
    if (selected)
        gfx_fill(x, y + 2, 2, h - 4, C_ACCENT);
    gfx_icon(x + 4, y + 3, n->icon ? n->icon : &icon_alarm, C_ACCENT);
    gfx_text(x + 19, y + 3, n->title, C_TEXT, FONT_BODY, 1, w - 23);
    if (n->text[0])
        gfx_text(x + 19, y + 14, n->text, C_DIM, FONT_SMALL, 1, w - 23);
    if (n->progress > 0)
        gfx_bar(x + 19, y + 24, w - 25, 2, n->progress, 1000, C_ACCENT);
    if (na) {                           /* the buttons, side by side */
        int bw = (w - 8 - (na - 1) * 3) / na, bx = x + 4, by = y + h - 14;

        for (int i = 0; i < NOTIFY_ACTIONS; i++) {
            bool on;

            if (!n->action[i])
                continue;
            on = selected && i == act_sel;
            gfx_round(bx, by, bw, 12, UI_RADIUS, on ? C_ACCENT : C_BG);
            gfx_text(bx + (bw - gfx_text_width(n->action[i], FONT_SMALL, 1)) / 2, by + 2,
                     n->action[i], on ? C_SEL_TEXT : C_TEXT, FONT_SMALL, 1, bw - 2);
            bx += bw + 3;
        }
    }
}

static void control_value(int c, char *buf, int n, int *val, int *max)
{
    *val = *max = 0;
    if (controls[c].vol == VOL_TACTILE) {
        *val = volume_level(VOL_TACTILE);
        *max = 3;
        snprintf(buf, (size_t)n, "%s", tactile_names[*val & 3]);
    } else if (controls[c].vol >= 0) {
        *val = volume_level(controls[c].vol);
        *max = VOL_LEVELS;
        if (*val)
            snprintf(buf, (size_t)n, "%d", *val);
        else
            snprintf(buf, (size_t)n, "Mute");
    } else if (c == P_BRIGHT) {
        *val = ui_settings.brightness;
        *max = 100;
        snprintf(buf, (size_t)n, "%d%%", *val);
    } else {
        buf[0] = 0;
    }
}

static void draw_control(int y, int c, bool selected)
{
    int x = UI_PAD, w = LCD_WIDTH - 2 * UI_PAD, val, max;
    uint16_t fg = selected ? C_SEL_TEXT : C_TEXT;
    char buf[12];

    gfx_round(x, y, w, CTRL_H - 2, UI_RADIUS, selected ? C_ACCENT : C_CARD);
    gfx_icon(x + 3, y + 2, controls[c].icon, selected ? C_SEL_TEXT : C_ACCENT);
    gfx_text(x + 18, y + 3, controls[c].name, fg, FONT_BODY, 1, 60);
    if (c == P_TORCH) {
        gfx_switch(x + w - 24, y + 3, torch_get());
        return;
    }
    control_value(c, buf, sizeof(buf), &val, &max);
    gfx_text(x + w - 4 - gfx_text_width(buf, FONT_SMALL, 1), y + 3, buf,
             selected ? C_SEL_TEXT : C_DIM, FONT_SMALL, 1, 0);
    gfx_bar(x + 18, y + 13, w - 22, 2, val, max, selected ? C_SEL_TEXT : C_ACCENT);
}

static void draw(screen_t *s)
{
    int n = notes(), top = UI_TOP + 2, bottom = UI_BOTTOM - 2;

    follow_sel();
    /* rows past the edges: the title and soft-key bars are drawn after */
    if (!n)
        gfx_text_centered(top - scroll + 3, "No notifications", C_DIM, FONT_SMALL, 1);
    for (int r = 0; r < rows(); r++) {
        int h, y = top - scroll + row_top(r, &h);

        if (y + h < top || y > bottom)
            continue;
        if (r < n)
            draw_note(y, notify_get(r), r == sel);
        else
            draw_control(y, r - n, r == sel);
    }
}

static void adjust(int c, int d)
{
    if (controls[c].vol >= 0) {
        volume_set(controls[c].vol, volume_level(controls[c].vol) + d);
        if (controls[c].vol == VOL_TACTILE && volume_level(VOL_TACTILE))
            ui_buzz(volume_level(VOL_TACTILE) * 2, 40);     /* how it feels */
    } else if (c == P_BRIGHT) {
        int b = ui_settings.brightness + d * 10;

        ui_settings.brightness = b < 10 ? 10 : b > 100 ? 100 : b;
        ui_apply_brightness();
        ui_settings_save();
    } else if (c == P_TORCH) {
        torch_set(!torch_get());
    }
}

static void key(screen_t *s, const key_event_t *ev)
{
    int n = notes();
    notify_t *note = sel < n ? notify_get(sel) : NULL;

    switch (ev->code) {
    case KEY_UP:
        if (sel > 0)
            sel--;
        break;
    case KEY_DOWN:
        if (sel < rows() - 1)
            sel++;
        break;
    case KEY_LEFT:
    case KEY_RIGHT: {
        int d = ev->code == KEY_RIGHT ? 1 : -1;

        if (note) {                     /* the next button that is there */
            for (int i = act_sel + d; i >= 0 && i < NOTIFY_ACTIONS; i += d)
                if (note->action[i]) {
                    act_sel = i;
                    break;
                }
        } else {
            adjust(sel - n, d);
        }
        break;
    }
    case KEY_CENTER:
    case KEY_LSOFT:
        if (note)
            notify_run(note, act_sel);
        else if (sel - n == P_TORCH)
            adjust(P_TORCH, 0);
        break;
    case KEY_RSOFT:
        ui_pop();
        return;
    }
    follow_sel();
}

static void tick(screen_t *s)
{
    notify_tick();                      /* live texts (the track, its time) */
}

static void enter(screen_t *s)
{
    sel = scroll = 0;
    sel_note = NULL;
    notify_tick();
    follow_sel();
}

static screen_t panel = {
    .title = "Quick panel", .left = "Press", .center = "OK", .right = "Close",
    .enter = enter, .draw = draw, .key = key, .tick = tick, .tick_ms = 500,
};

screen_t *ui_panel(void)
{
    return &panel;
}
