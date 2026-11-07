/* B310E OS — drawing into the frame: anti-aliased text and rounded shapes.
 *
 * Everything blends into the RGB565 back buffer (lcd_fb) with 5-bit
 * alpha: a colour and the pixel under it are spread into 0x07e0f81f
 * form, mixed in one multiply and packed again. Glyphs and icons are
 * 4-bit alpha maps (ui/font_montserrat.c, ui/icons.c). Rounded corners
 * use a coverage table per radius, computed once (4 x 4 samples a pixel).
 * No scene graph and no styles: screens draw what they show every frame,
 * which on a 128 x 160 panel costs a millisecond or two.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/drivers.h"
#include "os/lib.h"
#include "os/ui.h"
#include "ui/font.h"

#define MAX_RADIUS 12

/* ---- pixels ------------------------------------------------------------- */

static inline uint32_t spread(uint16_t c)
{
    return (c | (uint32_t)c << 16) & 0x07e0f81fu;
}

/* a: 0..32; each field times 32 still fits its gap in the spread form */
static inline uint16_t mix(uint16_t bg, uint32_t fg_spread, unsigned a)
{
    uint32_t b = (fg_spread * a + spread(bg) * (32 - a)) >> 5 & 0x07e0f81fu;

    return (uint16_t)(b | b >> 16);
}

static inline void blend(int x, int y, uint32_t fg, unsigned a)
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

void gfx_fill(int x, int y, int w, int h, uint16_t c)
{
    if (!clip(&x, &y, &w, &h))
        return;
    for (int j = 0; j < h; j++) {
        uint16_t *p = lcd_fb + (y + j) * LCD_WIDTH + x;

        for (int i = 0; i < w; i++)
            p[i] = c;
    }
}

