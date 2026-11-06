/* B310E OS — list, dialogs, text view.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/kernel.h"
#include "os/lib.h"
#include "os/status.h"
#include "os/ui.h"

/* ---- list ------------------------------------------------------------------ */

#define LIST_ROWS (UI_CONTENT_H / UI_ROW_H)

void list_draw(list_t *l)
{
    char text[48], detail[24];
    int y = UI_TOP + (UI_CONTENT_H - LIST_ROWS * UI_ROW_H) / 2;
    bool bar = l->count > LIST_ROWS;
    int width = LCD_WIDTH - (bar ? 4 : 0);

    os_status.ui_list_count = (uint32_t)l->count;
    if (l->count == 0) {
        gfx_text_centered(UI_TOP + UI_CONTENT_H / 2 - 4, "(empty)", C_DIM,
                          FONT_SMALL, 1);
        return;
    }
    if (l->sel < l->top)
        l->top = l->sel;
    if (l->sel >= l->top + LIST_ROWS)
        l->top = l->sel - LIST_ROWS + 1;

    for (int r = 0; r < LIST_ROWS && l->top + r < l->count; r++) {
        int i = l->top + r, dw;
        bool sel = i == l->sel;

        text[0] = detail[0] = 0;
        l->item(l->ctx, i, text, sizeof(text), detail, sizeof(detail));
        if (sel)
            gfx_fill(0, y, width, UI_ROW_H, C_ACCENT);
        dw = detail[0] ? gfx_text_width(detail, FONT_SMALL, 1) + 4 : 0;
        if (dw)
            gfx_text(width - dw, y + 4, detail, sel ? C_SEL_TEXT : C_DIM,
                     FONT_SMALL, 1, 0);
        gfx_text(4, y, text, sel ? C_SEL_TEXT : C_TEXT, FONT_LARGE, 1,
                 width - 6 - dw);
        y += UI_ROW_H;
    }
    if (bar) {
        int track = LIST_ROWS * UI_ROW_H, top = UI_TOP + (UI_CONTENT_H - track) / 2;
        int h = track * LIST_ROWS / l->count;
        int pos = (track - h) * l->top / (l->count - LIST_ROWS);

        gfx_fill(LCD_WIDTH - 3, top, 2, track, C_BAR);
        gfx_fill(LCD_WIDTH - 3, top + pos, 2, h < 4 ? 4 : h, C_DIM);
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

/* ---- wrapped text --------------------------------------------------------- */

#define COLS (LCD_WIDTH / 6)

/* Line starts of text wrapped at COLS small-font columns. */
static int wrap(const char *t, const char **starts, int *lens, int max)
{
    int n = 0;

    while (*t && n < max) {
        const char *e = t;
        int len = 0, cut = -1;

        while (*e && *e != '\n' && len < COLS) {
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

static void draw_lines(const char *text, int first, int y, int rows, uint16_t c,
                       bool centered)
{
    const char *starts[64];
    int lens[64], n = wrap(text, starts, lens, 64);
    char line[COLS + 1];

    for (int r = 0; r < rows && first + r < n; r++) {
        memcpy(line, starts[first + r], (size_t)lens[first + r]);
        line[lens[first + r]] = 0;
        if (centered)
            gfx_text_centered(y, line, c, FONT_SMALL, 1);
        else
            gfx_text(2, y, line, c, FONT_SMALL, 1, 0);
        y += 10;
    }
}

/* ---- dialogs ------------------------------------------------------------- */

typedef struct {
    screen_t s;
    void (*yes)(void);
    char text[];
} dialog_t;

static void dialog_draw(screen_t *s)
{
    dialog_t *d = (dialog_t *)s;

    draw_lines(d->text, 0, UI_TOP + 24, 8, C_TEXT, true);
}

static void dialog_key(screen_t *s, const key_event_t *ev)
{
    dialog_t *d = (dialog_t *)s;
    void (*yes)(void) = d->yes;

    if (yes) {
        if (ev->code == KEY_LSOFT || ev->code == KEY_CENTER) {
            ui_pop();
            yes();
        } else if (ev->code == KEY_RSOFT) {
            ui_pop();
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
    d->s.left = yes ? "Yes" : NULL;
    d->s.center = yes ? NULL : "OK";
    d->s.right = yes ? "No" : NULL;
    d->s.draw = dialog_draw;
    d->s.key = dialog_key;
    d->s.free_on_pop = true;
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

#define VIEW_ROWS (UI_CONTENT_H / 10)

typedef struct {
    screen_t s;
    int first, lines;
    char text[];
} view_t;

static void view_draw(screen_t *s)
{
    view_t *v = (view_t *)s;

    draw_lines(v->text, v->first, UI_TOP + 3, VIEW_ROWS, C_TEXT, false);
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
    const char *starts[64];
    int lens[64];

    if (!v)
        return;
    memcpy(v->text, text, n);
    v->lines = wrap(v->text, starts, lens, 64);
    v->s.title = title;
    v->s.right = "Back";
    v->s.draw = view_draw;
    v->s.key = view_key;
    v->s.free_on_pop = true;
    ui_push(&v->s);
}
