/* B310E OS — the quick panel: Center held on Home or the Menu.
 *
 * The notifications first (newest on top; Left/Right picks a button,
 * Center presses it), then the controls: Sound (Left/Right: the volume of
 * what is playing; Center: its page), brightness (Left/Right) and the torch
 * (Center). The Sound page has every volume: each kind of sound, tactile
 * feedback, and each app that has one, and LSoft resets them all. The changes
 * are the same as Settings' and kept the same way.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/audio.h"
#include "os/drivers.h"
#include "os/kernel.h"
#include "os/lib.h"
#include "os/notify.h"
#include "os/ui.h"

enum { P_SOUND, P_BRIGHT, P_TORCH, P_COUNT };

static const struct {
    const char *name;
    const icon_t *icon;
} controls[P_COUNT] = {
    { "Sound", &icon_speaker },
    { "Brightness", &icon_sun },
    { "Torch", &icon_torch },
};
static const char *const tactile_names[] = { "Off", "Light", "Medium", "Strong" };
static screen_t panel, sound;

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
    panel.left = sel - notes() == P_SOUND ? "Open" : "Press";
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

static void level_text(int cat, char *buf, int n)
{
    int v = volume_level(cat);

    if (cat == VOL_TACTILE)
        snprintf(buf, (size_t)n, "%s", tactile_names[v & 3]);
    else if (v)
        snprintf(buf, (size_t)n, "%d", v);
    else
        snprintf(buf, (size_t)n, "Mute");
}

static int level_max(int cat)
{
    return cat == VOL_TACTILE ? 3 : VOL_LEVELS;
}

/* a row with a value on the right and a bar under them */
static void draw_row(int y, const icon_t *icon, const char *name, const char *value,
                     int val, int max, bool selected)
{
    int x = UI_PAD, w = LCD_WIDTH - 2 * UI_PAD, vw = gfx_text_width(value, FONT_SMALL, 1);

    gfx_round(x, y, w, CTRL_H - 2, UI_RADIUS, selected ? C_ACCENT : C_CARD);
    gfx_icon(x + 3, y + 2, icon, selected ? C_SEL_TEXT : C_ACCENT);
    gfx_text(x + 18, y + 3, name, selected ? C_SEL_TEXT : C_TEXT, FONT_BODY, 1, w - 22 - vw - 3);
    gfx_text(x + w - 4 - vw, y + 3, value, selected ? C_SEL_TEXT : C_DIM, FONT_SMALL, 1, 0);
    gfx_bar(x + 18, y + 13, w - 22, 2, val, max, selected ? C_SEL_TEXT : C_ACCENT);
}

static void draw_control(int y, int c, bool selected)
{
    int x = UI_PAD, w = LCD_WIDTH - 2 * UI_PAD;
    char buf[32], level[8];

    if (c == P_TORCH) {
        gfx_round(x, y, w, CTRL_H - 2, UI_RADIUS, selected ? C_ACCENT : C_CARD);
        gfx_icon(x + 3, y + 2, controls[c].icon, selected ? C_SEL_TEXT : C_ACCENT);
        gfx_text(x + 18, y + 3, controls[c].name, selected ? C_SEL_TEXT : C_TEXT, FONT_BODY, 1, 60);
        gfx_switch(x + w - 24, y + 3, torch_get());
    } else if (c == P_BRIGHT) {
        snprintf(buf, sizeof(buf), "%d%%", ui_settings.brightness);
        draw_row(y, controls[c].icon, controls[c].name, buf, ui_settings.brightness, 100, selected);
    } else {                            /* Sound: whose level the keys change */
        int cat = volume_current();

        level_text(cat, level, sizeof(level));
        snprintf(buf, sizeof(buf), "%s %s", volume_name(cat), level);
        if (gfx_text_width(buf, FONT_SMALL, 1) > w - 22 - 34)
            snprintf(buf, sizeof(buf), "%s", level);
        draw_row(y, controls[c].icon, controls[c].name, buf, volume_level(cat), level_max(cat), selected);
    }
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

static void adjust_level(int cat, int d)
{
    volume_set(cat, volume_level(cat) + d);
    if (cat == VOL_TACTILE && volume_level(VOL_TACTILE))
        ui_buzz(volume_level(VOL_TACTILE) * 2, 40);         /* how it feels */
}

static void adjust(int c, int d)
{
    if (c == P_SOUND) {
        adjust_level(volume_current(), d);
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
        else if (sel - n == P_SOUND)
            ui_push(&sound);
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

/* ---- the Sound page: every volume, the apps' after the OS's ------------- */
static const uint8_t sound_fixed[] = { VOL_MUSIC, VOL_ALARM, VOL_CALL, VOL_TACTILE, VOL_APP };
#define SOUND_FIXED ((int)ARRAY_SIZE(sound_fixed))

static int sound_sel, sound_scroll;

static int sound_rows(void)
{
    return SOUND_FIXED + volume_tags();
}

static int sound_cat(int r)
{
    return r < SOUND_FIXED ? sound_fixed[r] : volume_tag_at(r - SOUND_FIXED);
}

static void sound_follow(void)
{
    int top = sound_sel * CTRL_H, view = UI_CONTENT_H - 4;

    if (top < sound_scroll)
        sound_scroll = top;
    if (top + CTRL_H > sound_scroll + view)
        sound_scroll = top + CTRL_H - view;
}

static void sound_draw(screen_t *s)
{
    int top = UI_TOP + 2, bottom = UI_BOTTOM - 2;

    for (int r = 0; r < sound_rows(); r++) {
        int y = top - sound_scroll + r * CTRL_H, cat = sound_cat(r);
        char level[8];

        if (y + CTRL_H < top || y > bottom)
            continue;
        level_text(cat, level, sizeof(level));
        draw_row(y, volume_icon(cat), volume_name(cat), level, volume_level(cat),
                 level_max(cat), r == sound_sel);
    }
}

/* It opens on what is playing: a game's own row, from over the game */
static void sound_enter(screen_t *s)
{
    int cat = volume_current();

    sound_sel = sound_scroll = 0;
    for (int r = 0; r < sound_rows(); r++)
        if (sound_cat(r) == cat)
            sound_sel = r;
    sound_follow();
}

static void sound_reset(void)
{
    volume_reset();
    sound_enter(&sound);                /* fewer rows now */
}

static void sound_key(screen_t *s, const key_event_t *ev)
{
    switch (ev->code) {
    case KEY_UP:
        if (sound_sel > 0)
            sound_sel--;
        break;
    case KEY_DOWN:
        if (sound_sel < sound_rows() - 1)
            sound_sel++;
        break;
    case KEY_LEFT:
    case KEY_RIGHT:
        adjust_level(sound_cat(sound_sel), ev->code == KEY_RIGHT ? 1 : -1);
        break;
    case KEY_LSOFT:
        ui_confirm("Reset sound", "Every volume back to its default, and the list of apps cleared?",
                   sound_reset);
        return;
    case KEY_RSOFT:
        ui_pop();
        return;
    }
    sound_follow();
}

static screen_t sound = {
    .title = "Sound", .left = "Reset", .right = "Back",
    .enter = sound_enter, .draw = sound_draw, .key = sound_key,
};

screen_t *ui_sound(void)
{
    return &sound;
}
