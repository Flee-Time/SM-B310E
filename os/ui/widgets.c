/* B310E OS — widgets: card list, dialogs, text view.
 *
 * The look follows LVGL's dark theme: rounded cards on a dark ground, one
 * accent colour for the focused element, Montserrat text. Dialogs float
 * as a card with a soft shadow over the dimmed screen under them.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/kernel.h"
#include "os/lib.h"
#include "os/status.h"
#include "os/ui.h"

/* ---- list ------------------------------------------------------------------ */

#define LIST_ROWS  (UI_CONTENT_H / UI_ROW_H)
#define CARD_H     (UI_ROW_H - 2)

/* A label at x and its value right-aligned at x + w, FONT_SMALL. The value
 * keeps up to two thirds of the width; the label gets the rest and is cut
 * with ".." before it can run into the value. */
void ui_kv(int x, int y, int w, const char *label, const char *value, uint16_t c)
{
    int vw = gfx_text_width(value, FONT_SMALL, 1), room;

    if (vw > w * 2 / 3)
        vw = w * 2 / 3;
    room = w - vw - 4;
    gfx_text(x, y, label, C_DIM, FONT_SMALL, 1, room > 8 ? room : 8);
    gfx_text(x + w - vw, y, value, c, FONT_SMALL, 1, vw);
}

#define LIST_Y     (UI_TOP + (UI_CONTENT_H - LIST_ROWS * UI_ROW_H) / 2 + 1)

/* One row as list_draw shows it: everything its pixels depend on but y */
typedef struct {
    char text[48], detail[24];
    const icon_t *icon;
    int sw;                             /* the switch, -1 none */
    bool sel, used;
} row_t;

/* What the last list_draw left in lcd_fb. With the frame still the screen's
 * (ui_partial), a key redraws only the rows that differ from it, and a scroll
 * by a row or a few moves the rows that stay instead of drawing them again:
 * every key redrew all six rows (157 glyphs and icons, ~124k instructions of
 * list-scroll's ~300k a key, bench 2026-11-17), for two rows that changed. */
static struct {
    const list_t *l;
    int count, top, w;
    row_t row[LIST_ROWS];
} drawn;

static void row_get(list_t *l, int i, row_t *r)
{
    r->text[0] = r->detail[0] = 0;
    l->item(l->ctx, i, r->text, sizeof(r->text), r->detail, sizeof(r->detail));
    utf8_trim(r->text);                 /* filled to a byte limit: never half a character */
    utf8_trim(r->detail);
    r->icon = l->icon ? l->icon(l->ctx, i) : NULL;
    r->sw = l->toggle ? l->toggle(l->ctx, i) : -1;
    r->sel = i == l->sel;
    r->used = true;
}

static bool row_same(const row_t *a, const row_t *b)
{
    return a->used && b->used && a->sel == b->sel && a->icon == b->icon && a->sw == b->sw &&
           !strcmp(a->text, b->text) && !strcmp(a->detail, b->detail);
}

static void row_draw(const row_t *rw, int x, int y, int w)
{
    int tx = x + 7, right = x + w - 6;
    int th = gfx_text_height(FONT_BODY, 1), sh = gfx_text_height(FONT_SMALL, 1);
    bool sel = rw->sel;
    uint16_t fg = sel ? C_SEL_TEXT : C_TEXT;

    gfx_round(x, y, w, CARD_H, UI_RADIUS, sel ? C_ACCENT : C_CARD);
    if (rw->icon) {
        gfx_icon(x + 4, y + (CARD_H - ICON_SIZE) / 2, rw->icon, sel ? C_SEL_TEXT : C_ACCENT);
        tx = x + 4 + ICON_SIZE + 5;
    }
    if (rw->sw >= 0) {                  /* a switch at the right */
        right -= 20;
        if (sel)                        /* keep it visible on the accent */
            gfx_round(right - 1, y + (CARD_H - 10) / 2 - 1, 22, 12, 2, C_SEL_TEXT);
        gfx_switch(right, y + (CARD_H - 10) / 2, rw->sw);
        right -= 4;
    } else if (rw->detail[0]) {
        int dw = gfx_text_width(rw->detail, FONT_SMALL, 1);

        right -= dw;
        gfx_text(right, y + (CARD_H - sh) / 2, rw->detail,
                 sel ? C_SEL_TEXT : C_DIM, FONT_SMALL, 1, 0);
        right -= 5;
    }
    gfx_text(tx, y + (CARD_H - th) / 2, rw->text, fg, FONT_BODY, 1, right - tx);
}

