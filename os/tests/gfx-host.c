/* B310E OS — ui/gfx.c against a reference gfx.c on the host (os/tests/test-gfx.py).
 *
 * Both are linked into this program: the reference with its public names
 * prefixed ref_ (test-gfx.py force-includes a header of #defines), drawing
 * into ref_back; the file under test into lcd_back. Every public gfx_*
 * function is called with random arguments (off-screen and negative
 * coordinates, zero and negative sizes, every font and scale, UTF-8 and
 * broken text, max_w, alpha, radius, every icon, blits, bars and graphs)
 * over backgrounds of solid theme colours and noise. After each call the
 * two frames, the return values and the missing-glyph counters must be
 * byte for byte the same, and every row the call changed must lie inside
 * the rows the file under test marked in lcd_dirty_y0..lcd_dirty_y1 (the
 * range lcd_update_drawn() compares; drivers/lcd.c). First the fonts are
 * checked for the zero padding nibble gfx.c's glyph loop relies on.
 *
 *   gfx-host <seed> <calls> [dirty 1|0]   exit 0: all the same; 1: the first difference
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "os/lib.h"
#include "os/status.h"
#include "os/ui.h"
#include "ui/font.h"

#define NPX (LCD_WIDTH * LCD_HEIGHT)

uint16_t lcd_back[NPX], ref_back[NPX];
volatile os_status_t os_status, ref_status;
int lcd_dirty_y0, lcd_dirty_y1, ref_dirty_y0, ref_dirty_y1;

void ref_gfx_fill(int x, int y, int w, int h, uint16_t c);
void ref_gfx_blit(int x, int y, int w, int h, const uint16_t *px);
void ref_gfx_shade(int x, int y, int w, int h, uint16_t c, int alpha);
void ref_gfx_hline(int x, int y, int w, uint16_t c);
void ref_gfx_rect(int x, int y, int w, int h, uint16_t c);
void ref_gfx_round(int x, int y, int w, int h, int r, uint16_t c);
void ref_gfx_round_alpha(int x, int y, int w, int h, int r, uint16_t c, int alpha);
void ref_gfx_icon(int x, int y, const icon_t *icon, uint16_t c);
int ref_gfx_text_height(int font, int scale);
int ref_gfx_text_width(const char *s, int font, int scale);
int ref_gfx_text(int x, int y, const char *s, uint16_t c, int font, int scale, int max_w);
int ref_gfx_text_centered(int y, const char *s, uint16_t c, int font, int scale);
void ref_gfx_bar(int x, int y, int w, int h, int value, int max, uint16_t c);
int ref_gfx_graph(int x, int y, int w, int h, const uint16_t *v, int n, int head, int max,
                  uint16_t c);
void ref_gfx_switch(int x, int y, bool on);

static uint32_t rs;

static uint32_t rnd32(void)
{
    rs ^= rs << 13;
    rs ^= rs >> 17;
    rs ^= rs << 5;
    return rs;
}

static int rnd(int lo, int hi)              /* lo..hi */
{
    return lo + (int)(rnd32() % (uint32_t)(hi - lo + 1));
}

static const uint16_t palette[] = {
    C_BG, C_BAR, C_CARD, C_CARD_HI, C_TEXT, C_DIM, C_ACCENT, C_SEL_TEXT, C_OK, C_BAD,
    0x0000, 0xffff, 0xf800, 0x07e0, 0x001f, 0x7bef, 0x8410,
};

static uint16_t colour(void)
{
    return rnd(0, 9) < 7 ? palette[rnd(0, (int)ARRAY_SIZE(palette) - 1)] : (uint16_t)rnd32();
}

static const icon_t *const icons[] = {
    &icon_apps, &icon_folder, &icon_file, &icon_music, &icon_radio, &icon_tools,
    &icon_chart, &icon_gear, &icon_power, &icon_torch, &icon_vibrate, NULL,
};

/* pieces text is made of: ASCII, what the huge font has, Latin-1, Latin
 * Extended-A, Cyrillic, signs, what no font has, broken sequences */
