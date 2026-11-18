/* B310E OS — drawing into the frame: anti-aliased text and rounded shapes.
 *
 * Everything blends into the RGB565 back buffer (lcd_fb) with 5-bit
 * alpha: a colour and the pixel under it are spread into 0x07e0f81f
 * form, mixed in one multiply and packed again. Glyphs and icons are
 * 4-bit alpha maps (ui/font_montserrat.c, ui/icons.c). Rounded corners
 * use a coverage table per radius, computed once (4 x 4 samples a pixel).
 * No scene graph and no styles: screens draw what they show every frame,
 * which on a 128 x 160 panel costs a millisecond or two. Every call marks
 * the rows it may change for lcd_update_drawn() (drivers/lcd.c).
 * os/tests/test-gfx.py checks every change pixel for pixel against the
 * committed file (on the host).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/drivers.h"
#include "os/lib.h"
#include "os/status.h"
#include "os/ui.h"
#include "ui/font.h"

#define MAX_RADIUS 12
#define M565       0x07e0f81fu          /* the spread form's fields */

/* ---- rows drawn ------------------------------------------------------------ */

/* The rows written since the last lcd_update, y0 .. y1-1 (drivers/lcd.c
 * owns them and empties them after an update; lcd_update_drawn() compares
 * only them). Every function here marks the rows it may change, clipped to
 * the screen, once a call. (Declared in os/drivers.h.) */

static inline void touch(int y0, int y1)    /* on the screen, y0 < y1 */
{
    if (y0 < lcd_dirty_y0)
        lcd_dirty_y0 = y0;
    if (y1 > lcd_dirty_y1)
        lcd_dirty_y1 = y1;
}

static inline void touch_clip(int y, int h)
{
    int y1 = y + h;

    if (y < 0)
        y = 0;
    if (y1 > LCD_HEIGHT)
        y1 = LCD_HEIGHT;
    if (y < y1)
        touch(y, y1);
}

/* ---- pixels ------------------------------------------------------------- */

static inline uint32_t spread(uint16_t c)
{
    return (c | (uint32_t)c << 16) & M565;
}

/* a: 0..32; each field times 32 still fits its gap in the spread form */
static inline uint16_t mix(uint16_t bg, uint32_t fg_spread, unsigned a)
{
    uint32_t b = (fg_spread * a + spread(bg) * (32 - a)) >> 5 & M565;

    return (uint16_t)(b | b >> 16);
}

static inline uint32_t ror16(uint32_t v)
{
    return v >> 16 | v << 16;
}

/* Two pixels of a frame word (p0 low, p1 high) mixed as mix() does each,
 * fga = fg_spread * a, ia = 32 - a, a 0..32. The word masked is p0's red
 * and blue with p1's green, the word turned by 16 masked the other three:
 * both in the spread form's places, so each field is mixed with the same
 * multiply and shift as in mix(), and with a <= 32 no field reaches the
 * next. */
static inline uint32_t mix2(uint32_t w, uint32_t fga, unsigned ia)
{
    uint32_t lo = w & M565, hi = ror16(w) & M565;

    lo = (fga + lo * ia) >> 5 & M565;
    hi = (fga + hi * ia) >> 5 & M565;
    return lo | ror16(hi);
}

/* Inlined (with -Os it was a call per pixel: blend was 26% of a list-scroll
 * redraw, prof 2026-11-17) */
static inline __attribute__((always_inline)) void blend(int x, int y, uint32_t fg,
                                                        unsigned a)
{
    uint16_t *p;

    if ((unsigned)x >= LCD_WIDTH || (unsigned)y >= LCD_HEIGHT || !a)
        return;
    p = &lcd_fb[y * LCD_WIDTH + x];
    *p = a >= 32 ? (uint16_t)(fg | fg >> 16) : mix(*p, fg, a);
}

static inline unsigned a4to32(unsigned a4)
{
    return a4 * 2 + (a4 >= 8);
}

/* ---- rectangles --------------------------------------------------------- */

static bool clip(int *x, int *y, int *w, int *h)
{
    if (*x < 0) { *w += *x; *x = 0; }
    if (*y < 0) { *h += *y; *y = 0; }
    if (*x + *w > LCD_WIDTH) *w = LCD_WIDTH - *x;
    if (*y + *h > LCD_HEIGHT) *h = LCD_HEIGHT - *y;
    return *w > 0 && *h > 0;
}