/* The rows' pixels moved by d rows (d > 0: up, as the list scrolls down),
 * a line at a time in the order that reads each before it is written over.
 * Through gfx_blit, so the frame stays the drawing code's to keep. */
static void rows_move(int x, int w, int d)
{
    int lines = (LIST_ROWS - (d > 0 ? d : -d)) * UI_ROW_H, by = d * UI_ROW_H;

    if (d > 0)
        for (int j = 0; j < lines; j++)
            gfx_blit(x, LIST_Y + j, w, 1, lcd_fb + (LIST_Y + j + by) * LCD_WIDTH + x);
    else
        for (int j = lines - 1; j >= 0; j--)
            gfx_blit(x, LIST_Y + j - by, w, 1, lcd_fb + (LIST_Y + j) * LCD_WIDTH + x);
    if (d > 0) {
        memmove(&drawn.row[0], &drawn.row[d], (size_t)(LIST_ROWS - d) * sizeof(row_t));
        for (int r = LIST_ROWS - d; r < LIST_ROWS; r++)
            drawn.row[r].used = false;
    } else {
        memmove(&drawn.row[-d], &drawn.row[0], (size_t)(LIST_ROWS + d) * sizeof(row_t));
        for (int r = 0; r < -d; r++)
            drawn.row[r].used = false;
    }
}

void list_draw(list_t *l)
{
    int y = LIST_Y;
    bool bar = l->count > LIST_ROWS, part;
    int x = UI_PAD, w = LCD_WIDTH - 2 * UI_PAD - (bar ? 4 : 0);
    row_t now;

    os_status.ui_list_count = (uint32_t)l->count;
    if (l->count == 0) {
        ui_draw_base();
        drawn.l = NULL;
        gfx_text_centered(UI_TOP + UI_CONTENT_H / 2 - 4, "(empty)", C_DIM,
                          FONT_BODY, 1);
        return;
    }
    if (l->sel < l->top)
        l->top = l->sel;
    if (l->sel >= l->top + LIST_ROWS)
        l->top = l->sel - LIST_ROWS + 1;

    /* the rows in lcd_fb are this list's as drawn last: only what differs */
    part = ui_partial() && drawn.l == l && drawn.count == l->count && drawn.w == w;
    if (part && drawn.top != l->top) {
        int d = l->top - drawn.top;

        /* a move by part of a page with every row there before and after */
        if (d >= LIST_ROWS || d <= -LIST_ROWS || l->top + LIST_ROWS > l->count ||
            drawn.top + LIST_ROWS > l->count)
            part = false;
        else
            rows_move(x, w, d);
    }
    if (!part) {
        ui_draw_base();                 /* a partial frame that needs it all */
        for (int r = 0; r < LIST_ROWS; r++)
            drawn.row[r].used = false;
    }
    drawn.l = l;
    drawn.count = l->count;
    drawn.top = l->top;
    drawn.w = w;

    for (int r = 0; r < LIST_ROWS; r++, y += UI_ROW_H) {
        if (l->top + r >= l->count) {
            drawn.row[r].used = false;
            continue;
        }
        row_get(l, l->top + r, &now);
        if (part && row_same(&now, &drawn.row[r]))
            continue;
        if (part)                       /* as the full draw's cleared ground */
            gfx_fill(x, y, w, CARD_H, C_BG);
        row_draw(&now, x, y, w);
        drawn.row[r] = now;
    }
    if (bar) {                          /* a thin scroll indicator */
        int track = LIST_ROWS * UI_ROW_H - 2;
        int h = track * LIST_ROWS / l->count;
        int pos = (track - (h < 8 ? 8 : h)) * l->top / (l->count - LIST_ROWS);

        /* the track over all of the old one: the same pixels a full draw has */
        gfx_fill(LCD_WIDTH - UI_PAD - 2, LIST_Y, 2, track, C_CARD);
        gfx_fill(LCD_WIDTH - UI_PAD - 2, LIST_Y + pos, 2, h < 8 ? 8 : h, C_DIM);
    }
}

uint32_t list_shown(list_t *l)
{
    uint32_t k = 2166136261u;
    row_t now;

    for (int r = 0; r < LIST_ROWS && l->top + r < l->count; r++) {
        row_get(l, l->top + r, &now);
        for (const char *p = now.text; *p; p++)
            k = (k ^ (uint8_t)*p) * 16777619u;
        k = (k ^ 1u) * 16777619u;
        for (const char *p = now.detail; *p; p++)
            k = (k ^ (uint8_t)*p) * 16777619u;
        k = (k ^ (uint32_t)(uintptr_t)now.icon) * 16777619u;
        k = (k ^ (uint32_t)(now.sw + 2)) * 16777619u;
    }
    return k ^ (uint32_t)l->count;
}

