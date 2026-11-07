/* B310E OS — Alarm: the OS's alarms (up to 8, os/alarm.h) to see, add,
 * change and delete. The OS keeps them and rings them itself, whatever is
 * open; this app only edits them.
 *
 * The list: each alarm's time, days and label with its switch, then "Add
 * alarm"; the next one to ring at the top. Center edits (or adds), LSoft
 * turns the alarm on or off. Editing: the time (Up/Down change, Left/Right
 * hours or minutes), the days it repeats on (none: once), a label, on or
 * off, delete. Every change is saved at once.
 *
 * A loadable app (os/modules); it was built into the OS (Tools -> Alarm,
 * one alarm) until 2026-11-07.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "app.h"
#include "icons.h"

#define CARD_H   24
#define ROW_H    (CARD_H + 2)
#define ROWS     4
#define LIST_Y   (UI_TOP + 14)

static int sel, top;
static int cur = -1;                    /* the alarm edited; -1: a new one */
static alarm_t a;                       /* its copy */

static bool fetch(void)
{
    return cur >= 0 && alarm_get(cur, &a);
}

static void put(void)
{
    if (cur >= 0 && alarm_set(cur, &a) < 0)
        ui_message("Alarm", "Could not save the alarm.");
}

static void hhmm(int minute, char *buf, int n)
{
    snprintf(buf, (size_t)n, "%02d:%02d", minute / 60, minute % 60);
}

/* ---- the time --------------------------------------------------------------------- */

static int edit_min, field;             /* field 0 hours, 1 minutes */

static void time_draw(screen_t *s)
{
    char hh[4], mm[4];
    int wh, wc, x, y = UI_TOP + 30;

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

static void time_key(screen_t *s, const key_event_t *ev)
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
        if (cur < 0) {                  /* a new alarm: on, once, no label */
            int i;

            memset(&a, 0, sizeof(a));
            a.on = 1;
            a.minute = (uint16_t)edit_min;
            a.days = ALARM_ONCE;
            i = alarm_set(alarm_count(), &a);
            if (i < 0) {
                ui_pop();
                ui_message("Alarm", "Could not add the alarm.");
                return;
            }
            sel = i;
            klog("alarm app: added %02d:%02d", edit_min / 60, edit_min % 60);
        } else if (fetch()) {
            a.minute = (uint16_t)edit_min;
            a.on = 1;                   /* setting a time means wanting it */
            put();
        }
        ui_pop();
        return;
    case KEY_RSOFT:
        ui_pop();
        return;
    }
}

static screen_t time_screen = {
    .title = "Time", .left = "Save", .center = "OK", .right = "Cancel",
    .draw = time_draw, .key = time_key,
};

static void edit_time(int minute, const char *title)
{
    edit_min = minute;
    field = 0;
    time_screen.title = title;
    ui_push(&time_screen);
}

/* ---- the days --------------------------------------------------------------------- */

static const char *const day_names[7] = {     /* Monday first; bit (i + 1) % 7 */
    "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday",
};
static list_t days_list;

static unsigned day_bit(int i)
{
    return 1u << ((i + 1) % 7);
}

static void day_item(void *ctx, int i, char *text, int tn, char *detail, int dn)
{
    snprintf(text, (size_t)tn, "%s", day_names[i]);
}

static int day_toggle(void *ctx, int i)
{
    return (a.days & day_bit(i)) != 0;
}

static void days_draw(screen_t *s)
{
    list_draw(&days_list);
}

static void days_key(screen_t *s, const key_event_t *ev)
{
    if (list_key(&days_list, ev))
        return;
    if (ev->code == KEY_CENTER || ev->code == KEY_LSOFT) {
        if (fetch()) {
            a.days ^= (uint8_t)day_bit(days_list.sel);
            put();
        }
    } else if (ev->code == KEY_RSOFT) {
        ui_pop();
    }
}

static screen_t days_screen = {
    .title = "Repeat", .left = "Switch", .center = "OK", .right = "Back",
    .draw = days_draw, .key = days_key,
};

/* ---- one alarm ---------------------------------------------------------------------- */

enum { E_TIME, E_DAYS, E_LABEL, E_ON, E_DELETE, E_ROWS };
static list_t edit_list;

