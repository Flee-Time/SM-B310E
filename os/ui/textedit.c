/* B310E OS — text entry: a multi-tap editor screen, in several input
 * languages (ui/textlang.c), with a symbol picker and word completion.
 *
 * The keys, a short press / one held 0.6 s (UI_MODE_HOLD_KEYS):
 *   2-9    the language's letters: tap again within 0.9 s for the next
 *          (Turkish 4: g h ı i ğ 4) / the digit
 *   1      punctuation, the same way (. , ? ! ' " - ( ) @ / : ...) / 1
 *   0      a space, then 0 / 0
 *   *      the mode: Abc, abc, ABC, 123 / the symbol picker (the language's
 *          letters, punctuation, money, maths: the D-pad moves, Center picks)
 *   #      a new line (in a multi-line text) / the next input language
 *   Dial   takes the word offered; again at once: the next one, and after the
 *          last the word as typed
 * In 123 a digit key is its digit either way. Left and Right move the cursor,
 * Up and Down a line; LSoft deletes the character before it, Center saves
 * (done() gets the text), RSoft leaves without saving.
 *
 * The text is UTF-8: the cursor, Delete and the letter being chosen move by
 * whole characters, and a character that would pass the limit (bytes) is not
 * typed. Completion looks at the word before the cursor (two letters or more)
 * a tick after the key was drawn, and offers up to three words in the bottom
 * line (ui/words.c); it learns a word of three letters or more once a space or
 * a sign follows it. Off in Settings, in 123 and for UI_TEXT_PLAIN fields.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/db.h"
#include "os/kernel.h"
#include "os/lib.h"
#include "os/ui.h"
#include "ui/textlang.h"

#define TAP_MS     900
#define LOOK_MS    20                   /* completion: the key drawn first */
#define LINE_H     12
#define MAX_LINES  256
#define SUGGEST    3
#define TEXT_X     (UI_PAD + 3)
#define TEXT_W     (LCD_WIDTH - 2 * TEXT_X)
#define STRIP_Y    (UI_BOTTOM - 12)     /* the mode, and the words offered */

enum { MODE_ABC1, MODE_ABC, MODE_ABC_UP, MODE_NUM, MODE_COUNT };

typedef struct {
    screen_t s;
    void (*done)(const char *text, void *ctx);
    void *ctx;
    unsigned flags;
    int max, len, cur, mode, top;
    int tap_key, tap_index, tap_len;    /* the letter being chosen: its key, which, its bytes */
    uint32_t tap_at, key_at;
    int lang;                           /* input_langs[] */
    bool typed;                         /* the word before the cursor was typed here */
    bool look;                          /* completions wanted at the next tick */
    int nsugg, cycle;                   /* cycle: the one Dial put in, -1 none */
    int cyc_at;                         /* where that word starts */
    char cyc_word[WORD_MAX];            /* the word as typed */
    char sugg[SUGGEST][WORD_MAX];       /* as they would be typed, the case applied */
    int line_start[MAX_LINES], lines;
    bool laid;                          /* line_start is for the text as it is */
    char text[];                        /* max + 1 */
} edit_t;

static bool tail(char c)
{
    return ((uint8_t)c & 0xc0) == 0x80;
}

/* the width of the character at s; 0 on a tail byte */
static int char_w(const char *s)
{
    char one[5];
    int n;

    if (tail(*s))
        return 0;
    n = utf8_len(s);
    memcpy(one, s, (size_t)n);
    one[n] = 0;
    return gfx_text_width(one, FONT_BODY, 1);
}

static int before(const edit_t *e, int pos)     /* the start of the character before pos */
{
    while (pos > 0 && tail(e->text[--pos]))
        ;
    return pos;
}

static uint32_t char_at(const edit_t *e, int pos)
{
    const char *p = e->text + pos;

    return pos < e->len ? utf8_get(&p) : 0;
}

/* Lines as they wrap: at a '\n', or at the last space before the width
 * runs out (mid-word when a word is wider than the line). */