/* two pixels a store (the frame is RGB565 halfwords); four words, which
 * GCC stores with one stm */
typedef uint32_t __attribute__((may_alias)) px2_t;
typedef struct { uint32_t a, b, c, d; } __attribute__((may_alias)) px8_t;

/* n >= 1 pixels of cc (the colour in both halves) from p: a halfword to
 * the word boundary, 16 pixels a step in two stm, words, a halfword left
 * over. gfx_fill was 23% of a list-scroll redraw storing a halfword at a
 * time, fill_span 15% with a str a word (7 instructions per 8 pixels). */
static void fill_span(uint16_t *p, int n, uint32_t cc)
{
    px2_t *q;

    if ((uintptr_t)p & 2) {
        *p++ = (uint16_t)cc;
        n--;
    }
    q = (px2_t *)p;
    if (n >= 8) {
        px8_t v = { cc, cc, cc, cc }, *q8 = (px8_t *)q;

#if defined(__arm__) && !defined(__thumb__)
        /* GCC stores a struct of four equal words with four str: the stm
         * by hand, 16 pixels in two (r4-r7, in the order stm wants) */
        if (n >= 16) {
            register uint32_t c0 __asm__("r4") = cc, c1 __asm__("r5") = cc;
            register uint32_t c2 __asm__("r6") = cc, c3 __asm__("r7") = cc;
            int k = n >> 4;

            n &= 15;
            __asm__ volatile("1:\n\t"
                             "stmia %0!, {%2, %3, %4, %5}\n\t"
                             "stmia %0!, {%2, %3, %4, %5}\n\t"
                             "subs  %1, %1, #1\n\t"
                             "bne   1b"
                             : "+r"(q8), "+r"(k)
                             : "r"(c0), "r"(c1), "r"(c2), "r"(c3)
                             : "memory", "cc");
        }
#endif
        for (; n >= 16; n -= 16, q8 += 2) {
            q8[0] = v;
            q8[1] = v;
        }
        if (n >= 8) {
            *q8++ = v;
            n -= 8;
        }
        q = (px2_t *)q8;
    }
    for (; n >= 2; n -= 2)
        *q++ = cc;
    if (n)
        *(uint16_t *)q = (uint16_t)cc;
}

/* n pixels from p mixed with fg at a constant 1..31, as blend() would each:
 * two a word. The dialog's shade and shadow did a mix() per pixel. */
static void mix_span(uint16_t *p, int n, uint32_t fg, unsigned a)
{
    uint32_t fga = fg * a;
    unsigned ia = 32 - a;
    px2_t *q;

    if (n <= 0)
        return;
    if ((uintptr_t)p & 2) {
        *p = mix(*p, fg, a);
        p++;
        n--;
    }
    q = (px2_t *)p;
    for (; n >= 4; n -= 4, q += 2) {
        q[0] = mix2(q[0], fga, ia);
        q[1] = mix2(q[1], fga, ia);
    }
    if (n >= 2) {
        *q = mix2(*q, fga, ia);
        q++;
        n -= 2;
    }
    if (n) {
        uint16_t *e = (uint16_t *)q;

        *e = mix(*e, fg, a);
    }
}

/* n pixels as blend() at a constant a: none at 0, the colour at 32 or
 * more, mixed between */
static void blend_span(uint16_t *p, int n, uint32_t fg, unsigned a)
{
    if (n <= 0 || !a)
        return;
    if (a >= 32) {
        uint32_t c = (uint16_t)(fg | fg >> 16);

        fill_span(p, n, c | c << 16);
    } else {
        mix_span(p, n, fg, a);
    }
}

void gfx_fill(int x, int y, int w, int h, uint16_t c)
{
    uint32_t cc = c | (uint32_t)c << 16;
    uint16_t *p;

    if (!clip(&x, &y, &w, &h))
        return;
    touch(y, y + h);
    p = lcd_fb + y * LCD_WIDTH + x;
    if (w == LCD_WIDTH) {               /* whole rows: one span */
        fill_span(p, w * h, cc);
        return;
    }
    for (int j = 0; j < h; j++, p += LCD_WIDTH)
        fill_span(p, w, cc);
}

