/* B310E OS — "Date & time": shows the RTC's date and time and sets them.
 *
 * Fields: day, month, year, hour, minute, second. Left/Right move between
 * them, Up/Down step the selected one (wrapping), digits type it (the
 * field moves on when full), LSoft saves, RSoft cancels. Fields not
 * touched keep following the clock, so changing only the date does not
 * stop the time.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/lib.h"
#include "os/rtc.h"
#include "os/ui.h"

enum { F_DAY, F_MONTH, F_YEAR, F_HOUR, F_MIN, F_SEC, F_COUNT };

static const uint8_t field_width[F_COUNT] = { 2, 2, 4, 2, 2, 2 };

/* Read by os/tests/test-rtc.py: keep the layout. */
static struct {
    int v[F_COUNT];                 /* the values shown */
    int field;                      /* selected */
    int typed;                      /* digits typed into it so far */
    unsigned touched;               /* fields the user changed (bit each) */
} clock_state;

static int field_min(int f)
{
    return f == F_DAY || f == F_MONTH ? 1 : f == F_YEAR ? RTC_YEAR_MIN : 0;
}

/* A typed day may be up to 31 until the month and year are in: saving
 * cuts it to the month's length (strict). */
static int field_max(int f, bool strict)
{
    switch (f) {
    case F_DAY:
        return strict ? rtc_days_in_month(clock_state.v[F_YEAR],
                                          clock_state.v[F_MONTH]) : 31;
    case F_MONTH:
        return 12;
    case F_YEAR:
        return RTC_YEAR_MAX;
    case F_HOUR:
        return 23;
    default:
        return 59;
    }
}

static void clamp(int f, bool strict)
{
    int *v = &clock_state.v[f];

    if (*v < field_min(f))
        *v = field_min(f);
    if (*v > field_max(f, strict))
        *v = field_max(f, strict);
}

static void clamp_all(bool strict)
{
    clamp(F_MONTH, strict);
    clamp(F_YEAR, strict);
    clamp(F_DAY, strict);           /* after them: its maximum depends on both */
    clamp(F_HOUR, strict);
    clamp(F_MIN, strict);
    clamp(F_SEC, strict);
}

/* The untouched fields from the clock. */
static void follow_clock(void)
{
    rtc_datetime_t t;
    int live[F_COUNT];

    rtc_get(&t);
    live[F_DAY] = t.day;
    live[F_MONTH] = t.month;
    live[F_YEAR] = t.year;
    live[F_HOUR] = t.hour;
    live[F_MIN] = t.min;
    live[F_SEC] = t.sec;
    for (int f = 0; f < F_COUNT; f++)
        if (!(clock_state.touched & 1u << f))
            clock_state.v[f] = live[f];
    clamp_all(false);
}

static void end_typing(void)
{
    if (clock_state.typed) {
        clock_state.typed = 0;
        clamp_all(false);
    }
}

static void touch(int f)
{
    clock_state.touched |= 1u << f;
}

static void clock_enter(screen_t *s)
{
    memset(&clock_state, 0, sizeof(clock_state));
    follow_clock();
}

static void clock_tick(screen_t *s)
{
    if (!clock_state.typed)
        follow_clock();
}

static void field_text(int f, char *buf, size_t n)
{
    int w = field_width[f], typed = clock_state.typed;

    if (f == clock_state.field && typed) {
        /* the digits so far, then a blank for each one to come */
        int len = snprintf(buf, n, "%0*d", typed, clock_state.v[f]);

        while (len < w && (size_t)len + 1 < n)
            buf[len++] = '_';
        buf[len] = '\0';
    } else {
        snprintf(buf, n, "%0*d", w, clock_state.v[f]);
    }
}

/* One line of three fields with separators, centred, the selected field
 * on the accent colour. */