bool list_key(list_t *l, const key_event_t *ev)
{
    if (!l->count)
        return false;
    switch (ev->code) {
    case KEY_UP:
        l->sel = l->sel > 0 ? l->sel - 1 : l->count - 1;
        return true;
    case KEY_DOWN:
        l->sel = l->sel < l->count - 1 ? l->sel + 1 : 0;
        return true;
    case KEY_LEFT:                      /* a page up */
        l->sel = l->sel >= LIST_ROWS ? l->sel - LIST_ROWS : 0;
        return true;
    case KEY_RIGHT:
        l->sel = l->sel + LIST_ROWS < l->count ? l->sel + LIST_ROWS : l->count - 1;
        return true;
    }
    return false;
}

/* ---- wrapped text (by pixel width) ----------------------------------------- */

#define MAX_LINES 96

/* Line starts and lengths of text wrapped to width pixels. */
static int wrap(const char *t, int font, int width, const char **starts, int *lens,
                int max)
{
    int n = 0;

    while (*t && n < max) {
        const char *e = t;
        int len = 0, cut = -1, w = 0;
        char one[5];

        while (*e && *e != '\n') {      /* a character at a time, not a byte */
            int k = utf8_len(e);

            memcpy(one, e, (size_t)k);
            one[k] = 0;
            w += gfx_text_width(one, font, 1);
            if (w > width && len)
                break;
            if (*e == ' ')
                cut = len;
            e += k, len += k;
        }
        if (*e && *e != '\n' && cut > 0)
            len = cut;
        starts[n] = t;
        lens[n++] = len;
        t += len;
        if (*t == '\n' || *t == ' ')
            t++;
    }
    return n;
}

static int draw_lines(const char *text, int font, int first, int x, int y, int width,
                      int rows, int line_h, uint16_t c, bool centered)
{
    const char *starts[MAX_LINES];
    int lens[MAX_LINES], n = wrap(text, font, width, starts, lens, MAX_LINES);
    char line[96];

    for (int r = 0; r < rows && first + r < n; r++) {
        int len = lens[first + r] < (int)sizeof(line) - 1 ? lens[first + r]
                  : (int)utf8_cut(starts[first + r], sizeof(line) - 1);

        memcpy(line, starts[first + r], (size_t)len);
        line[len] = 0;
        if (centered)
            gfx_text(x + (width - gfx_text_width(line, font, 1)) / 2, y, line, c, font, 1, width);
        else
            gfx_text(x, y, line, c, font, 1, width);
        y += line_h;
    }
    return n;
}

/* ---- dialogs ------------------------------------------------------------- */

typedef struct {
    screen_t s;
    void (*yes)(void);
    bool no;                            /* No is the selected button */
    bool drawn_no;                      /* the one drawn last */
    int y, h;                           /* the card as drawn */
    char text[];
} dialog_t;

/* A static screen's shown key: keys and ui_redraw() redraw it */
static uint32_t shown_fixed(screen_t *s)
{
    return 0;
}

#define DLG_W   112
#define DLG_X   ((LCD_WIDTH - DLG_W) / 2)

static void button(int x, int y, int w, const char *label, bool primary)
{
    gfx_round(x, y, w, 15, UI_RADIUS, primary ? C_ACCENT : C_CARD_HI);
    gfx_text(x + (w - gfx_text_width(label, FONT_BODY, 1)) / 2,
             y + (15 - gfx_text_height(FONT_BODY, 1)) / 2, label,
             primary ? C_SEL_TEXT : C_TEXT, FONT_BODY, 1, w - 4);
}

/* Opened, the dimmed screen, the shadow, the card and its text are drawn
 * once; a Yes/No move then redraws the two buttons on the card's plain
 * ground. It redrew all of it, the list under it too, at ~680k instructions
 * a move (gfx_round_alpha 225k, gfx_shade 154k; bench 2026-11-17). */