void gfx_shade(int x, int y, int w, int h, uint16_t c, int alpha)
{
    uint32_t fg = spread(c);
    unsigned a = (unsigned)(alpha * 32 + 127) / 255;

    if (!clip(&x, &y, &w, &h))
        return;
    for (int j = 0; j < h; j++) {
        uint16_t *p = lcd_fb + (y + j) * LCD_WIDTH + x;

        for (int i = 0; i < w; i++)
            p[i] = mix(p[i], fg, a);
    }
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

static void round_rect(int x, int y, int w, int h, int r, uint16_t c, unsigned alpha)
{
    uint32_t fg = spread(c);
    const uint8_t *cov;

    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    if (r > MAX_RADIUS) r = MAX_RADIUS;
    if (r < 0) r = 0;
    cov = corner(r);
    for (int j = 0; j < h; j++) {
        int yy = y + j, cj = j < r ? j : j >= h - r ? h - 1 - j : -1;

        if ((unsigned)yy >= LCD_HEIGHT)
            continue;
        for (int i = 0; i < w; i++) {
            int ci = i < r ? i : i >= w - r ? w - 1 - i : -1;
            unsigned a = alpha;

            if (cj >= 0 && ci >= 0)
                a = (unsigned)cov[cj * MAX_RADIUS + ci] * alpha >> 5;
            if (a)
                blend(x + i, yy, fg, a);
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
    /* the straight middle in one go; the top and bottom r rows blended */
    gfx_fill(x, y + r, w, h - 2 * r, c);
    cov = corner(r);
    for (int j = 0; j < r; j++)
        for (int i = 0; i < w; i++) {
            int ci = i < r ? i : i >= w - r ? w - 1 - i : -1;
            unsigned a = ci >= 0 ? cov[j * MAX_RADIUS + ci] : 32;

            blend(x + i, y + j, fg, a);
            blend(x + i, y + h - 1 - j, fg, a);
        }
}

void gfx_round_alpha(int x, int y, int w, int h, int r, uint16_t c, int alpha)
{
    round_rect(x, y, w, h, r, c, (unsigned)(alpha * 32 + 127) / 255);
}

/* ---- alpha maps: icons and glyphs ---------------------------------------- */

static void alpha_map(int x, int y, int w, int h, const uint8_t *bits, uint16_t c)
{
    uint32_t fg = spread(c);
    int stride = (w + 1) / 2;

    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            uint8_t b = bits[j * stride + i / 2];

            blend(x + i, y + j, fg, a4to32(i & 1 ? b & 15 : b >> 4));
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

static const font_glyph_t *glyph(const font_t *f, unsigned char ch)
{
    int lo = 0, hi = f->count - 1;

    while (lo <= hi) {
        int mid = (lo + hi) / 2;

        if (f->glyphs[mid].code == ch)
            return &f->glyphs[mid];
        if (f->glyphs[mid].code < ch)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return NULL;
}

/* The huge font has digits only: other characters fall back to the title
 * font, so "100.0 MHz" still draws. */
static const font_glyph_t *glyph_or(const font_t **f, unsigned char ch)
{
    const font_glyph_t *g = glyph(*f, ch);

    if (!g && *f == &font_huge) {
        *f = &font_title;
        g = glyph(*f, ch);
    }
    if (!g)
        g = glyph(*f, '?');
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

static int char_width(const font_t *f, unsigned char ch)
{
    const font_glyph_t *g = glyph_or(&f, ch);

    return g ? g->advance : 0;
}

int gfx_text_width(const char *s, int font, int scale)
{
    const font_t *f = pick(font, scale);
    int w = 0;

    while (*s)
        w += char_width(f, (unsigned char)*s++);
    return w;
}

static int draw_char(int x, int base, const font_t *f, unsigned char ch, uint16_t c)
{
    const font_glyph_t *g = glyph_or(&f, ch);

    if (!g)
        return 0;
    if (g->w)
        alpha_map(x + g->x, base + g->y, g->w, g->h, f->bits + g->offset, c);
    return g->advance;
}

int gfx_text(int x, int y, const char *s, uint16_t c, int font, int scale,
             int max_w)
{
    const font_t *f = pick(font, scale);
    int base = y + cap_height(f), w = gfx_text_width(s, font, scale), x0 = x;

    if (max_w > 0 && w > max_w) {       /* as much as fits, then ".." */
        int dots = 2 * char_width(f, '.');

        while (*s) {
            int cw = char_width(f, (unsigned char)*s);

            if (x + cw - x0 > max_w - dots)
                break;
            x += draw_char(x, base, f, (unsigned char)*s++, c);
        }
        x += draw_char(x, base, f, '.', c);
        x += draw_char(x, base, f, '.', c);
        return x - x0;
    }
    while (*s)
        x += draw_char(x, base, f, (unsigned char)*s++, c);
    return x - x0;
}

int gfx_text_centered(int y, const char *s, uint16_t c, int font, int scale)
{
    int w = gfx_text_width(s, font, scale);

    if (w > LCD_WIDTH - 4)
        w = LCD_WIDTH - 4;
    return gfx_text((LCD_WIDTH - w) / 2, y, s, c, font, scale, LCD_WIDTH - 4);
}

/* ---- small widgets ------------------------------------------------------- */

void gfx_bar(int x, int y, int w, int h, int value, int max, uint16_t c)
{
    int fill;

    if (max <= 0) max = 1;
    if (value < 0) value = 0;
    if (value > max) value = max;
    fill = (int)((int64_t)w * value / max);
    gfx_round(x, y, w, h, 1, C_CARD_HI);
    if (fill > 0)
        gfx_round(x, y, fill < 2 ? 2 : fill, h, 1, c);
}

void gfx_switch(int x, int y, bool on)
{
    gfx_round(x, y, 20, 10, 2, on ? C_ACCENT : C_CARD_HI);
    gfx_round(on ? x + 11 : x + 1, y + 1, 8, 8, 1, on ? C_SEL_TEXT : C_DIM);
}