static void layout(edit_t *e)
{
    int start = 0, w = 0, space = -1;

    /* once per edit, not per frame: every character measured on every draw
     * was 3,529 gfx_text_width calls in a 24-key session (bench 2026-11-17) */
    if (e->laid)
        return;
    e->laid = true;
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
        w += char_w(e->text + i);
        if (c == ' ')
            space = i;
        if (w > TEXT_W && i > start) {
            start = space >= start ? space + 1 : i;
            e->line_start[e->lines++] = start;
            w = 0;
            for (int j = start; j <= i; j++)
                w += char_w(e->text + j);
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
        w += char_w(e->text + i);
    return w;
}

static bool completing(const edit_t *e)
{
    return ui_settings.completion && !(e->flags & (UI_TEXT_PLAIN | UI_TEXT_NUMBER)) &&
           e->mode != MODE_NUM;
}

/* the bottom line: the words offered at the left, language and mode at the right */
static void draw_strip(edit_t *e)
{
    static const char *const names[MODE_COUNT] = { "Abc", "abc", "ABC", "123" };
    int next = e->cycle + 1, x = TEXT_X, right;
    char tag[20];

    /* the bytes left only near the limit: the words offered need the room */
    snprintf(tag, sizeof(tag), "%s%s%s", e->mode == MODE_NUM ? "" : input_langs[e->lang].tag,
             e->mode == MODE_NUM ? "" : " ", names[e->mode]);
    if (e->max - e->len < 100)
        snprintf(tag + strlen(tag), sizeof(tag) - strlen(tag), " %d", e->max - e->len);
    right = LCD_WIDTH - UI_PAD - 3 - gfx_text_width(tag, FONT_SMALL, 1);
    gfx_text(right, STRIP_Y + 2, tag, C_DIM, FONT_SMALL, 1, 0);
    for (int i = 0; i < e->nsugg && x < right - 12; i++) {
        uint16_t c = i == next ? C_ACCENT : C_DIM;

        x += gfx_text(x, STRIP_Y + 2, e->sugg[i], c, FONT_SMALL, 1, right - 6 - x) + 6;
    }
}

static void draw(screen_t *s)
{
    edit_t *e = s->data;
    int rows = (STRIP_Y - UI_TOP - 6) / LINE_H, cl;

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
        char buf[128];
        int n = b - a < (int)sizeof(buf) - 1 ? b - a : (int)utf8_cut(e->text + a, sizeof(buf) - 1);

        memcpy(buf, e->text + a, (size_t)n);
        buf[n] = 0;
        gfx_text(TEXT_X, y, buf, C_TEXT, FONT_BODY, 1, 0);
        if (l == cl) {                  /* the cursor, and the letter being chosen */
            int x = TEXT_X + text_width(e, a, e->cur);

            if (e->tap_key >= 0 && e->cur > a) {
                int w = char_w(e->text + before(e, e->cur));

                gfx_fill(x - w, y + 9, w, 1, C_ACCENT);
            }
            gfx_fill(x, y - 2, 1, 11, C_ACCENT);
        }
    }
    draw_strip(e);
}

/* ---- changing the text ------------------------------------------------------- */

static bool insert(edit_t *e, const char *s, int n)
{
    if (e->len + n > e->max)
        return false;
    memmove(e->text + e->cur + n, e->text + e->cur, (size_t)(e->len - e->cur + 1));
    memcpy(e->text + e->cur, s, (size_t)n);
    e->cur += n;
    e->len += n;
    e->laid = false;
    return true;
}

static void remove_before(edit_t *e, int n)
{
    memmove(e->text + e->cur - n, e->text + e->cur, (size_t)(e->len - e->cur + 1));
    e->cur -= n;
    e->len -= n;
    e->laid = false;
}

/* The start of the word that ends at pos */
static int word_start(const edit_t *e, int pos)
{
    while (pos > 0) {
        int p = before(e, pos);

        if (!text_is_letter(char_at(e, p)))
            break;
        pos = p;
    }
    return pos;
}

/* text[a, b) in small letters into out (WORD_MAX); its characters, 0 if it
 * does not fit */