/* w x h RGB565 pixels (rows of w) at x, y, clipped to the screen */
void gfx_blit(int x, int y, int w, int h, const uint16_t *px)
{
    int x0 = x, y0 = y, cw = w, ch = h;

    if (!clip(&x, &y, &cw, &ch))
        return;
    touch(y, y + ch);
    for (int j = 0; j < ch; j++)
        memcpy(lcd_fb + (y + j) * LCD_WIDTH + x, px + (y - y0 + j) * w + (x - x0),
               (size_t)cw * 2);
}

/* Every pixel mixed with c at alpha/255 (32 steps). Two pixels a word
 * (mix_span): 154k instructions for a dialog's shade per pixel (prof
 * 2026-11-17). At a 0 mix() leaves a pixel as it is, at 32 it is c; an
 * alpha outside 0..255 (a step above 32) mixes per pixel as before. */
void gfx_shade(int x, int y, int w, int h, uint16_t c, int alpha)
{
    uint32_t fg = spread(c);
    unsigned a = (unsigned)(alpha * 32 + 127) / 255;
    uint16_t *p;

    if (!clip(&x, &y, &w, &h) || !a)
        return;
    touch(y, y + h);
    p = lcd_fb + y * LCD_WIDTH + x;
    if (a > 32) {
        for (int j = 0; j < h; j++, p += LCD_WIDTH)
            for (int i = 0; i < w; i++)
                p[i] = mix(p[i], fg, a);
        return;
    }
    if (w == LCD_WIDTH) {               /* whole rows: one span */
        w *= h;
        h = 1;
    }
    for (int j = 0; j < h; j++, p += LCD_WIDTH)
        if (a == 32)
            fill_span(p, w, c | (uint32_t)c << 16);
        else
            mix_span(p, w, fg, a);
}

void gfx_hline(int x, int y, int w, uint16_t c)
{
    gfx_fill(x, y, w, 1, c);
}

void gfx_rect(int x, int y, int w, int h, uint16_t c)
{
    gfx_fill(x, y, w, 1, c);
    gfx_fill(x, y + h - 1, w, 1, c);
    gfx_fill(x, y, 1, h, c);
    gfx_fill(x + w - 1, y, 1, h, c);
}

/* ---- rounded rectangles ------------------------------------------------- */

/* coverage[r][j][i]: 0..32 for the pixel i, j of a corner of radius r,
 * counted from the outer corner */
static uint8_t coverage[MAX_RADIUS + 1][MAX_RADIUS][MAX_RADIUS];
static uint16_t have;                   /* radii computed */

static const uint8_t *corner(int r)
{
    if (!(have & 1u << r)) {
        for (int j = 0; j < r; j++)
            for (int i = 0; i < r; i++) {
                int in = 0;

                for (int sy = 0; sy < 4; sy++)
                    for (int sx = 0; sx < 4; sx++) {
                        /* sample centre from the circle centre at (r, r) */
                        int dx = (r * 8) - (i * 8 + sx * 2 + 1);
                        int dy = (r * 8) - (j * 8 + sy * 2 + 1);

                        in += dx * dx + dy * dy <= r * r * 64;
                    }
                coverage[r][j][i] = (uint8_t)(in * 2);
            }
        have |= (uint16_t)(1u << r);
    }
    return &coverage[r][0][0];
}

/* blend() on the screen, a > 0 */
static inline __attribute__((always_inline)) void blend_px(uint16_t *p, uint32_t fg,
                                                           unsigned a)
{
    *p = a >= 32 ? (uint16_t)(fg | fg >> 16) : mix(*p, fg, a);
}

/* Every pixel of the rectangle blended at alpha (0..32, more is opaque),
 * the r x r corners at their coverage times alpha. Clipped once; a row's
 * straight part (all of the rows between the corners) is one blend_span,
 * two pixels a word: the dialog's shadow was 225k instructions blending
 * pixel by pixel through blend()'s bounds test (prof 2026-11-17). */
