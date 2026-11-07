/* B310E OS — text entry: a multi-tap editor screen.
 *
 * The phone's keys as on any feature phone: tap a digit key again within
 * 0.9 s for its next letter (2: a b c 2 ...), 1 for punctuation, 0 for a
 * space, * for the mode (Abc, abc, ABC, 123), # for a new line (in a
 * multi-line text). Left and Right move the cursor, Up and Down a line;
 * LSoft deletes the character before it, Center saves (done() gets the
 * text), RSoft leaves without saving.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/kernel.h"
#include "os/lib.h"
#include "os/ui.h"

#define TAP_MS     900
#define LINE_H     12
#define MAX_LINES  256
#define TEXT_X     (UI_PAD + 3)
#define TEXT_W     (LCD_WIDTH - 2 * TEXT_X)

enum { MODE_ABC1, MODE_ABC, MODE_ABC_UP, MODE_NUM, MODE_COUNT };

static const char *const taps[10] = {
    " 0", ".,?!'-_()@/:;1", "abc2", "def3", "ghi4",
    "jkl5", "mno6", "pqrs7", "tuv8", "wxyz9",
};

typedef struct {
    screen_t s;
    void (*done)(const char *text, void *ctx);
    void *ctx;
    bool multiline;
    int max, len, cur, mode, top;
    int tap_key, tap_index;             /* the letter being chosen */
    uint32_t tap_at;
    int line_start[MAX_LINES], lines;
    char text[];                        /* max + 1 */
} edit_t;

static int char_w(char c)
{
    char s[2] = { c, 0 };

    return gfx_text_width(s, FONT_BODY, 1);
}

/* Lines as they wrap: at a '\n', or at the last space before the width
 * runs out (mid-word when a word is wider than the line). */
static void layout(edit_t *e)
{
    int start = 0, w = 0, space = -1;

    e->lines = 0;
    e->line_start[e->lines++] = 0;
    for (int i = 0; i < e->len && e->lines < MAX_LINES; i++) {
        char c = e->text[i];

        if (c == '\n') {
            start = i + 1;
            w = 0;
            space = -1;
            e->line_start[e->lines++] = start;
            continue;
        }
        w += char_w(c);
        if (c == ' ')
            space = i;
        if (w > TEXT_W && i > start) {
            start = space >= start ? space + 1 : i;
            e->line_start[e->lines++] = start;
            w = 0;
            for (int j = start; j <= i; j++)
                w += char_w(e->text[j]);
            space = -1;
        }
    }
}

static int line_of(edit_t *e, int pos)
{
    int l = 0;

    while (l + 1 < e->lines && e->line_start[l + 1] <= pos)
        l++;
    return l;
}

static int line_end(edit_t *e, int l)
{
    int end = l + 1 < e->lines ? e->line_start[l + 1] : e->len;

    if (end > e->line_start[l] && end <= e->len && e->text[end - 1] == '\n')
        end--;
    return end;
}

static int text_width(edit_t *e, int from, int to)
{
    int w = 0;

    for (int i = from; i < to; i++)
        w += char_w(e->text[i]);
    return w;
}

static void draw(screen_t *s)
{
    edit_t *e = s->data;
    static const char *const names[MODE_COUNT] = { "Abc", "abc", "ABC", "123" };
    int rows = (UI_CONTENT_H - 8) / LINE_H, cl;
    char tag[16];

    layout(e);
    cl = line_of(e, e->cur);
    if (cl < e->top)
        e->top = cl;
    if (cl >= e->top + rows)
        e->top = cl - rows + 1;

    gfx_round(UI_PAD, UI_TOP + 2, LCD_WIDTH - 2 * UI_PAD, UI_CONTENT_H - 4, UI_RADIUS, C_CARD);
    for (int r = 0; r < rows && e->top + r < e->lines; r++) {
        int l = e->top + r, a = e->line_start[l], b = line_end(e, l);
        int y = UI_TOP + 6 + r * LINE_H;
        char buf[64];
        int n = b - a < (int)sizeof(buf) - 1 ? b - a : (int)sizeof(buf) - 1;

        memcpy(buf, e->text + a, (size_t)n);
        buf[n] = 0;
        gfx_text(TEXT_X, y, buf, C_TEXT, FONT_BODY, 1, 0);
        if (l == cl) {                  /* the cursor, and the letter being chosen */
            int x = TEXT_X + text_width(e, a, e->cur);

            if (e->tap_key >= 0 && e->cur > a)
                gfx_fill(x - char_w(e->text[e->cur - 1]), y + 9, char_w(e->text[e->cur - 1]), 1,
                         C_ACCENT);
            gfx_fill(x, y - 2, 1, 11, C_ACCENT);
        }
    }
    snprintf(tag, sizeof(tag), "%s %d", names[e->mode], e->max - e->len);
    gfx_text(LCD_WIDTH - UI_PAD - 3 - gfx_text_width(tag, FONT_SMALL, 1), UI_BOTTOM - 10,
             tag, C_DIM, FONT_SMALL, 1, 0);
}