static void edit_item(void *ctx, int i, char *text, int tn, char *detail, int dn)
{
    switch (i) {
    case E_TIME:
        snprintf(text, (size_t)tn, "Time");
        hhmm(a.minute, detail, dn);
        break;
    case E_DAYS:
        snprintf(text, (size_t)tn, "Repeat");
        alarm_days_text(a.days, detail, dn);
        break;
    case E_LABEL:
        snprintf(text, (size_t)tn, "Label");
        snprintf(detail, (size_t)dn, "%s", a.label[0] ? a.label : "none");
        break;
    case E_ON:
        snprintf(text, (size_t)tn, "Alarm on");
        break;
    case E_DELETE:
        snprintf(text, (size_t)tn, "Delete");
        break;
    }
}

static const icon_t *edit_icon(void *ctx, int i)
{
    static const char *const names[E_ROWS] = { "clock", "alarm", "notes", "alarm", "file" };

    return ui_icon(names[i]);
}

static int edit_toggle(void *ctx, int i)
{
    return i == E_ON ? a.on != 0 : -1;
}

static void label_done(const char *text, void *ctx)
{
    if (!fetch())
        return;
    snprintf(a.label, sizeof(a.label), "%s", text);
    put();
}

static void delete_yes(void)
{
    if (cur >= 0 && alarm_delete(cur))
        klog("alarm app: deleted alarm %d", cur);
    cur = -1;
    ui_pop();                           /* the alarm's page */
}

static void edit_draw(screen_t *s)
{
    if (!fetch()) {                     /* gone */
        gfx_text_centered(UI_TOP + 46, "(deleted)", C_DIM, FONT_BODY, 1);
        return;
    }
    list_draw(&edit_list);
}

static void edit_key(screen_t *s, const key_event_t *ev)
{
    char msg[40];

    if (list_key(&edit_list, ev))
        return;
    if (ev->code == KEY_RSOFT) {
        ui_pop();
        return;
    }
    if ((ev->code != KEY_CENTER && ev->code != KEY_LSOFT) || !fetch())
        return;
    switch (edit_list.sel) {
    case E_TIME:
        edit_time(a.minute, "Time");
        break;
    case E_DAYS:
        days_list.sel = days_list.top = 0;
        ui_push(&days_screen);
        break;
    case E_LABEL:
        ui_text_edit("Label", a.label, (int)sizeof(a.label) - 1, false, label_done, NULL);
        break;
    case E_ON:
        a.on = !a.on;
        put();
        break;
    case E_DELETE:
        snprintf(msg, sizeof(msg), "Delete the %02d:%02d alarm?", a.minute / 60, a.minute % 60);
        ui_confirm("Delete", msg, delete_yes);
        break;
    }
}

static screen_t edit_screen = {
    .title = "Edit alarm", .left = "Change", .center = "OK", .right = "Back",
    .draw = edit_draw, .key = edit_key,
};

/* ---- the list ------------------------------------------------------------------------ */

static void next_text(char *buf, int n)
{
    uint32_t in;
    int i = alarm_next(&in);
    alarm_t x;
    char t[8];

    if (i < 0 || !alarm_get(i, &x)) {
        snprintf(buf, (size_t)n, "No alarm on");
        return;
    }
    hhmm(x.minute, t, sizeof(t));
    if (in < 60)
        snprintf(buf, (size_t)n, "Next %s, in %lu min", t, (unsigned long)in);
    else if (in < 24 * 60)
        snprintf(buf, (size_t)n, "Next %s, in %lu h %lu min", t, (unsigned long)(in / 60),
                 (unsigned long)(in % 60));
    else
        snprintf(buf, (size_t)n, "Next %s, in %lu d %lu h", t, (unsigned long)(in / 1440),
                 (unsigned long)(in / 60 % 24));
}

static void list_labels(screen_t *s)
{
    alarm_t x;

    if (sel < alarm_count() && alarm_get(sel, &x)) {
        s->left = x.on ? "Off" : "On";
        s->center = "Edit";
    } else {
        s->left = alarm_count() < ALARM_MAX ? "Add" : NULL;
        s->center = "Add";
    }
}

