/* B310E OS — drawing into the LCD frame buffer.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/drivers.h"
#include "os/lib.h"
#include "os/ui.h"

extern const uint8_t font5x7[95][5];
extern const uint8_t font8x16[96][16];

static inline void plot(int x, int y, uint16_t c)
{
    if ((unsigned)x < LCD_WIDTH && (unsigned)y < LCD_HEIGHT)
        lcd_fb[y * LCD_WIDTH + x] = c;
}

void gfx_fill(int x, int y, int w, int h, uint16_t c)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > LCD_WIDTH) w = LCD_WIDTH - x;
    if (y + h > LCD_HEIGHT) h = LCD_HEIGHT - y;
    for (int j = 0; j < h; j++) {
        uint16_t *p = lcd_fb + (y + j) * LCD_WIDTH + x;
        for (int i = 0; i < w; i++)
            p[i] = c;
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

static int cell_w(int font, int scale)
{
    return (font == FONT_LARGE ? 8 : 6) * scale;
}

static void glyph(int x, int y, unsigned char ch, uint16_t c, int font, int scale)
{
    if (ch < 0x20 || ch > 0x7e)
        ch = '?';
    if (font == FONT_LARGE) {
        const uint8_t *g = font8x16[ch - 0x20];
        for (int r = 0; r < 16; r++)
            for (int b = 0; b < 8; b++)
                if (g[r] & (0x80 >> b))
                    for (int sy = 0; sy < scale; sy++)
                        for (int sx = 0; sx < scale; sx++)
                            plot(x + b * scale + sx, y + r * scale + sy, c);
    } else {
        const uint8_t *g = font5x7[ch - 0x20];
        for (int col = 0; col < 5; col++)
            for (int r = 0; r < 7; r++)
                if (g[col] & (1 << r))
                    for (int sy = 0; sy < scale; sy++)
                        for (int sx = 0; sx < scale; sx++)
                            plot(x + col * scale + sx, y + r * scale + sy, c);
    }
}

int gfx_text_width(const char *s, int font, int scale)
{
    return (int)strlen(s) * cell_w(font, scale);
}

int gfx_text(int x, int y, const char *s, uint16_t c, int font, int scale,
             int max_w)
{
    int w = cell_w(font, scale), n = (int)strlen(s), fit = n;

    if (max_w > 0 && n * w > max_w) {
        fit = max_w / w - 2;            /* room for ".." */
        if (fit < 0)
            fit = 0;
    }
    for (int i = 0; i < fit; i++)
        glyph(x + i * w, y, (unsigned char)s[i], c, font, scale);
    if (fit < n) {
        glyph(x + fit * w, y, '.', c, font, scale);
        glyph(x + (fit + 1) * w, y, '.', c, font, scale);
        return (fit + 2) * w;
    }
    return n * w;
}

int gfx_text_centered(int y, const char *s, uint16_t c, int font, int scale)
{
    int w = gfx_text_width(s, font, scale);

    if (w > LCD_WIDTH)
        w = LCD_WIDTH;
    return gfx_text((LCD_WIDTH - w) / 2, y, s, c, font, scale, LCD_WIDTH);
}