static void dialog_draw(screen_t *s)
{
    dialog_t *d = (dialog_t *)s;
    const char *starts[MAX_LINES];
    int lens[MAX_LINES];
    int lines, h, y;

    if (ui_partial()) {                 /* the frame is this dialog's last */
        if (d->yes && d->no != d->drawn_no) {
            y = d->y + d->h - 20;
            gfx_fill(DLG_X + 6, y, 44, 15, C_CARD);
            gfx_fill(DLG_X + DLG_W - 50, y, 44, 15, C_CARD);
            button(DLG_X + 6, y, 44, "Yes", !d->no);
            button(DLG_X + DLG_W - 50, y, 44, "No", d->no);
            d->drawn_no = d->no;
        }
        return;
    }
    lines = wrap(d->text, FONT_BODY, DLG_W - 16, starts, lens, MAX_LINES);
    if (lines > 5)
        lines = 5;
    h = 20 + lines * 12 + 24;
    y = UI_TOP + (UI_CONTENT_H - h) / 2;
    d->y = y;
    d->h = h;
    d->drawn_no = d->no;
    /* the screen under it, dimmed, and a hard drop shadow */
    gfx_shade(0, UI_TOP, LCD_WIDTH, UI_CONTENT_H, RGB(0, 0, 0), 120);
    gfx_round_alpha(DLG_X + 3, y + 3, DLG_W, h, UI_RADIUS, RGB(0, 0, 0), 150);
    gfx_round(DLG_X, y, DLG_W, h, UI_RADIUS + 1, C_CARD);
    gfx_fill(DLG_X, y, DLG_W, 2, C_ACCENT);
    gfx_text(DLG_X + 7, y + 7, s->title ? s->title : "", C_TEXT, FONT_LARGE, 1, DLG_W - 14);
    draw_lines(d->text, FONT_BODY, 0, DLG_X + 7, y + 22, DLG_W - 14, lines, 12, C_DIM, false);
    if (d->yes) {                       /* where their soft keys are */
        button(DLG_X + 6, y + h - 20, 44, "Yes", !d->no);
        button(DLG_X + DLG_W - 50, y + h - 20, 44, "No", d->no);
    } else {
        button(DLG_X + (DLG_W - 50) / 2, y + h - 20, 50, "OK", true);
    }
}

static void dialog_key(screen_t *s, const key_event_t *ev)
{
    dialog_t *d = (dialog_t *)s;
    void (*yes)(void) = d->yes;

    if (yes) {
        switch (ev->code) {
        case KEY_LEFT: case KEY_RIGHT: case KEY_UP: case KEY_DOWN:
            d->no = !d->no;             /* two buttons: any direction moves; */
            break;                      /* the key's frame draws them */
        case KEY_CENTER: {              /* the selected one */
            bool no = d->no;            /* ui_pop frees d */

            ui_pop();
            if (!no)
                yes();
            break;
        }
        case KEY_LSOFT:                 /* the soft keys under Yes and No */
            ui_pop();
            yes();
            break;
        case KEY_RSOFT:
            ui_pop();
            break;
        default:
            break;
        }
    } else {
        ui_pop();                       /* a message: any key */
    }
}

static void dialog(const char *title, const char *message, void (*yes)(void))
{
    size_t n = strlen(message) + 1;
    dialog_t *d = kzalloc(sizeof(*d) + n);

    if (!d)
        return;
    memcpy(d->text, message, n);
    d->s.title = title;
    /* the buttons are drawn in the dialog, over their soft keys */
    d->s.left = d->s.center = d->s.right = NULL;
    d->s.draw = dialog_draw;
    d->s.key = dialog_key;
    d->s.free_on_pop = true;
    d->s.overlay = true;
    d->yes = yes;
    ui_screen_shown(&d->s, shown_fixed);
    ui_screen_partial(&d->s);
    ui_push(&d->s);
}

void ui_confirm(const char *title, const char *message, void (*yes)(void))
{
    dialog(title, message, yes);
}

void ui_message(const char *title, const char *message)
{
    dialog(title, message, NULL);
}

/* ---- text view ------------------------------------------------------------- */

#define VIEW_LINE  11
#define VIEW_ROWS  ((UI_CONTENT_H - 4) / VIEW_LINE)
#define VIEW_W     (LCD_WIDTH - 2 * UI_PAD - 4)

typedef struct {
    screen_t s;
    int first, lines;
    char text[];
} view_t;

static void view_draw(screen_t *s)
{
    view_t *v = (view_t *)s;

    draw_lines(v->text, FONT_BODY, v->first, UI_PAD + 2, UI_TOP + 3, VIEW_W,
               VIEW_ROWS, VIEW_LINE, C_TEXT, false);
    if (v->lines > VIEW_ROWS) {
        int track = UI_CONTENT_H - 6, h = track * VIEW_ROWS / v->lines;
        int pos = (track - h) * v->first / (v->lines - VIEW_ROWS);

        gfx_round(LCD_WIDTH - UI_PAD - 2, UI_TOP + 3, 2, track, 1, C_CARD);
        gfx_round(LCD_WIDTH - UI_PAD - 2, UI_TOP + 3 + pos, 2, h < 8 ? 8 : h, 1, C_DIM);
    }
}