static int fold(const edit_t *e, int a, int b, char *out)
{
    const char *p = e->text + a;
    int n = 0, chars = 0;

    while (p < e->text + b) {
        char buf[4];
        int k = utf8_put(text_lower(&input_langs[e->lang], utf8_get(&p)), buf);

        if (n + k >= WORD_MAX)
            return 0;
        memcpy(out + n, buf, (size_t)k);
        n += k;
        chars++;
    }
    out[n] = 0;
    return chars;
}

/* The word just typed before the cursor is learned (its use counted) */
static void learn(edit_t *e)
{
    char w[WORD_MAX];
    int a;

    if (!e->typed || !completing(e))
        return;
    e->typed = false;
    if (e->cur > e->len || (e->cur < e->len && text_is_letter(char_at(e, e->cur))))
        return;
    a = word_start(e, e->cur);
    if (fold(e, a, e->cur, w) >= WORD_MIN)
        words_learn(input_langs[e->lang].code, w);
}

/* One character typed (or picked): a sign or a space ends the word before it */
static bool put(edit_t *e, uint32_t c)
{
    char buf[4];
    bool letter = text_is_letter(c);

    if (!letter)
        learn(e);
    if (!insert(e, buf, utf8_put(c, buf)))
        return false;
    e->typed = letter;
    return true;
}

static uint32_t cased(edit_t *e, uint32_t c)
{
    if (e->mode == MODE_ABC_UP || e->mode == MODE_ABC1)
        return text_upper(&input_langs[e->lang], c);
    return c;
}

static uint32_t nth(const char *set, int i, int *count)
{
    uint32_t c = 0;
    int n = 0;

    while (*set) {
        uint32_t x = utf8_get(&set);

        if (n++ == i)
            c = x;
    }
    *count = n;
    return c;
}

static void end_tap(edit_t *e)
{
    if (e->tap_key >= 0 && e->mode == MODE_ABC1 && e->cur > 0 && e->text[e->cur - 1] != ' ')
        e->mode = MODE_ABC;             /* one capital, then small */
    e->tap_key = -1;
}

static void digit(edit_t *e, int k, bool held, uint32_t now)
{
    const char *set = input_key(&input_langs[e->lang], k);
    int n;

    if (e->mode == MODE_NUM || held) {
        end_tap(e);
        put(e, (uint32_t)('0' + k));
        return;
    }
    if (e->tap_key == k && now - e->tap_at < TAP_MS && e->cur >= e->tap_len) {
        nth(set, 0, &n);
        for (int step = 1; step < n; step++) {  /* the next that fits the limit */
            int i = (e->tap_index + step) % n, k2;
            uint32_t c = cased(e, nth(set, i, &n));
            char buf[4];

            k2 = utf8_put(c, buf);
            if (e->len - e->tap_len + k2 > e->max)
                continue;
            remove_before(e, e->tap_len);
            insert(e, buf, k2);
            e->tap_index = i;
            e->tap_len = k2;
            e->typed = text_is_letter(c);
            break;
        }
    } else {
        int at;

        end_tap(e);
        at = e->cur;
        if (!put(e, cased(e, nth(set, 0, &n)))) {
            e->tap_key = -1;            /* full: no letter in, so none for a next tap to change */
            return;
        }
        e->tap_key = k;
        e->tap_index = 0;
        e->tap_len = e->cur - at;
    }
    e->tap_at = now;
}

/* ---- completion ------------------------------------------------------------ */