static void insert(edit_t *e, char c)
{
    if (e->len >= e->max)
        return;
    memmove(e->text + e->cur + 1, e->text + e->cur, (size_t)(e->len - e->cur + 1));
    e->text[e->cur++] = c;
    e->len++;
}

static char cased(edit_t *e, char c)
{
    if (c >= 'a' && c <= 'z' && (e->mode == MODE_ABC_UP || e->mode == MODE_ABC1))
        return (char)(c - 32);
    return c;
}

static void digit(edit_t *e, int k, uint32_t now)
{
    const char *set = taps[k];
    int n = (int)strlen(set);

    if (e->mode == MODE_NUM) {
        e->tap_key = -1;
        insert(e, (char)('0' + k));
        return;
    }
    if (e->tap_key == k && now - e->tap_at < TAP_MS && e->cur > 0) {
        e->tap_index = (e->tap_index + 1) % n;
        e->text[e->cur - 1] = cased(e, set[e->tap_index]);
    } else {
        if (e->tap_key >= 0 && e->mode == MODE_ABC1 && e->cur > 0 &&
            e->text[e->cur - 1] != ' ')
            e->mode = MODE_ABC;         /* one capital, then small */
        e->tap_key = k;
        e->tap_index = 0;
        insert(e, cased(e, set[0]));
    }
    e->tap_at = now;
}

static void end_tap(edit_t *e)
{
    if (e->tap_key >= 0 && e->mode == MODE_ABC1 && e->cur > 0 && e->text[e->cur - 1] != ' ')
        e->mode = MODE_ABC;
    e->tap_key = -1;
}

static void tick(screen_t *s)
{
    edit_t *e = s->data;

    if (e->tap_key >= 0 && kernel_ticks() - e->tap_at >= TAP_MS)
        end_tap(e);
}

static void move_line(edit_t *e, int d)
{
    int l, x, a, b, pos;

    layout(e);
    l = line_of(e, e->cur) + d;
    if (l < 0 || l >= e->lines)
        return;
    x = text_width(e, e->line_start[line_of(e, e->cur)], e->cur);
    a = e->line_start[l];
    b = line_end(e, l);
    for (pos = a; pos < b && text_width(e, a, pos + 1) <= x; pos++)
        ;
    e->cur = pos;
}

static void key(screen_t *s, const key_event_t *ev)
{
    edit_t *e = s->data;
    uint32_t now = kernel_ticks();

    if (ev->code >= KEY_0 && ev->code <= KEY_9) {
        digit(e, ev->code - KEY_0, now);
        return;
    }
    end_tap(e);
    switch (ev->code) {
    case KEY_STAR:
        e->mode = (e->mode + 1) % MODE_COUNT;
        break;
    case KEY_HASH:
        if (e->multiline)
            insert(e, '\n');
        break;
    case KEY_LEFT:
        if (e->cur > 0)
            e->cur--;
        break;
    case KEY_RIGHT:
        if (e->cur < e->len)
            e->cur++;
        break;
    case KEY_UP:
        move_line(e, -1);
        break;
    case KEY_DOWN:
        move_line(e, 1);
        break;
    case KEY_LSOFT:
        if (e->cur > 0) {
            memmove(e->text + e->cur - 1, e->text + e->cur, (size_t)(e->len - e->cur + 1));
            e->cur--;
            e->len--;
        }
        break;
    case KEY_CENTER:
        if (e->done)
            e->done(e->text, e->ctx);
        ui_pop();                       /* frees e */
        return;
    case KEY_RSOFT:
        ui_pop();
        return;
    }
}

void ui_text_edit(const char *title, const char *initial, int max, bool multiline,
                  void (*done)(const char *text, void *ctx), void *ctx)
{
    edit_t *e = kzalloc(sizeof(*e) + (size_t)max + 1);
    int n = initial ? (int)strlen(initial) : 0;

    if (!e)
        return;
    if (n > max)
        n = max;
    memcpy(e->text, initial ? initial : "", (size_t)n);
    e->text[n] = 0;
    e->len = e->cur = n;
    e->max = max;
    e->multiline = multiline;
    e->done = done;
    e->ctx = ctx;
    e->tap_key = -1;
    e->mode = n ? MODE_ABC : MODE_ABC1;
    e->s.title = title;
    e->s.left = "Del";
    e->s.center = "Save";
    e->s.right = "Back";
    e->s.draw = draw;
    e->s.key = key;
    e->s.tick = tick;
    e->s.tick_ms = 100;
    e->s.data = e;
    e->s.free_on_pop = true;
    ui_push(&e->s);
}