static void round_rect(int x, int y, int w, int h, int r, uint16_t c, unsigned alpha)
{
    uint32_t fg = spread(c);
    const uint8_t *cov;
    int i0, i1, j0, j1;

    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    if (r > MAX_RADIUS) r = MAX_RADIUS;
    if (r < 0) r = 0;
    cov = corner(r);
    i0 = x < 0 ? -x : 0;                /* columns i0..i1-1 and rows on the screen */
    i1 = x + w > LCD_WIDTH ? LCD_WIDTH - x : w;
    j0 = y < 0 ? -y : 0;
    j1 = y + h > LCD_HEIGHT ? LCD_HEIGHT - y : h;
    if (!alpha || i0 >= i1 || j0 >= j1)
        return;
    touch(y + j0, y + j1);
    for (int j = j0; j < j1; j++) {
        uint16_t *row = lcd_fb + (y + j) * LCD_WIDTH;     /* pixel i at row + (x + i) */
        int cj = j < r ? j : j >= h - r ? h - 1 - j : -1, m0, m1;
        const uint8_t *cr;

        if (cj < 0) {
            blend_span(row + (x + i0), i1 - i0, fg, alpha);
            continue;
        }
        cr = cov + cj * MAX_RADIUS;
        /* 2r <= w: the left corner 0..r-1, the middle, the right w-r..w-1 */
        for (int i = i0; i < r && i < i1; i++) {
            unsigned a = (unsigned)cr[i] * alpha >> 5;

            if (a)
                blend_px(row + (x + i), fg, a);
        }
        m0 = r > i0 ? r : i0;
        m1 = w - r < i1 ? w - r : i1;
        blend_span(row + (x + m0), m1 - m0, fg, alpha);
        for (int i = w - r > i0 ? w - r : i0; i < i1; i++) {
            unsigned a = (unsigned)cr[w - 1 - i] * alpha >> 5;

            if (a)
                blend_px(row + (x + i), fg, a);
        }
    }
}

void gfx_round(int x, int y, int w, int h, int r, uint16_t c)
{
    const uint8_t *cov;
    uint32_t fg = spread(c);

    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    if (r > MAX_RADIUS) r = MAX_RADIUS;
    if (r <= 0) {
        gfx_fill(x, y, w, h, c);
        return;
    }
    /* The straight middle rows, and the middle of the top and bottom r rows
     * (full coverage: blend at 32 stores c), filled; only the four r x r
     * corners blended. Every pixel was blended before (gfx_round 13% of a
     * list-scroll redraw, half of blend's 26%). r <= w/2 and r <= h/2, so
     * the parts do not overlap. */
    gfx_fill(x, y + r, w, h - 2 * r, c);
    gfx_fill(x + r, y, w - 2 * r, r, c);
    gfx_fill(x + r, y + h - r, w - 2 * r, r, c);
    touch_clip(y, r);                   /* the corners' rows */
    touch_clip(y + h - r, r);
    cov = corner(r);
    for (int j = 0; j < r; j++)
        for (int i = 0; i < r; i++) {
            unsigned a = cov[j * MAX_RADIUS + i];

            blend(x + i, y + j, fg, a);
            blend(x + w - 1 - i, y + j, fg, a);
            blend(x + i, y + h - 1 - j, fg, a);
            blend(x + w - 1 - i, y + h - 1 - j, fg, a);
        }
}

void gfx_round_alpha(int x, int y, int w, int h, int r, uint16_t c, int alpha)
{
    round_rect(x, y, w, h, r, c, (unsigned)(alpha * 32 + 127) / 255);
}

/* ---- alpha maps: icons and glyphs ---------------------------------------- */

/* Text and icons are drawn over a plain background nearly always (the
 * content, a card, the selection bar), so a map's 15 mixes of its colour
 * with that background are kept: mixtab[] holds them for the last 8 pairs
 * of colour and background, px[a4] = mix(bg, fg, a4to32(a4)), hashed on
 * the pair, and px[0] = fg (spread) for map_mix. A key of 0 (black on
 * black) matches the zeroed table as it is. The UI thread draws alone (the
 * key is written after the mixes). */
typedef struct {
    uint32_t key;                       /* c | bg << 16 */
    uint32_t px[16];
} mixtab_t;

static mixtab_t mixtab[8];
static uint32_t no_tab[16];             /* px[0] only: a map with no background */

static const uint32_t *mix_table(uint16_t c, uint32_t fg, uint16_t bg)
{
    uint32_t key = c | (uint32_t)bg << 16;
    mixtab_t *t = &mixtab[key * 0x9e3779b1u >> 29];

    if (t->key != key) {
        t->px[0] = fg;
        for (unsigned a4 = 1; a4 < 16; a4++)
            t->px[a4] = mix(bg, fg, a4to32(a4));
        t->key = key;
    }
    return t->px;
}

/* mix(v, fg, a4to32(a4)) before the cast, fg = tab[0] (the spread colour):
 * out of line and with one pointer for both, so the loop keeps its
 * registers */
