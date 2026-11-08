/* Low detail on this phone. The LCD shows 160 columns of the 320 either way, so
 * drawing 160 is half the work for what the screen can show. Each pixel goes
 * twice, side by side: here in one 16-bit write, where upstream's two byte
 * writes (Doom) each go to the slow memory by themselves. (--wrap in
 * chocolate.mk: the renderer takes these through its function pointers.)
 *
 * Heretic's and Hexen's low detail was never finished: their R_DrawColumnLow
 * and R_DrawSpanLow draw one column at x, so the view fills the screen's left
 * half. These two draw it right, the rest of their column drawers run twice
 * for a low-detail column, the sky comes from prepare.py, and it is on from
 * the start (R_Init).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>

extern uint8_t *ylookup[];
extern int columnofs[];
extern int centery;
extern int dc_x, dc_yl, dc_yh, dc_iscale, dc_texturemid;
extern const uint8_t *dc_colormap, *dc_source;
extern int ds_y, ds_x1, ds_x2, ds_xfrac, ds_yfrac, ds_xstep, ds_ystep;
extern const uint8_t *ds_colormap, *ds_source;

#define SCREEN_HALFWORDS 160            /* SCREENWIDTH in 16-bit steps */

/* (x doubled is even, and the view's left edge is too: scaledviewwidth is in
 * 32s, so the writes are aligned) */
void __wrap_R_DrawColumnLow(void)
{
    int count = dc_yh - dc_yl;
    const uint8_t *colormap = dc_colormap, *source = dc_source;
    uint16_t *dest;
    int frac, step;

    if (count < 0)
        return;
    dest = (uint16_t *)(void *)(ylookup[dc_yl] + columnofs[dc_x << 1]);
    step = dc_iscale;
    frac = dc_texturemid + (dc_yl - centery) * step;
    do {
        *dest = (uint16_t)(colormap[source[(frac >> 16) & 127]] * 0x0101u);
        dest += SCREEN_HALFWORDS;
        frac += step;
    } while (count--);
}

void __wrap_R_DrawSpanLow(void)
{
    uint32_t position = (((uint32_t)ds_xfrac << 10) & 0xffff0000u) | (((uint32_t)ds_yfrac >> 6) & 0xffffu);
    uint32_t step = (((uint32_t)ds_xstep << 10) & 0xffff0000u) | (((uint32_t)ds_ystep >> 6) & 0xffffu);
    const uint8_t *colormap = ds_colormap, *source = ds_source;
    uint16_t *dest = (uint16_t *)(void *)(ylookup[ds_y] + columnofs[ds_x1 << 1]);
    int count = ds_x2 - ds_x1;

    do {
        *dest++ = (uint16_t)(colormap[source[((position >> 4) & 0x0fc0u) | (position >> 26)]] * 0x0101u);
        position += step;
    } while (count--);
}

#if !defined(GAME_DOOM)
extern int detailshift, detailLevel;

#define TWICE(name)                                             \
    void __real_##name(void);                                   \
    void __wrap_##name(void)                                    \
    {                                                           \
        int x = dc_x;                                           \
                                                                \
        if (!detailshift) {                                     \
            __real_##name();                                    \
            return;                                             \
        }                                                       \
        dc_x = x << 1;                                          \
        __real_##name();                                        \
        dc_x = (x << 1) + 1;                                    \
        __real_##name();                                        \
        dc_x = x;                                               \
    }

TWICE(R_DrawTLColumn)
TWICE(R_DrawTranslatedColumn)
TWICE(R_DrawTranslatedTLColumn)
#if defined(GAME_HEXEN)
TWICE(R_DrawAltTLColumn)
TWICE(R_DrawTranslatedAltTLColumn)
#endif

void __real_R_Init(void);

void __wrap_R_Init(void)
{
    detailLevel = 1;
    __real_R_Init();
}
#endif