static const char *const bits[] = {
    "a", "W", "g", "j", " ", "Hello", "file name.mp3", "0", "1", "7", "12:34", ".", "-", "+",
    "%", ":", "100.0 MHz", "00:02.7", "?", "~", "\x7f", "\t", "\xc3\xa7", "\xc3\xbc",
    "\xc3\x96", "\xc4\x9f", "\xc5\x9f", "\xc4\xb1", "\xc4\xb0", "\xd0\x96", "\xd0\xb8",
    "\xd1\x91", "\xe2\x80\xa6", "\xe2\x82\xac", "\xe2\x84\x96", "\xe4\xb8\xad",
    "\xf0\x9f\x98\x80", "\xff", "\xfe", "\x80", "\xc3", "\xe2\x82", "\xc0\xaf",
    "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xc2\xa0", "\xc2\xb0",
};

static char text[160];

static const char *make_text(void)
{
    int n = rnd(0, 9) < 2 ? rnd(0, 1) : rnd(1, 14);
    size_t len = 0;

    text[0] = 0;
    for (int i = 0; i < n; i++) {
        const char *b = bits[rnd(0, (int)ARRAY_SIZE(bits) - 1)];
        size_t k = strlen(b);

        if (len + k >= sizeof(text))
            break;
        memcpy(text + len, b, k + 1);
        len += k;
    }
    return text;
}

static uint16_t blit_px[200 * 200];
static uint16_t graph_v[64];
static uint16_t before[NPX];

static int coord(int lo, int hi)
{
    return rnd(0, 19) ? rnd(lo, hi) : rnd(-1000, 1000);
}

static int size(int hi)
{
    int k = rnd(0, 19);

    return k == 0 ? rnd(-20, 0) : k == 1 ? rnd(0, 2) : k == 2 ? rnd(hi, hi + 400) : rnd(0, hi);
}

static int font(void)
{
    return rnd(0, 19) ? rnd(0, 3) : rnd(-3, 9);
}

static int alpha(void)
{
    int k = rnd(0, 19);

    return k == 0 ? rnd(-300, -1) : k == 1 ? rnd(256, 300) : k == 2 ? 0 : k == 3 ? 255 : rnd(0, 255);
}

