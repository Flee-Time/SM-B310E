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

void list_draw(list_t *l)
{
    char text[48], detail[24];
    int y = UI_TOP + (UI_CONTENT_H - LIST_ROWS * UI_ROW_H) / 2 + 1;
    bool bar = l->count > LIST_ROWS;
    int x = UI_PAD, w = LCD_WIDTH - 2 * UI_PAD - (bar ? 4 : 0);
    int th = gfx_text_height(FONT_BODY, 1), sh = gfx_text_height(FONT_SMALL, 1);

    os_status.ui_list_count = (uint32_t)l->count;
    if (l->count == 0) {
        gfx_text_centered(UI_TOP + UI_CONTENT_H / 2 - 4, "(empty)", C_DIM,
                          FONT_BODY, 1);
        return;
    }
    if (l->sel < l->top)
        l->top = l->sel;
    if (l->sel >= l->top + LIST_ROWS)
        l->top = l->sel - LIST_ROWS + 1;

    for (int r = 0; r < LIST_ROWS && l->top + r < l->count; r++) {
        int i = l->top + r, tx = x + 7, right = x + w - 6, sw = -1;
        bool sel = i == l->sel;
        const icon_t *icon = l->icon ? l->icon(l->ctx, i) : NULL;
        uint16_t fg = sel ? C_SEL_TEXT : C_TEXT;

        text[0] = detail[0] = 0;
        l->item(l->ctx, i, text, sizeof(text), detail, sizeof(detail));
        gfx_round(x, y, w, CARD_H, UI_RADIUS, sel ? C_ACCENT : C_CARD);
        if (icon) {
            gfx_icon(x + 4, y + (CARD_H - ICON_SIZE) / 2, icon, sel ? C_SEL_TEXT : C_ACCENT);
            tx = x + 4 + ICON_SIZE + 5;
        }
        if (l->toggle)
            sw = l->toggle(l->ctx, i);
        if (sw >= 0) {                  /* a switch at the right */
            right -= 20;
            if (sel)                    /* keep it visible on the accent */
                gfx_round(right - 1, y + (CARD_H - 10) / 2 - 1, 22, 12, 2, C_SEL_TEXT);
            gfx_switch(right, y + (CARD_H - 10) / 2, sw);
            right -= 4;
        } else if (detail[0]) {
            int dw = gfx_text_width(detail, FONT_SMALL, 1);

            right -= dw;
            gfx_text(right, y + (CARD_H - sh) / 2, detail,
                     sel ? C_SEL_TEXT : C_DIM, FONT_SMALL, 1, 0);
            right -= 5;
        }
        gfx_text(tx, y + (CARD_H - th) / 2, text, fg, FONT_BODY, 1, right - tx);
        y += UI_ROW_H;
    }
    if (bar) {                          /* a thin scroll indicator */
        int track = LIST_ROWS * UI_ROW_H - 2;
        int top = UI_TOP + (UI_CONTENT_H - LIST_ROWS * UI_ROW_H) / 2 + 1;
        int h = track * LIST_ROWS / l->count;
        int pos = (track - (h < 8 ? 8 : h)) * l->top / (l->count - LIST_ROWS);

        gfx_fill(LCD_WIDTH - UI_PAD - 2, top, 2, track, C_CARD);
        gfx_fill(LCD_WIDTH - UI_PAD - 2, top + pos, 2, h < 8 ? 8 : h, C_DIM);
    }
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
        char one[2] = { 0, 0 };

        while (*e && *e != '\n') {
            one[0] = *e;
            w += gfx_text_width(one, font, 1);
            if (w > width && len)
                break;
            if (*e == ' ')
                cut = len;
            e++, len++;
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
                                                           : (int)sizeof(line) - 1;

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
    char text[];
} dialog_t;

#define DLG_W   112
#define DLG_X   ((LCD_WIDTH - DLG_W) / 2)

static void button(int x, int y, int w, const char *label, bool primary)
{
    gfx_round(x, y, w, 15, UI_RADIUS, primary ? C_ACCENT : C_CARD_HI);
    gfx_text(x + (w - gfx_text_width(label, FONT_BODY, 1)) / 2,
             y + (15 - gfx_text_height(FONT_BODY, 1)) / 2, label,
             primary ? C_SEL_TEXT : C_TEXT, FONT_BODY, 1, w - 4);
}

static void dialog_draw(screen_t *s)
{
    dialog_t *d = (dialog_t *)s;
    const char *starts[MAX_LINES];
    int lens[MAX_LINES];
    int lines = wrap(d->text, FONT_BODY, DLG_W - 16, starts, lens, MAX_LINES);
    int h, y;

    if (lines > 5)
        lines = 5;
    h = 20 + lines * 12 + 24;
    y = UI_TOP + (UI_CONTENT_H - h) / 2;
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
            d->no = !d->no;             /* two buttons: any direction moves */
            ui_redraw();
            break;
        case KEY_CENTER:                /* the selected one */
            ui_pop();
            if (!d->no)
                yes();
            break;
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
    view_t *v = kzalloc(sizeof(*v) + n);
    const char *starts[MAX_LINES];
    int lens[MAX_LINES];

    if (!v)
        return;
    memcpy(v->text, text, n);
    v->lines = wrap(v->text, FONT_BODY, VIEW_W, starts, lens, MAX_LINES);
    v->s.title = title;
    v->s.right = "Back";
    v->s.draw = view_draw;
    v->s.key = view_key;
    v->s.free_on_pop = true;
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
    ui_push(&p->s);
}