static void suggest(edit_t *e)
{
    const input_lang_t *l = &input_langs[e->lang];
    char prefix[WORD_MAX], found[SUGGEST][WORD_MAX];
    int a = word_start(e, e->cur), chars, n;
    bool upper = e->mode == MODE_ABC_UP, all = true;

    e->nsugg = 0;
    if (!completing(e) || (e->cur < e->len && text_is_letter(char_at(e, e->cur))))
        return;
    chars = fold(e, a, e->cur, prefix);
    if (chars < 2)
        return;
    for (const char *p = e->text + a; p < e->text + e->cur; ) {
        uint32_t c = utf8_get(&p);

        all = all && text_lower(l, c) != c;
    }
    upper = upper || all;               /* "MER": MERHABA */
    n = words_find(l->code, prefix, found, SUGGEST);
    for (int i = 0; i < n; i++) {
        /* the typed letters as they are, the rest in their case */
        const char *rest = found[i];
        char *o = e->sugg[e->nsugg];
        int len = e->cur - a, skip = chars;

        if (len >= WORD_MAX)
            break;
        memcpy(o, e->text + a, (size_t)len);
        while (skip-- && *rest)
            utf8_get(&rest);
        while (*rest) {
            char buf[4];
            uint32_t c = utf8_get(&rest);
            int k = utf8_put(upper ? text_upper(l, c) : c, buf);

            if (len + k >= WORD_MAX)
                break;
            memcpy(o + len, buf, (size_t)k);
            len += k;
        }
        o[len] = 0;
        if (!*rest)
            e->nsugg++;
    }
}

/* Dial: the first word offered in place of the one typed; again, the next */
static void complete(edit_t *e)
{
    int from, was;
    const char *w;

    if (e->cycle < 0) {
        if (!e->nsugg)
            return;
        e->cyc_at = word_start(e, e->cur);
        if (e->cur - e->cyc_at >= WORD_MAX)
            return;
        memcpy(e->cyc_word, e->text + e->cyc_at, (size_t)(e->cur - e->cyc_at));
        e->cyc_word[e->cur - e->cyc_at] = 0;
    }
    from = e->cycle;
    for (int step = 1; step <= e->nsugg + 1; step++) {
        int i = (from + step) % (e->nsugg + 1);     /* nsugg: the word as typed */

        w = i < e->nsugg ? e->sugg[i] : e->cyc_word;
        was = e->cur - e->cyc_at;
        if (e->len - was + (int)strlen(w) > e->max)
            continue;
        remove_before(e, was);
        insert(e, w, (int)strlen(w));
        e->cycle = i < e->nsugg ? i : -1;
        e->typed = true;
        if (e->cycle < 0)
            e->look = true;             /* back to the word typed: offered anew */
        return;
    }
}

/* ---- the symbol picker ------------------------------------------------------ */

#define PICK_COLS  8
#define PICK_W     15
#define PICK_H     15
#define PICK_ROWS  ((UI_CONTENT_H - 4) / PICK_H)

typedef struct {
    screen_t s;
    edit_t *e;
    int n, sel, top;
    uint16_t cp[];
} picker_t;

static void pick_draw(screen_t *s)
{
    picker_t *p = s->data;
    int x0 = (LCD_WIDTH - PICK_COLS * PICK_W) / 2, row = p->sel / PICK_COLS;
    int th = gfx_text_height(FONT_BODY, 1);

    if (row < p->top)
        p->top = row;
    if (row >= p->top + PICK_ROWS)
        p->top = row - PICK_ROWS + 1;
    for (int r = 0; r < PICK_ROWS; r++)
        for (int c = 0; c < PICK_COLS; c++) {
            int i = (p->top + r) * PICK_COLS + c, x = x0 + c * PICK_W, y = UI_TOP + 2 + r * PICK_H;
            char one[4];

            if (i >= p->n)
                return;
            one[utf8_put(p->cp[i], one)] = 0;
            if (i == p->sel)
                gfx_round(x, y, PICK_W - 1, PICK_H - 1, UI_RADIUS, C_ACCENT);
            gfx_text(x + (PICK_W - 1 - gfx_text_width(one, FONT_BODY, 1)) / 2,
                     y + (PICK_H - 1 - th) / 2, one, i == p->sel ? C_SEL_TEXT : C_TEXT,
                     FONT_BODY, 1, 0);
        }
}