static void list_draw_(screen_t *s)
{
    int count = alarm_count(), rows = count + 1, x = UI_PAD, y = LIST_Y;
    int w = LCD_WIDTH - 2 * UI_PAD - (rows > ROWS ? 4 : 0);
    char buf[48];

    if (sel >= rows)
        sel = rows - 1;
    if (sel < top)
        top = sel;
    if (sel >= top + ROWS)
        top = sel - ROWS + 1;
    list_labels(s);
    next_text(buf, sizeof(buf));
    gfx_text(x + 2, UI_TOP + 3, buf, C_DIM, FONT_SMALL, 1, LCD_WIDTH - 2 * UI_PAD - 4);

    for (int r = 0; r < ROWS && top + r < rows; r++, y += ROW_H) {
        int i = top + r;
        bool on_sel = i == sel;
        uint16_t fg = on_sel ? C_SEL_TEXT : C_TEXT, dim = on_sel ? C_SEL_TEXT : C_DIM;
        alarm_t x_;

        gfx_round(x, y, w, CARD_H, UI_RADIUS, on_sel ? C_ACCENT : C_CARD);
        if (i == count) {               /* the last row: add one */
            const char *t = count < ALARM_MAX ? "Add alarm" : "8 alarms at most";

            gfx_icon(x + 4, y + (CARD_H - ICON_SIZE) / 2, ui_icon("alarm"),
                     on_sel ? C_SEL_TEXT : C_ACCENT);
            gfx_text(x + 4 + ICON_SIZE + 5, y + (CARD_H - gfx_text_height(FONT_BODY, 1)) / 2,
                     t, fg, FONT_BODY, 1, w - ICON_SIZE - 14);
            continue;
        }
        if (!alarm_get(i, &x_))
            continue;
        {
            int sx = x + w - 6 - 20;
            char days[32], sub[48];

            hhmm(x_.minute, buf, sizeof(buf));
            gfx_text(x + 6, y + 3, buf, x_.on ? fg : dim, FONT_LARGE, 1, 0);
            alarm_days_text(x_.days, days, sizeof(days));
            if (x_.label[0])
                snprintf(sub, sizeof(sub), "%s, %s", x_.label, days);
            else
                snprintf(sub, sizeof(sub), "%s", days);
            gfx_text(x + 6, y + 15, sub, dim, FONT_SMALL, 1, sx - x - 10);
            if (on_sel)
                gfx_round(sx - 1, y + (CARD_H - 10) / 2 - 1, 22, 12, 2, C_SEL_TEXT);
            gfx_switch(sx, y + (CARD_H - 10) / 2, x_.on);
        }
    }
    if (rows > ROWS) {                  /* a thin scroll indicator */
        int track = ROWS * ROW_H - 2, h = track * ROWS / rows;
        int pos = (track - (h < 8 ? 8 : h)) * top / (rows - ROWS);

        gfx_fill(LCD_WIDTH - UI_PAD - 2, LIST_Y, 2, track, C_CARD);
        gfx_fill(LCD_WIDTH - UI_PAD - 2, LIST_Y + pos, 2, h < 8 ? 8 : h, C_DIM);
    }
}

static void list_key_(screen_t *s, const key_event_t *ev)
{
    int count = alarm_count(), rows = count + 1;

    switch (ev->code) {
    case KEY_UP:
        sel = sel > 0 ? sel - 1 : rows - 1;
        break;
    case KEY_DOWN:
        sel = sel < rows - 1 ? sel + 1 : 0;
        break;
    case KEY_CENTER:
    case KEY_LSOFT:
        if (sel >= count) {             /* add */
            if (count >= ALARM_MAX) {
                ui_message("Alarm", "There are 8 alarms already.");
                break;
            }
            cur = -1;
            edit_time(7 * 60, "New alarm");
        } else if (ev->code == KEY_LSOFT) {
            cur = sel;
            if (fetch()) {
                a.on = !a.on;
                put();
            }
        } else {
            cur = sel;
            edit_list.sel = edit_list.top = 0;
            if (fetch())
                ui_push(&edit_screen);
        }
        break;
    case KEY_RSOFT:
        ui_pop();
        return;
    }
    list_labels(s);
}

static void list_tick(screen_t *s)
{
    /* redrawn every second: the next alarm's countdown, one that rang */
}

static screen_t list_screen = {
    .title = "Alarm", .left = "Add", .center = "Add", .right = "Back",
    .draw = list_draw_, .key = list_key_, .tick = list_tick,
};

static screen_t *start(void)
{
    sel = top = 0;
    cur = -1;
    edit_list.count = E_ROWS;
    edit_list.item = edit_item;
    edit_list.icon = edit_icon;
    edit_list.toggle = edit_toggle;
    days_list.count = 7;
    days_list.item = day_item;
    days_list.toggle = day_toggle;
    list_labels(&list_screen);
    return &list_screen;
}

static const icon_t app_icon = APP_ICON_ALARM;
APP("Alarm", start, NULL, &app_icon);
