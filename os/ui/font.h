/* B310E OS — anti-aliased bitmap fonts (ui/font_montserrat.c, os/tools/mkfont.py).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef UI_FONT_H
#define UI_FONT_H

#include <stdint.h>

typedef struct {
    uint8_t code;               /* character */
    uint16_t offset;            /* into the font's bits */
    uint8_t w, h;               /* ink box */
    int8_t x, y;                /* ink box from the pen on the baseline */
    uint8_t advance;
} font_glyph_t;

typedef struct {
    uint8_t ascent, descent;
    uint8_t count;
    const font_glyph_t *glyphs; /* sorted by code */
    const uint8_t *bits;        /* 4-bit alpha, two pixels a byte, rows padded */
} font_t;

extern const font_t font_small, font_body, font_title, font_huge;

#endif