static void pick_key(screen_t *s, const key_event_t *ev)
{
    picker_t *p = s->data;

    switch (ev->code) {
    case KEY_LEFT:
        p->sel = p->sel > 0 ? p->sel - 1 : p->n - 1;
        break;
    case KEY_RIGHT:
        p->sel = p->sel < p->n - 1 ? p->sel + 1 : 0;
        break;
    case KEY_UP:
        p->sel = p->sel >= PICK_COLS ? p->sel - PICK_COLS : p->sel;
        break;
    case KEY_DOWN:
        p->sel = p->sel + PICK_COLS < p->n ? p->sel + PICK_COLS : p->n - 1;
        break;
    case KEY_CENTER:
    case KEY_LSOFT: {
        edit_t *e = p->e;
        uint32_t c = p->cp[p->sel];

        ui_pop();                       /* frees p */
        put(e, c);
        e->look = true;
        break;
    }
    case KEY_RSOFT:
        ui_pop();
        break;
    }
}

static uint32_t pick_shown(screen_t *s)
{
    return 0;                           /* a fixed set: keys redraw it */
}

static void open_picker(edit_t *e)
{
    const char *sets[2] = { input_langs[e->lang].extra, input_symbols };
    int n = 0;
    picker_t *p;

    for (int k = 0; k < 2; k++)
        for (const char *q = sets[k]; *q; n++)
            utf8_get(&q);
    p = kzalloc(sizeof(*p) + (size_t)n * sizeof(uint16_t));
    if (!p)
        return;
    for (int k = 0; k < 2; k++)
        for (const char *q = sets[k]; *q; )
            p->cp[p->n++] = (uint16_t)utf8_get(&q);
    p->e = e;
    p->s.title = "Symbols";
    p->s.left = "Pick";
    p->s.center = "Pick";
    p->s.right = "Back";
    p->s.draw = pick_draw;
    p->s.key = pick_key;
    p->s.data = p;
    p->s.free_on_pop = true;
    ui_screen_shown(&p->s, pick_shown);     /* not drawn again every second */
    ui_push(&p->s);
}

/* ---- keys ------------------------------------------------------------------- */

static void next_lang(edit_t *e)
{
    int on[16], n = input_langs_on(on), i = 0;

    while (i < n && on[i] != e->lang)
        i++;
    e->lang = on[i < n ? (i + 1) % n : 0];
    db_set_str(INPUT_LANG_KEY, input_langs[e->lang].code);
}

static void tick(screen_t *s)
{
    edit_t *e = s->data;
    uint32_t now = kernel_ticks();
    bool changed = false;

    /* a frame only for what the tick changed (the letter's underline, the
     * mode after one capital, the words offered): it was ten a second */
    if (e->tap_key >= 0 && now - e->tap_at >= TAP_MS) {
        end_tap(e);
        changed = true;
    }
    if (e->look && now - e->key_at >= LOOK_MS) {
        e->look = false;
        if (completing(e))
            words_load();
        if (e->cycle < 0) {
            char was[SUGGEST][WORD_MAX];
            int nwas = e->nsugg;

            memcpy(was, e->sugg, sizeof(was));
            suggest(e);
            if (e->nsugg != nwas)
                changed = true;
            for (int i = 0; i < e->nsugg && !changed; i++)
                changed = strcmp(was[i], e->sugg[i]) != 0;
        }
    }
    if (changed)
        ui_redraw();
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
    while (pos < b && tail(e->text[pos]))
        pos++;
    e->cur = pos;
}