static uint32_t __attribute__((noinline)) map_mix(unsigned v, const uint32_t *tab, unsigned a4)
{
    unsigned a = a4to32(a4);
    uint32_t b = (tab[0] * a + spread((uint16_t)v) * (32 - a)) >> 5 & M565;

    return b | b >> 16;
}

/* one pixel of a map: a4 0 leaves it; on the table's background the
 * table's mix, else mixed (a4to32 tops out at 31, so a map never takes
 * blend's opaque store) */
static inline __attribute__((always_inline)) void map_px(uint16_t *p, unsigned a4,
                                                         const uint32_t *tab, uint32_t tbg)
{
    if (a4) {
        unsigned v = *p;

        *p = (uint16_t)(__builtin_expect(v == tbg, 1) ? tab[a4] : map_mix(v, tab, a4));
    }
}

/* Clipped once, not per pixel; a byte (two pixels) at a time, a zero byte
 * skipped. When the clipped box's first and last pixels are the same
 * colour that is taken as the background: its pixels get the table's mix,
 * a load instead of the two multiplies (alpha_map was 41% of list-scroll,
 * ~790 instructions a glyph or icon, prof 2026-11-17). Any other pixel
 * (an earlier glyph's ink, a gradient) is mixed as before. */
static void alpha_map(int x, int y, int w, int h, const uint8_t *bits, uint16_t c)
{
    uint32_t fg = spread(c), tbg = 0x10000;     /* a background no pixel has */
    const uint32_t *tab = no_tab;
    int stride = (w + 1) / 2;
    int j0 = y < 0 ? -y : 0, j1 = y + h > LCD_HEIGHT ? LCD_HEIGHT - y : h;
    int i0 = x < 0 ? -x : 0, i1 = x + w > LCD_WIDTH ? LCD_WIDTH - x : w;
    int odd, pairs, tail;
    const uint8_t *row;
    uint16_t *line, *end;

    if (i0 >= i1 || j0 >= j1)
        return;
    touch(y + j0, y + j1);
    line = lcd_fb + (y + j0) * LCD_WIDTH + (x + i0);
    end = line + (j1 - j0) * LCD_WIDTH;
    {
        uint16_t first = line[0], last = end[i1 - 1 - i0 - LCD_WIDTH];

        if (first == last) {
            tab = mix_table(c, fg, first);
            tbg = first;
        } else {
            no_tab[0] = fg;
        }
    }
    if (!i0 && i1 == w) {
        /* Not clipped at the sides, as nearly every glyph and icon: whole
         * bytes, the rows' bytes one after the other. An odd width's last
         * byte has a zero low nibble (os/tools/mkfont.py pads rows so),
         * which leaves the pixel after the box alone like any 0. About 30
         * instructions a row less than the clipped loop's. */
        const uint8_t *s = bits + j0 * stride;

        for (; line != end; line += LCD_WIDTH) {
            const uint8_t *e = s + stride;
            uint16_t *p = line;

            do {
                unsigned b = *s++;

                if (b) {
                    map_px(p, b >> 4, tab, tbg);
                    map_px(p + 1, b & 15, tab, tbg);
                }
                p += 2;
            } while (s != e);
        }
        return;
    }
    /* a row: a low nibble first when clipped on an odd column, bytes, a
     * high nibble left over */
    odd = i0 & 1;
    pairs = (i1 - i0 - odd) >> 1;
    tail = (i1 - i0 - odd) & 1;
    row = bits + j0 * stride + (i0 >> 1);
    for (; line != end; row += stride, line += LCD_WIDTH) {
        const uint8_t *s = row, *e;
        uint16_t *p = line;

        if (odd)
            map_px(p++, *s++ & 15, tab, tbg);
        for (e = s + pairs; s != e; p += 2) {
            unsigned b = *s++;

            if (b) {
                map_px(p, b >> 4, tab, tbg);
                map_px(p + 1, b & 15, tab, tbg);
            }
        }
        if (tail)
            map_px(p, *s >> 4, tab, tbg);
    }
}

void gfx_icon(int x, int y, const icon_t *icon, uint16_t c)
{
    if (icon)
        alpha_map(x, y, ICON_SIZE, ICON_SIZE, icon->a, c);
}

/* ---- text ------------------------------------------------------------------ */