static void view_key(screen_t *s, const key_event_t *ev)
{
    view_t *v = (view_t *)s;
    int max = v->lines > VIEW_ROWS ? v->lines - VIEW_ROWS : 0;

    switch (ev->code) {
    case KEY_UP:    v->first -= 1; break;
    case KEY_DOWN:  v->first += 1; break;
    case KEY_LEFT:  v->first -= VIEW_ROWS; break;
    case KEY_RIGHT: v->first += VIEW_ROWS; break;
    case KEY_RSOFT:
    case KEY_CENTER:
        ui_pop();
        return;
    }
    if (v->first > max) v->first = max;
    if (v->first < 0) v->first = 0;
}

void ui_text_view(const char *title, const char *text)
{
    size_t n = strlen(text) + 1;
    view_t *v = kzalloc(sizeof(*v) + n + 4);    /* + room for the "..." of a cut */
    const char *starts[MAX_LINES];
    int lens[MAX_LINES];

    if (!v)
        return;
    memcpy(v->text, text, n);
    v->lines = wrap(v->text, FONT_BODY, VIEW_W, starts, lens, MAX_LINES);
    if (v->lines == MAX_LINES) {        /* more than it wraps: the last line says so */
        const char *rest = starts[MAX_LINES - 1] + lens[MAX_LINES - 1];

        if (*rest == '\n' || *rest == ' ')
            rest++;
        if (*rest)
            memcpy((char *)starts[MAX_LINES - 1], "...", 4);
    }
    v->s.title = title;
    v->s.right = "Back";
    v->s.draw = view_draw;
    v->s.key = view_key;
    v->s.free_on_pop = true;
    ui_screen_shown(&v->s, shown_fixed);    /* a copy: it changes by keys only */
    ui_push(&v->s);
}

/* ---- the option picker ------------------------------------------------------ */

typedef struct {
    screen_t s;
    list_t list;
    const char *const *options;
    int current;
    void (*picked)(int i, void *ctx);
    void *ctx;
} picker_t;

static void picker_item(void *ctx, int i, char *text, int tn, char *detail, int dn)
{
    picker_t *p = ctx;

    snprintf(text, (size_t)tn, "%s", p->options[i]);
}

static const icon_t *picker_icon(void *ctx, int i)
{
    picker_t *p = ctx;

    return i == p->current ? &icon_dot_on : &icon_dot_off;
}

static void picker_draw(screen_t *s)
{
    list_draw(&((picker_t *)s->data)->list);
}

/* the options are the caller's: drawn again if their texts change */
static uint32_t picker_shown(screen_t *s)
{
    return list_shown(&((picker_t *)s->data)->list);
}

static void picker_key(screen_t *s, const key_event_t *ev)
{
    picker_t *p = s->data;

    if (list_key(&p->list, ev))
        return;
    if (ev->code == KEY_CENTER || ev->code == KEY_LSOFT) {
        void (*picked)(int, void *) = p->picked;
        void *ctx = p->ctx;
        int i = p->list.sel;

        ui_pop();                       /* frees p */
        if (picked)
            picked(i, ctx);
    } else if (ev->code == KEY_RSOFT) {
        ui_pop();
    }
}

/* A list of options, the current one marked; Center picks one and calls
 * picked(i, ctx) after the picker has closed, RSoft leaves it as it was.
 * title and options must outlive the picker (string constants do). */
void ui_choose(const char *title, const char *const *options, int n, int current,
               void (*picked)(int i, void *ctx), void *ctx)
{
    picker_t *p = kzalloc(sizeof(*p));

    if (!p)
        return;
    p->options = options;
    p->current = current;
    p->picked = picked;
    p->ctx = ctx;
    p->list.count = n;
    p->list.sel = current >= 0 && current < n ? current : 0;
    p->list.item = picker_item;
    p->list.icon = picker_icon;
    p->list.ctx = p;
    p->s.title = title;
    p->s.left = "Select";
    p->s.center = "OK";
    p->s.right = "Back";
    p->s.draw = picker_draw;
    p->s.key = picker_key;
    p->s.data = p;
    p->s.free_on_pop = true;
    ui_screen_shown(&p->s, picker_shown);
    ui_screen_partial(&p->s);           /* its draw is a list_draw */
    ui_push(&p->s);
}