static void draw_line(int y, int first, char sep, int scale)
{
    char text[3][8], sep_s[2] = { sep, '\0' };
    int width = 2 * gfx_text_width(sep_s, FONT_LARGE, scale), x, h = 16 * scale;

    for (int i = 0; i < 3; i++) {
        field_text(first + i, text[i], sizeof(text[i]));
        width += gfx_text_width(text[i], FONT_LARGE, scale);
    }
    x = (LCD_WIDTH - width) / 2;
    for (int i = 0; i < 3; i++) {
        int w = gfx_text_width(text[i], FONT_LARGE, scale);
        bool sel = first + i == clock_state.field;

        if (sel)
            gfx_fill(x - 1, y - 1, w + 2, h + 2, C_ACCENT);
        gfx_text(x, y, text[i], sel ? C_SEL_TEXT : C_TEXT, FONT_LARGE, scale, 0);
        x += w;
        if (i < 2)
            x += gfx_text(x, y, sep_s, C_DIM, FONT_LARGE, scale, 0);
    }
}

static void clock_draw(screen_t *s)
{
    int wday = rtc_weekday(clock_state.v[F_YEAR], clock_state.v[F_MONTH],
                           clock_state.v[F_DAY]);

    gfx_text_centered(UI_TOP + 8, rtc_weekday_name(wday), C_DIM, FONT_SMALL, 1);
    draw_line(UI_TOP + 22, F_DAY, '.', 1);
    draw_line(UI_TOP + 50, F_HOUR, ':', 1);
    gfx_text_centered(UI_TOP + 82, "Up/Down: change", C_DIM, FONT_SMALL, 1);
    gfx_text_centered(UI_TOP + 92, "Left/Right: field", C_DIM, FONT_SMALL, 1);
    gfx_text_centered(UI_TOP + 102, "0-9: type", C_DIM, FONT_SMALL, 1);
}

static void step(int delta)
{
    int f = clock_state.field, lo, hi, *v = &clock_state.v[f];

    end_typing();
    lo = field_min(f);
    hi = field_max(f, true);        /* the day wraps at the month's end */
    if (*v > hi)
        *v = hi;
    *v += delta;
    if (*v > hi)
        *v = lo;
    if (*v < lo)
        *v = hi;
    touch(f);
}

static void move(int delta)
{
    end_typing();
    clock_state.field = (clock_state.field + delta + F_COUNT) % F_COUNT;
}

static void type_digit(int d)
{
    int f = clock_state.field;

    if (!clock_state.typed)
        clock_state.v[f] = 0;
    clock_state.v[f] = clock_state.v[f] * 10 + d;
    clock_state.typed++;
    touch(f);
    if (clock_state.typed >= field_width[f]) {
        end_typing();
        if (f + 1 < F_COUNT)
            clock_state.field = f + 1;
    }
}

static void save(void)
{
    rtc_datetime_t t;
    int r;

    end_typing();
    follow_clock();                 /* the untouched fields as of now */
    clamp_all(true);                /* 31.02. -> 28.02. */
    t.day = (uint8_t)clock_state.v[F_DAY];
    t.month = (uint8_t)clock_state.v[F_MONTH];
    t.year = (uint16_t)clock_state.v[F_YEAR];
    t.hour = (uint8_t)clock_state.v[F_HOUR];
    t.min = (uint8_t)clock_state.v[F_MIN];
    t.sec = (uint8_t)clock_state.v[F_SEC];
    t.wday = 0;
    r = rtc_set(&t);
    if (r == 0) {
        ui_pop();
        return;
    }
    ui_message("Date & time", r == RTC_ETIMEOUT ?
               "The clock did not take the new time." :
               "Could not set the clock.");
}

static void clock_key(screen_t *s, const key_event_t *ev)
{
    switch (ev->code) {
    case KEY_UP:
        step(1);
        break;
    case KEY_DOWN:
        step(-1);
        break;
    case KEY_LEFT:
        move(-1);
        break;
    case KEY_RIGHT:
        move(1);
        break;
    case KEY_LSOFT:
        save();
        break;
    case KEY_RSOFT:
        ui_pop();
        break;
    default:
        if (ev->code >= KEY_0 && ev->code <= KEY_9)
            type_digit(ev->code - KEY_0);
        break;
    }
}

static screen_t clock_screen = {
    .title = "Date & time", .left = "Save", .center = NULL, .right = "Cancel",
    .enter = clock_enter, .draw = clock_draw, .key = clock_key,
    .tick = clock_tick, .tick_ms = 250,
};

screen_t *app_clock(void)
{
    return &clock_screen;
}