static const font_t *pick(int font, int scale)
{
    switch (font) {
    case FONT_SMALL: return &font_small;
    case FONT_BODY:  return &font_body;
    case FONT_HUGE:  return &font_huge;
    default:         return scale >= 2 ? &font_huge : &font_title;
    }
}

/* The rest by binary search: some 9 steps in 480 glyphs, never a scan. */
static const font_glyph_t *__attribute__((noinline)) glyph_search(const font_t *f, uint32_t ch)
{
    int lo = 0, hi = f->count - 1;

    while (lo <= hi) {
        int mid = (int)((unsigned)(lo + hi) >> 1);

        if (f->glyphs[mid].code == ch)
            return &f->glyphs[mid];
        if (f->glyphs[mid].code < ch)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return NULL;
}

/* ASCII by index (the text fonts hold it in one run from the space), in
 * the caller's loop: the call was 18 instructions a character. */
static inline __attribute__((always_inline)) const font_glyph_t *glyph(const font_t *f,
                                                                      uint32_t ch)
{
    if (ch - 0x20 < 0x5f && f->count >= 0x5f && f->glyphs[ch - 0x20].code == ch)
        return &f->glyphs[ch - 0x20];
    return glyph_search(f, ch);
}

/* glyph() out of line, for the fallbacks */
static const font_glyph_t *__attribute__((noinline)) glyph_any(const font_t *f, uint32_t ch)
{
    return glyph(f, ch);
}

/* The huge font has digits only: other characters fall back to the title
 * font, so "100.0 MHz" still draws. A character no font has is a '?'. */
static inline const font_glyph_t *glyph_or(const font_t **f, uint32_t ch)
{
    const font_glyph_t *g = glyph(*f, ch);

    if (!g && *f == &font_huge) {
        *f = &font_title;
        g = glyph_any(*f, ch);
    }
    if (!g)
        g = glyph_any(*f, '?');
    return g;
}

/* As glyph_or, counting what draws as '?' for the tests (os/status.h);
 * the first lookup not done twice */
static const font_glyph_t *glyph_drawn(const font_t **f, uint32_t ch)
{
    const font_glyph_t *g = glyph(*f, ch);

    if (g)
        return g;
    if (*f == &font_huge) {
        *f = &font_title;
        g = glyph_any(*f, ch);
    }
    if (!g)
        g = glyph_any(*f, '?');
    if (g && g->code == '?')
        os_status.ui_missing_glyphs++;
    return g;
}

static int cap_height(const font_t *f)
{
    const font_glyph_t *g = glyph(f, f == &font_huge ? '0' : 'H');

    return g ? -g->y : f->ascent;
}

int gfx_text_height(int font, int scale)
{
    return cap_height(pick(font, scale));
}

/* Text is UTF-8, one glyph a character (lib/utf8.c); a broken sequence is a
 * '?' a byte. ASCII here, as utf8_get reads it (one byte, itself): the call
 * and utf8_len were 28 instructions a character. */
static inline __attribute__((always_inline)) uint32_t next_char(const char **s)
{
    const char *p = *s;
    unsigned b = (unsigned char)*p;

    if (b < 0x80) {
        *s = p + 1;
        return b;
    }
    {
        uint32_t ch = utf8_get(&p);     /* only p's address taken: s stays in a register */

        *s = p;
        return ch;
    }
}

static int char_width(const font_t *f, uint32_t ch)
{
    const font_glyph_t *g = glyph_or(&f, ch);

    return g ? g->advance : 0;
}

int gfx_text_width(const char *s, int font, int scale)
{
    const font_t *f = pick(font, scale);
    int w = 0;

    while (*s)
        w += char_width(f, next_char(&s));
    return w;
}

static int draw_char(int x, int base, const font_t *f, uint32_t ch, uint16_t c)
{
    const font_glyph_t *g = glyph_drawn(&f, ch);

    if (!g)
        return 0;
    if (g->w)
        alpha_map(x + g->x, base + g->y, g->w, g->h, f->bits + g->offset, c);
    return g->advance;
}

/* Whether s is wider than max_w: the sum stops once it is (advances are
 * never negative). gfx_text measured every string in full first, even
 * with no max_w to check. */
static bool wider_than(const char *s, const font_t *f, int max_w)
{
    int w = 0;

    while (*s) {
        w += char_width(f, next_char(&s));
        if (w > max_w)
            return true;
    }
    return false;
}

static int draw_text(int x, int y, const char *s, uint16_t c, const font_t *f,
                     int max_w, bool cut)
{
    int base = y + cap_height(f), x0 = x;

    if (cut) {                          /* as much as fits, then ".." */
        int dots = 2 * char_width(f, '.');

        while (*s) {
            const char *at = s;
            uint32_t ch = next_char(&at);

            if (x + char_width(f, ch) - x0 > max_w - dots)
                break;
            x += draw_char(x, base, f, ch, c);
            s = at;
        }
        x += draw_char(x, base, f, '.', c);
        x += draw_char(x, base, f, '.', c);
        return x - x0;
    }
    while (*s)
        x += draw_char(x, base, f, next_char(&s), c);
    return x - x0;
}

int gfx_text(int x, int y, const char *s, uint16_t c, int font, int scale,
             int max_w)
{
    const font_t *f = pick(font, scale);

    return draw_text(x, y, s, c, f, max_w, max_w > 0 && wider_than(s, f, max_w));
}

/* measured once: the width it centres on also says whether it is cut */
int gfx_text_centered(int y, const char *s, uint16_t c, int font, int scale)
{
    int w = gfx_text_width(s, font, scale);
    bool cut = w > LCD_WIDTH - 4;

    if (cut)
        w = LCD_WIDTH - 4;
    return draw_text((LCD_WIDTH - w) / 2, y, s, c, pick(font, scale), LCD_WIDTH - 4, cut);
}

/* ---- small widgets ------------------------------------------------------- */

void gfx_bar(int x, int y, int w, int h, int value, int max, uint16_t c)
{
    int fill;

    if (max <= 0) max = 1;
    if (value < 0) value = 0;
    if (value > max) value = max;
    /* 0 <= value <= max: in 32 bits when the product fits (as it always
     * does on this screen), the same quotient without the 64-bit divide */
    if (w >= -32768 && w <= 32767 && max <= 65535)
        fill = w * value / max;
    else
        fill = (int)((int64_t)w * value / max);
    gfx_round(x, y, w, h, 1, C_CARD_HI);
    if (fill > 0)
        gfx_round(x, y, fill < 2 ? 2 : fill, h, 1, c);
}

/* A history graph: n samples in a ring (newest at v[(head - 1) mod n]),
 * oldest at the left; scaled to max (0: the largest sample, at least 1).
 * A card with two faint grid lines, the area under the line tinted, the
 * line itself in c. Returns the max used. */
int gfx_graph(int x, int y, int w, int h, const uint16_t *v, int n, int head,
              int max, uint16_t c)
{
    int px = -1, py = 0;

    if (max <= 0) {
        max = 1;
        for (int i = 0; i < n; i++)
            if (v[i] > max)
                max = v[i];
    }
    gfx_round(x, y, w, h, 1, C_CARD);
    gfx_hline(x + 2, y + h / 4, w - 4, C_BG);
    gfx_hline(x + 2, y + h / 2, w - 4, C_BG);
    gfx_hline(x + 2, y + h * 3 / 4, w - 4, C_BG);
    for (int i = 0; i < n; i++) {
        int s = v[(head + i) % n], cx = x + 1 + (int)((int64_t)(w - 3) * i / (n > 1 ? n - 1 : 1));
        int cy = y + h - 2 - (int)((int64_t)(h - 4) * (s > max ? max : s) / max);

        if (px >= 0) {
            int y0 = py;                                /* py follows the columns */

            for (int xx = px + 1; xx <= cx; xx++) {     /* a column per pixel */
                int yy = y0 + (cy - y0) * (xx - px) / (cx - px);

                gfx_shade(xx, yy, 1, y + h - 1 - yy, c, 60);
                gfx_fill(xx, yy, 1, 1, c);
                if (yy != py)                           /* steep: close the gap */
                    gfx_fill(xx, yy < py ? yy : py, 1, (yy < py ? py - yy : yy - py), c);
                py = yy;
            }
        }
        px = cx;
        py = cy;
    }
    return max;
}

void gfx_switch(int x, int y, bool on)
{
    gfx_round(x, y, 20, 10, 2, on ? C_ACCENT : C_CARD_HI);
    gfx_round(on ? x + 11 : x + 1, y + 1, 8, 8, 1, on ? C_SEL_TEXT : C_DIM);
}