static void key(screen_t *s, const key_event_t *ev)
{
    edit_t *e = s->data;
    uint32_t now = kernel_ticks();
    bool held = ev->repeat == UI_KEY_LONG;

    e->key_at = now;
    e->look = true;
    if (ev->code != KEY_DIAL)
        e->cycle = -1;
    if (ev->code >= KEY_0 && ev->code <= KEY_9) {
        digit(e, ev->code - KEY_0, held, now);
        return;
    }
    end_tap(e);
    switch (ev->code) {
    case KEY_STAR:
        if (held)
            open_picker(e);
        else
            e->mode = (e->mode + 1) % MODE_COUNT;
        break;
    case KEY_HASH:
        if (held)
            next_lang(e);
        else if (e->flags & UI_TEXT_MULTILINE)
            put(e, '\n');
        break;
    case KEY_DIAL:
        complete(e);
        break;
    case KEY_LEFT:
        e->cur = before(e, e->cur);
        e->typed = false;
        break;
    case KEY_RIGHT:
        if (e->cur < e->len)
            e->cur += utf8_len(e->text + e->cur);
        e->typed = false;
        break;
    case KEY_UP:
        move_line(e, -1);
        e->typed = false;
        break;
    case KEY_DOWN:
        move_line(e, 1);
        e->typed = false;
        break;
    case KEY_LSOFT:
        if (e->cur > 0)
            remove_before(e, e->cur - before(e, e->cur));
        break;
    case KEY_CENTER:
        {
            /* the editor closes first: a message done() shows must stay up */
            void (*done)(const char *, void *) = e->done;
            void *ctx = e->ctx;
            char *copy = done ? kmalloc((size_t)e->len + 1) : NULL;

            learn(e);                   /* the last word counts too */
            if (done && !copy) {
                done(e->text, ctx);
                done = NULL;
            } else if (copy) {
                memcpy(copy, e->text, (size_t)e->len);
                copy[e->len] = 0;
            }
            ui_pop();                   /* frees e */
            if (done && copy)
                done(copy, ctx);
            kfree(copy);
        }
        return;
    case KEY_RSOFT:
        ui_pop();
        return;
    }
}

/* Editors open (one over another is possible): the learned words stay in RAM
 * only while one is. Counted at enter, which ui_push calls only once pushed
 * (a push that fails frees the screen without leave). */
static int editors;

static void enter(screen_t *s)
{
    editors++;
}

static void leave(screen_t *s)
{
    if (editors > 0)
        editors--;
    if (editors)
        words_flush();                  /* what it learned, in one write */
    else
        words_unload();                 /* the same, and the 8 KB pool goes */
    words_close();
}

void ui_text_edit_ex(const char *title, const char *initial, int max, unsigned flags,
                     void (*done)(const char *text, void *ctx), void *ctx)
{
    edit_t *e;
    int n = initial ? (int)strlen(initial) : 0, on[16], count;
    char code[8];

    if (max < 0)
        max = 0;
    e = kzalloc(sizeof(*e) + (size_t)max + 1);
    if (!e)
        return;
    if (n > max)                        /* cut on a character, not inside one */
        n = (int)utf8_cut(initial, (size_t)max);
    memcpy(e->text, initial ? initial : "", (size_t)n);
    e->text[n] = 0;
    e->len = e->cur = n;
    e->max = max;
    e->flags = flags;
    e->done = done;
    e->ctx = ctx;
    e->tap_key = -1;
    e->cycle = -1;
    e->mode = flags & UI_TEXT_NUMBER ? MODE_NUM : n ? MODE_ABC : MODE_ABC1;
    /* the language last used, if it is still on; else the first that is */
    count = input_langs_on(on);
    db_get_str(INPUT_LANG_KEY, code, sizeof(code), "");
    e->lang = on[0];
    for (int i = 0; i < count; i++)
        if (!strcmp(input_langs[on[i]].code, code))
            e->lang = on[i];
    e->look = true;                     /* the learned words: loaded at the first tick */
    e->s.title = title;
    e->s.left = "Del";
    e->s.center = "Save";
    e->s.right = "Back";
    e->s.draw = draw;
    e->s.key = key;
    e->s.tick = tick;
    e->s.enter = enter;
    e->s.leave = leave;
    e->s.tick_ms = 100;
    e->s.data = e;
    e->s.free_on_pop = true;
    ui_screen_mode(&e->s, UI_MODE_HOLD_KEYS);
    ui_push(&e->s);
}

void ui_text_edit(const char *title, const char *initial, int max, bool multiline,
                  void (*done)(const char *text, void *ctx), void *ctx)
{
    ui_text_edit_ex(title, initial, max, multiline ? UI_TEXT_MULTILINE : 0, done, ctx);
}