int main(int argc, char **argv)
{
    unsigned long seed = argc > 1 ? strtoul(argv[1], NULL, 0) : 1;
    long calls = argc > 2 ? strtol(argv[2], NULL, 0) : 200000;
    bool dirty_check = argc > 3 ? atoi(argv[3]) != 0 : true;
    long counts[20] = { 0 };

    /* gfx.c's unclipped glyph loop relies on it: an odd width's rows end
     * in a zero nibble (os/tools/mkfont.py) */
    {
        const font_t *const fonts[] = { &font_small, &font_body, &font_title, &font_huge };

        for (int k = 0; k < 4; k++)
            for (int g = 0; g < fonts[k]->count; g++) {
                const font_glyph_t *gl = &fonts[k]->glyphs[g];
                int stride = (gl->w + 1) / 2;

                for (int r = 0; (gl->w & 1) && r < gl->h; r++)
                    if (fonts[k]->bits[gl->offset + r * stride + stride - 1] & 15) {
                        printf("font %d glyph U+%04x: row %d's padding nibble is not 0\n", k,
                               gl->code, r);
                        return 1;
                    }
            }
    }
    rs = (uint32_t)(seed * 2654435761u) | 1;
    for (int i = 0; i < NPX; i++)
        lcd_back[i] = ref_back[i] = (uint16_t)rnd32();
    for (int i = 0; i < (int)ARRAY_SIZE(blit_px); i++)
        blit_px[i] = (uint16_t)rnd32();

    for (long n = 0; n < calls; n++) {
        int op = rnd(0, 19), x = coord(-40, 170), y = coord(-40, 200);
        int w = size(140), h = size(170), r1 = 0, r2 = 0, fnt, scale, a, v, m;
        uint16_t c = colour();
        const char *s;
        char what[400];

        memcpy(before, lcd_back, sizeof(before));
        lcd_dirty_y0 = LCD_HEIGHT;
        lcd_dirty_y1 = 0;
        counts[op]++;
        switch (op) {
        case 0:                         /* solid background, as screens draw them */
        case 1:
            snprintf(what, sizeof(what), "gfx_fill(%d, %d, %d, %d, %04x)", x, y, w, h, c);
            gfx_fill(x, y, w, h, c);
            ref_gfx_fill(x, y, w, h, c);
            break;
        case 2: {                       /* noise, written directly into both */
            int x0 = rnd(0, LCD_WIDTH - 1), y0 = rnd(0, LCD_HEIGHT - 1);
            int x1 = rnd(x0, LCD_WIDTH - 1), y1 = rnd(y0, LCD_HEIGHT - 1);

            for (int yy = y0; yy <= y1; yy++)
                for (int xx = x0; xx <= x1; xx++)
                    lcd_back[yy * LCD_WIDTH + xx] = ref_back[yy * LCD_WIDTH + xx] = (uint16_t)rnd32();
            continue;                   /* not drawn through gfx: no dirty check */
        }
        case 3:
            snprintf(what, sizeof(what), "gfx_rect(%d, %d, %d, %d, %04x)", x, y, w, h, c);
            gfx_rect(x, y, w, h, c);
            ref_gfx_rect(x, y, w, h, c);
            break;
        case 4:
            snprintf(what, sizeof(what), "gfx_hline(%d, %d, %d, %04x)", x, y, w, c);
            gfx_hline(x, y, w, c);
            ref_gfx_hline(x, y, w, c);
            break;
        case 5:
            a = alpha();
            if (rnd(0, 1))
                w %= 60, h %= 60;
            snprintf(what, sizeof(what), "gfx_shade(%d, %d, %d, %d, %04x, %d)", x, y, w, h, c, a);
            gfx_shade(x, y, w, h, c, a);
            ref_gfx_shade(x, y, w, h, c, a);
            break;
        case 6:
            v = rnd(-3, 16);
            snprintf(what, sizeof(what), "gfx_round(%d, %d, %d, %d, %d, %04x)", x, y, w, h, v, c);
            gfx_round(x, y, w, h, v, c);
            ref_gfx_round(x, y, w, h, v, c);
            break;
        case 7:
            v = rnd(-3, 16);
            a = alpha();
            snprintf(what, sizeof(what), "gfx_round_alpha(%d, %d, %d, %d, %d, %04x, %d)", x, y, w, h,
                     v, c, a);
            gfx_round_alpha(x, y, w, h, v, c, a);
            ref_gfx_round_alpha(x, y, w, h, v, c, a);
            break;
        case 8:
        case 9: {
            const icon_t *ic = icons[rnd(0, (int)ARRAY_SIZE(icons) - 1)];

            x = coord(-14, 130);
            y = coord(-14, 162);
            snprintf(what, sizeof(what), "gfx_icon(%d, %d, %p, %04x)", x, y, (const void *)ic, c);
            gfx_icon(x, y, ic, c);
            ref_gfx_icon(x, y, ic, c);
            break;
        }
        case 10:
        case 11:
        case 12:
            s = make_text();
            fnt = font();
            scale = rnd(0, 3);
            m = rnd(0, 3) ? rnd(-10, 140) : 0;
            x = coord(-30, 130);
            y = coord(-30, 165);
            snprintf(what, sizeof(what), "gfx_text(%d, %d, \"%s\", %04x, %d, %d, %d)", x, y, s, c, fnt,
                     scale, m);
            r1 = gfx_text(x, y, s, c, fnt, scale, m);
            r2 = ref_gfx_text(x, y, s, c, fnt, scale, m);
            break;
        case 13:
            s = make_text();
            fnt = font();
            scale = rnd(0, 3);
            y = coord(-30, 165);
            snprintf(what, sizeof(what), "gfx_text_centered(%d, \"%s\", %04x, %d, %d)", y, s, c, fnt,
                     scale);
            r1 = gfx_text_centered(y, s, c, fnt, scale);
            r2 = ref_gfx_text_centered(y, s, c, fnt, scale);
            break;
        case 14:
            s = make_text();
            fnt = font();
            scale = rnd(0, 3);
            snprintf(what, sizeof(what), "gfx_text_width(\"%s\", %d, %d) / gfx_text_height", s, fnt,
                     scale);
            r1 = gfx_text_width(s, fnt, scale) * 1000 + gfx_text_height(fnt, scale);
            r2 = ref_gfx_text_width(s, fnt, scale) * 1000 + ref_gfx_text_height(fnt, scale);
            break;
        case 15:
            if (rnd(0, 1))
                w %= 200, h %= 200;
            if (w * h > (int)ARRAY_SIZE(blit_px))
                w = 200, h = 200;
            snprintf(what, sizeof(what), "gfx_blit(%d, %d, %d, %d)", x, y, w, h);
            gfx_blit(x, y, w, h, blit_px);
            ref_gfx_blit(x, y, w, h, blit_px);
            break;
        case 16:
            v = rnd(-20, 300);
            m = rnd(-5, 300);
            snprintf(what, sizeof(what), "gfx_bar(%d, %d, %d, %d, %d, %d, %04x)", x, y, w, h, v, m, c);
            gfx_bar(x, y, w, h, v, m, c);
            ref_gfx_bar(x, y, w, h, v, m, c);
            break;
        case 17: {
            int gn = rnd(0, 64), head = gn ? rnd(0, gn - 1) : 0;

            for (int i = 0; i < gn; i++)
                graph_v[i] = (uint16_t)(rnd(0, 3) ? (uint32_t)rnd(0, 1000) : rnd32());
            m = rnd(0, 2) ? 0 : rnd(-5, 1200);
            w = rnd(-5, 130);
            h = rnd(-5, 100);
            snprintf(what, sizeof(what), "gfx_graph(%d, %d, %d, %d, n %d, head %d, max %d, %04x)", x, y,
                     w, h, gn, head, m, c);
            r1 = gfx_graph(x, y, w, h, graph_v, gn, head, m, c);
            r2 = ref_gfx_graph(x, y, w, h, graph_v, gn, head, m, c);
            break;
        }
        default:
            v = rnd(0, 1);
            snprintf(what, sizeof(what), "gfx_switch(%d, %d, %d)", x, y, v);
            gfx_switch(x, y, v);
            ref_gfx_switch(x, y, v);
            break;
        }
        if (r1 != r2 || os_status.ui_missing_glyphs != ref_status.ui_missing_glyphs) {
            printf("call %ld (seed %lu): %s: returned %d, reference %d; missing glyphs %lu, reference %lu\n",
                   n, seed, what, r1, r2, (unsigned long)os_status.ui_missing_glyphs,
                   (unsigned long)ref_status.ui_missing_glyphs);
            return 1;
        }
        if (memcmp(lcd_back, ref_back, sizeof(lcd_back))) {
            int i = 0;

            while (lcd_back[i] == ref_back[i])
                i++;
            printf("call %ld (seed %lu): %s: pixel %d,%d is %04x, reference %04x (was %04x)\n", n, seed,
                   what, i % LCD_WIDTH, i / LCD_WIDTH, lcd_back[i], ref_back[i], before[i]);
            return 1;
        }
        for (int yy = 0; dirty_check && yy < LCD_HEIGHT; yy++)
            if ((yy < lcd_dirty_y0 || yy >= lcd_dirty_y1) &&
                memcmp(lcd_back + yy * LCD_WIDTH, before + yy * LCD_WIDTH, LCD_WIDTH * 2)) {
                printf("call %ld (seed %lu): %s: row %d changed outside the marked rows %d..%d\n", n,
                       seed, what, yy, lcd_dirty_y0, lcd_dirty_y1);
                return 1;
            }
    }
    printf("seed %lu: %ld calls, the same (fill %ld, rect %ld, hline %ld, shade %ld, round %ld, "
           "round_alpha %ld, icon %ld, text %ld, centred %ld, width %ld, blit %ld, bar %ld, graph %ld, "
           "switch %ld); missing glyphs %lu\n", seed, calls, counts[0] + counts[1], counts[3], counts[4],
           counts[5], counts[6], counts[7], counts[8] + counts[9], counts[10] + counts[11] + counts[12],
           counts[13], counts[14], counts[15], counts[16], counts[17], counts[18] + counts[19],
           (unsigned long)os_status.ui_missing_glyphs);
    return 0;
}
