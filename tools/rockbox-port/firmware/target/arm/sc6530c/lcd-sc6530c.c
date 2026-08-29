/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2007 by Greg White
 * Copyright (C) 2009 by Bob Cousins
 * Copyright (C) 2026 by B310E-OS project
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "lcd.h"
#include "lcd-target.h"
#include <string.h>

/*
 * B310E-OS Rockbox port — b310e/target/arm/sc6530c/lcd-sc6530c.c
 * (GPLv2, Rockbox-derived; modeled on Rockbox's s3c2440/lcd-s3c2440.c,
 * the init sequence is a port of the B310E-OS drivers/lcd.c — fpdoom
 * syscode.c, Unlicense).
 *
 * The panel is a Sitronix ST7735S BOE 128x160 RGB565 hung off the SC6530
 * parallel DBI:
 *   LCM  controller @ 0x20800000 — 16-bit command/data window
 *   LCDC controller @ 0x20d00000 — DMA image source -> panel
 *
 * Rockbox draws into ITS OWN framebuffer (FBADDR, in .bss); lcd-memframe.c
 * calls lcd_copy_buffer_rect() (below) to copy it to FRAME — the fixed
 * physical address the LCDC DMA reads (firmware/export/sc6530c.h) — then
 * we trigger one bounded DMA refresh. The crt0 identity MMU makes FRAME
 * the same address for the CPU and the DMA, and everything is uncached in
 * M1 so no flush is strictly needed; commit_dcache() is kept for the
 * discipline (it is a no-op while the D-cache is disabled).
 *
 * SAFETY: no 0x8c000000 pinmux writes (they hang the phone); all waits are
 * bounded so a wedged LCM/LCDC degrades to a no-op instead of hanging the
 * Rockbox kernel.
 */

#define B310E_MADCTL 0x10u   /* BOE mount orientation (B310E-OS arch.h) */

#define REG32(a) (*(volatile uint32_t *)(a))
#define REG16(a) (*(volatile uint16_t *)(a))

/* ---- SoC register map (B310E-OS drivers/lcd.c, fpdoom Unlicense) ------ */
#define LCM_REG_BASE  0x20800000u
#define LCM_CR(x)     REG32(LCM_REG_BASE + (x))
#define LCM_WIN_CS0   0x60000000u   /* CS0 data window */
#define LCM_CS        0u
#define LCD_RST_REG   0x8b000224u   /* GPIO-style AHB reg, NOT a pinmux */
#define AHB_PWR_ON    0x20500060u
#define AHB_RST_SET   0x20500020u
#define AHB_RST_CLR   0x20500030u

/* ---- LCDC display controller register map (syscode.h:34-56 mirror) ---- */
typedef volatile struct {
    uint32_t ctrl;              /* 0x00 */
    uint32_t disp_size;         /* 0x04 */
    uint32_t lcm_start;         /* 0x08 */
    uint32_t lcm_size;          /* 0x0c */
    uint32_t bg_color;          /* 0x10 */
    uint32_t fifo_status;       /* 0x14 */
    uint32_t sync_delay;        /* 0x18 */
    uint32_t dummy[1];          /* 0x1c */
    struct {                    /* 0x20 */
        uint32_t ctrl;          /* 0x20 */
        uint32_t y_base_addr;   /* 0x24 */
        uint32_t uv_base_addr;  /* 0x28 */
        uint32_t size_xy;       /* 0x2c */
        uint32_t pitch;         /* 0x30 */
        uint32_t disp_xy;       /* 0x34 */
        uint32_t dummy[6];      /* 0x38..0x4c */
    } img;
    struct {                    /* 0x50, 0x80, 0xb0 */
        uint32_t ctrl;
        uint32_t base_addr;
        uint32_t alpha_base_addr;
        uint32_t size_xy;
        uint32_t pitch;
        uint32_t disp_xy;
        uint32_t alpha;
        uint32_t grey_rgb;
        uint32_t ck;
        uint32_t dummy[3];
    } ocd1, ocd2, ocd3;
    struct {                    /* 0xe0 */
        uint32_t ctrl;          /* 0xe0 */
        uint32_t base_addr;     /* 0xe4 */
        uint32_t start_xy;      /* 0xe8 */
        uint32_t size_xy;       /* 0xec */
        uint32_t pitch;         /* 0xf0 */
        uint32_t dummy[3];      /* 0xf4..0xfc */
    } cap;
    struct {                    /* 0x100 */
        uint32_t ctrl, contrast, saturation, brightness;
    } y2r;
    struct {                    /* 0x110 */
        uint32_t en;            /* 0x110 */
        uint32_t clr;           /* 0x114 */
        uint32_t status;        /* 0x118 */
        uint32_t raw;           /* 0x11c */
    } irq;
} lcdc_t;

#define LCDC ((lcdc_t *)0x20d00000u)

/* ---- ST7735 init sequences (fpdoom lcd_config.h, Unlicense) ----------- */

#define LCM_CMD(cmd, len) 0x80 | (len), (cmd)
#define LCM_DELAY(ms)     0x40 | (((ms) >> 8) & 0x1f), ((ms) & 0xff)
#define LCM_END           0

/* ST7735 TNM (id 0x5ca1f1) */
static const uint8_t cmd5ca1f1_init[] = {
    LCM_CMD(0x11, 0), LCM_DELAY(120),
    LCM_CMD(0xb1, 3), 0x0f, 0x04, 0x04,
    LCM_CMD(0xb2, 3), 0x05, 0x3a, 0x3a,
    LCM_CMD(0xb3, 6), 0x05, 0x3a, 0x3a, 0x05, 0x3a, 0x3a,
    LCM_CMD(0xb4, 1), 0x00,
    LCM_CMD(0xc0, 3), 0xc3, 0x06, 0x44,
    LCM_CMD(0xc1, 1), 0xc2,
    LCM_CMD(0xc2, 2), 0x0d, 0x00,
    LCM_CMD(0xc3, 2), 0xdb, 0x2a,
    LCM_CMD(0xc4, 2), 0x8b, 0xee,
    LCM_CMD(0xc5, 1), 0x0d,
    LCM_CMD(0x3a, 1), 0x05,
    LCM_CMD(0xe0, 16), 0x13, 0x14, 0x06, 0x11,
        0x29, 0x26, 0x21, 0x26, 0x25, 0x26, 0x2e, 0x3b,
        0x00, 0x03, 0x02, 0x06,
    LCM_CMD(0xe1, 16), 0x02, 0x25, 0x06, 0x11,
        0x29, 0x26, 0x21, 0x26, 0x26, 0x26, 0x2e, 0x3b,
        0x00, 0x03, 0x02, 0x06,
    LCM_CMD(0x29, 0),
    LCM_END
};

/* ST7735 DTC (id 0x5cc0f1) */
static const uint8_t cmd5cc0f1_init[] = {
    LCM_CMD(0x11, 0), LCM_DELAY(120),
    LCM_CMD(0xb1, 3), 0x04, 0x10, 0x10,
    LCM_CMD(0xb4, 1), 0x03,
    LCM_CMD(0xb6, 2), 0x17, 0x00,
    LCM_CMD(0xc0, 3), 0xa4, 0x04, 0x04,
    LCM_CMD(0xc1, 1), 0xc0,
    LCM_CMD(0xc2, 2), 0x0a, 0x00,
    LCM_CMD(0xc5, 1), 0x0f,
    LCM_CMD(0x3a, 1), 0x05,
    LCM_CMD(0xe0, 16), 0x02, 0x1f, 0x0b, 0x12,
        0x36, 0x33, 0x2d, 0x31, 0x2f, 0x2c, 0x33, 0x3b,
        0x00, 0x02, 0x01, 0x02,
    LCM_CMD(0xe1, 16), 0x02, 0x1f, 0x0b, 0x12,
        0x36, 0x32, 0x2d, 0x30, 0x2f, 0x2c, 0x33, 0x3b,
        0x00, 0x01, 0x00, 0x02,
    LCM_CMD(0x29, 0),
    LCM_END
};

/* ST7735S BOE (id 0x7c89f0) — the B310E's panel */
static const uint8_t cmd7c89f0_init[] = {
    LCM_CMD(0x11, 0), LCM_DELAY(120),
    LCM_CMD(0xb1, 3), 0x05, 0x3c, 0x3c,
    LCM_CMD(0xb2, 3), 0x05, 0x3c, 0x3c,
    LCM_CMD(0xb3, 6), 0x05, 0x3c, 0x3c, 0x05, 0x3c, 0x3c,
    LCM_CMD(0xb4, 2), 0x03, 0x02,
    LCM_CMD(0xc0, 3), 0xa4, 0x04, 0x84,
    LCM_CMD(0xc1, 1), 0xc4,
    LCM_CMD(0xc2, 2), 0x0d, 0x00,
    LCM_CMD(0xc3, 2), 0x8d, 0x2a,
    LCM_CMD(0xc4, 2), 0x8d, 0xee,
    LCM_CMD(0xc5, 1), 0x04,
    LCM_CMD(0xe0, 16), 0x05, 0x19, 0x14, 0x17,
        0x3d, 0x38, 0x2e, 0x2f, 0x2d, 0x29, 0x31, 0x3b,
        0x00, 0x03, 0x00, 0x10,
    LCM_CMD(0xe1, 16), 0x04, 0x15, 0x0e, 0x10,
        0x31, 0x2d, 0x29, 0x2d, 0x2b, 0x28, 0x2e, 0x39,
        0x00, 0x01, 0x02, 0x10,
    LCM_CMD(0x35, 1), 0x00,
    LCM_CMD(0x3a, 1), 0x05,
    LCM_CMD(0x21, 0),
    LCM_CMD(0x29, 0),
    LCM_END
};

/* MADCTL + window + Memory Write (cmd_init2, syscode.c:689-703).
 * MADCTL byte [2] is patched per panel in lcd_init_device(). */
static uint8_t cmd_init2[] = {
    LCM_CMD(0x36, 1), 0x00,
    LCM_CMD(0x2a, 4), 0x00, 0x00, 0x00, (uint8_t)(LCD_WIDTH - 1),
    LCM_CMD(0x2b, 4), 0x00, 0x00, 0x00, (uint8_t)(LCD_HEIGHT - 1),
    LCM_CMD(0x2c, 0),
    LCM_END
};

typedef struct {
    uint32_t id;
    uint16_t width, height;
    uint16_t mac_arg;
    struct {
        uint8_t rcss, rlpw, rhpw, wcss, wlpw, whpw;
    } mcu_timing;
    struct { uint32_t freq; } spi;
    const uint8_t *cmd_init;
} lcd_config_t;

static const lcd_config_t lcd_config[] = {
    { 0x5cc0f1, 128, 160, 0xc8, { 15, 45, 90, 5, 15, 40 }, { 0 }, cmd5cc0f1_init },
    { 0x7c89f0, 128, 160, 0xd0, { 150, 150, 150, 150, 150, 150 }, { 0 }, cmd7c89f0_init },
    { 0x5ca1f1, 128, 160, 0x00, { 15, 45, 90, 5, 15, 40 }, { 0 }, cmd5ca1f1_init },
};

/* ---- low-level DBI access (fpdoom syscode.c, Unlicense) ---------------- */

#define LCM_BUSY_BUDGET 1000000u

static void lcm_wait_idle(void)
{
    uint32_t n = LCM_BUSY_BUDGET;

    while (LCM_CR(0) & 2)
        if (--n == 0) break;
}

static void lcm_send(unsigned idx, unsigned val)
{
    lcm_wait_idle();
    REG16(LCM_WIN_CS0 | (idx << 17)) = (uint16_t)val;
}

static uint16_t lcm_recv(unsigned idx)
{
    lcm_wait_idle();
    return REG16(LCM_WIN_CS0 | (idx << 17));
}

static void lcm_send_cmd(uint16_t cmd)  { lcm_send(0, cmd); }
static void lcm_send_data(uint16_t val) { lcm_send(1, val); }

static void lcm_set_mode(uint32_t val)
{
    lcm_wait_idle();
    LCM_CR(0x10 + (LCM_CS << 4)) = val;
}

/* ---- DBI clock timing (fpdoom syscode.c:307-392, Unlicense) ----------- */

static uint32_t lcd_get_cpu_freq(void)
{
#define X(a) (a / 26)
#define X2(x, a, b, c, d) uint32_t x = X(a) | X(b) << 8 | X(c) << 16 | X(d) << 24;
    X2(t1, 260, 1040, 2080, 2080)
    X2(t2, 260, 2080, 2496, 3120)
    X2(t3, 260, 2080, 1040, 1560)
#undef X2
#undef X
    uint32_t t = t3, addr = 0x8b000040;   /* SC6530 path */

    (void)t1;
    (void)t2;
    return (t >> (REG32(addr) & 3) * 8 & 0xff) * 2600000;
}

static uint32_t lcd_get_ahb_freq(void)
{
    uint32_t freq = lcd_get_cpu_freq();

    if (REG32(0x8b000040) & 1 << 2)
        freq >>= 1;
    return freq;
}

static void lcm_set_safe_freq(void)
{
    uint32_t addr = LCM_REG_BASE + 0x10 + (LCM_CS << 4);
    uint32_t sum = 30 << 16 | 6 | 0x80;

    sum |= 30 << 21 | 14 << 8 | 6 << 4;
    REG32(addr) = 1;
    REG32(addr + 4) = sum;
}

static void lcm_set_freq(uint32_t clk_rate, const lcd_config_t *lcd)
{
    unsigned sum; int t1, t2, t3;

    if (clk_rate > 1000000) clk_rate /= 1000000;
    LCM_CR(0) = 0x11110000;

#define DBI_CYCLES(r, name, max) \
    r = (clk_rate * lcd->mcu_timing.name - 1) / 1000 + 1; \
    if (r > max) r = max;

    DBI_CYCLES(t1, rcss, 6) sum = t1;
    DBI_CYCLES(t2, rlpw, 14) sum += t2;
    DBI_CYCLES(t3, rhpw, 14) sum += t3;
    if (sum > 30) sum = 30;
    sum = sum << 16 | t1 | 0x80;

    DBI_CYCLES(t1, wcss, 6) sum |= t1 << 4;
    DBI_CYCLES(t2, wlpw, 14) sum |= t2 << 8;
    DBI_CYCLES(t3, whpw, 14)
    t2 += t3 - 1 > t1 ? t3 - 1 : t1 + 1;
    sum |= t2 << 21;
#undef DBI_CYCLES

    lcm_wait_idle();
    LCM_CR(0x14 + (LCM_CS << 4)) = sum;
}

static uint32_t lcd_getid(void)
{
    uint32_t ret = 0, n = 4;

    lcm_send_cmd(0x04);               /* RDDID */
    do ret = ret << 8 | (lcm_recv(1) & 0xff); while (--n);
    return ret & 0xffffff;
}

static void lcm_exec(const uint8_t *p)
{
    for (;;) {
        uint32_t a = *p++, len;

        if (!a) break;
        len = a & 0x1f;
        a >>= 5;
        if (a == 4) {
            lcm_send_cmd(*p++);
            a = 0;
        }
        if (a == 0) {
            while (len--) lcm_send_data(*p++);
        } else if (a == 2) {
            udelay(1000 * ((len << 8) | *p++));
        }
    }
}

static void lcd_reset(void)
{
    uint32_t addr = LCD_RST_REG, t;
    int i, method = 0;

    method &= 1;
    for (i = 0; i < 3; i++) {
        t = REG32(addr) & ~(uint32_t)method;
        t |= (uint32_t)(method ^= 1);
        REG32(addr) = t;
        udelay(32000);
    }
}

static void lcdc_init_regs(void)
{
    lcdc_t *lcdc = LCDC;
    unsigned w = LCD_WIDTH, h = LCD_HEIGHT;
    unsigned w2 = LCD_WIDTH, h2 = LCD_HEIGHT;

    REG32(AHB_PWR_ON) = 0x1000;              /* LCDC enable */

    REG32(AHB_RST_SET) = 0x200;              /* AHB soft reset bit 9 */
    udelay(10000);
    REG32(AHB_RST_CLR) = 0x200;

    lcdc->ctrl |= 1;
    lcdc->ctrl |= 2;
    lcdc->ctrl &= ~4u;
    lcdc->bg_color = 0x000000;

    lcdc->disp_size = w | h << 16;
    lcdc->lcm_start = 0;
    lcdc->lcm_size = w | h << 16;

    /* DBI sink: 8x2 BE mode, DMA writes the CS0 data window. */
    lcm_set_mode(0x28);
    lcdc->cap.ctrl |= 0x20;
    lcdc->cap.ctrl |= (lcdc->cap.ctrl & ~(3u << 6)) | 2u << 6;
    lcdc->ctrl &= ~(7u << 5);
    lcdc->cap.base_addr = (LCM_WIN_CS0 | 1u << 17) >> 2;

    /* One dummy refresh to clock the panel pipeline (bounded — a one-time
     * boot prime; ~1M iterations ≈ 5 ms is plenty, no need for the old
     * 100M budget). */
    {
        uint32_t n = 1000000u;

        lcdc->irq.en |= 1;
        lcdc->ctrl |= 8;
        while (!(lcdc->irq.raw & 1) && --n) ;
        lcdc->irq.clr |= 1;
    }
    lcdc->irq.en &= ~1u;

    /* image source config: RGB565, little endian */
    {
        uint32_t a = lcdc->img.ctrl;
        int fmt = 5;

        a &= ~2u;
        a = (a & ~(15u << 4)) | fmt << 4;
        a = (a & ~(3u << 8)) | 2u << 8;
        lcdc->img.ctrl = a;
    }

    lcdc->img.pitch = w2;
    w2 = w2 > w ? w : w2;
    h2 = h2 > h ? h : h2;
    w2 &= ~1u;
    lcdc->img.size_xy = w2 | h2 << 16;
    lcdc->img.disp_xy = ((w - w2) >> 1) | ((h - h2) >> 1) << 16;
}

/* ---- Rockbox target API ------------------------------------------------ */

void lcd_init_device(void)
{
    uint32_t id, clk_rate;
    const lcd_config_t *lcd;
    unsigned i;

    /* LCM power, panel reset, safe DBI baseline, id read, then the REAL
     * DBI timing from the AHB clock (fpdoom lcm_init, syscode.c:507-551). */
    REG32(AHB_PWR_ON) = 0x40;                /* LCM enable */
    LCM_CR(0) = 0;
    LCM_CR(0x10) = 1;
    LCM_CR(0x14) = 0xa50100;
    lcd_reset();
    clk_rate = lcd_get_ahb_freq();
    lcm_set_safe_freq();
    id = lcd_getid();
    lcd = &lcd_config[0];
    for (i = 0; i < sizeof(lcd_config) / sizeof(*lcd_config); i++)
        if (id == lcd_config[i].id && lcd_config[i].mcu_timing.rcss)
            lcd = &lcd_config[i];
    lcm_set_freq(clk_rate, lcd);

    /* Panel init table, then the MADCTL + window + 0x2c handshake (the
     * ONLY place 0x2c appears — never in the refresh path). */
    lcm_exec(lcd->cmd_init);
    if (id == 0x7c89f0)
        cmd_init2[2] = B310E_MADCTL;         /* BOE mount orientation */
    else
        cmd_init2[2] = (uint8_t)lcd->mac_arg;
    lcm_exec(cmd_init2);

    /* LCDC engine + framebuffer base = FRAME (word address). The CPU and
     * the LCDC DMA share the same physical address (identity MMU). */
    lcdc_init_regs();
    LCDC->img.y_base_addr = (uint32_t)FRAME >> 2;
    LCDC->img.ctrl |= 1;
}

/* Called by lcd-memframe.c: copy a Rockbox framebuffer rectangle to FRAME
 * (the LCDC DMA source), then trigger one bounded DMA refresh. Everything
 * is uncached in M1 so the copy is already coherent with the DMA;
 * commit_dcache() is kept for discipline (no-op while D-cache is off). */
void lcd_copy_buffer_rect(fb_data *dst, const fb_data *src,
                          int width, int height)
{
    lcdc_t *lc = LCDC;
    int r;

    /* Two call shapes (lcd-memframe.c): full-frame / full-width rects call
     * with (width = total pixels, height = 1) — one contiguous copy; the
     * line-by-line callers pass (width = pixels per row, height = row
     * count) — copy PER ROW with a stride of LCD_WIDTH pixels (a flat
     * memcpy of width*height would smear/truncate centered rects, e.g.
     * cut popups in half). */
    if (height == 1)
        memcpy(dst, src, width * sizeof(fb_data));
    else
        for (r = 0; r < height; r++)
            memcpy(dst + r * LCD_WIDTH, src + r * LCD_WIDTH,
                   width * sizeof(fb_data));

    /* commit_dcache() flushes the cached PSRAM framebuffer so the LCDC
     * DMA reads committed pixels (the B310E-OS clean_dcache discipline). */
    commit_dcache();

    /* Fire-and-forget refresh: the LCDC continuously scans FRAME
     * (img.ctrl |= 1 at init), so the ctrl|=8 kick pushes the pixels
     * regardless of the completion flag. The OLD code polled irq.raw for
     * up to 100M iterations (~1 s) PER FRAME — irq.raw never asserts
     * reliably in the Rockbox context, so every frame burned the full
     * budget (the user's "~1 s per frame draw"). The ~6 ms DBI DMA push
     * fits easily between Rockbox's ~20-33 ms frame cadence, so a short
     * bounded spin (~50 µs) is only there to avoid stacking two kicks on
     * the same frame; we proceed regardless. */
    lc->irq.en |= 1;
    lc->ctrl |= 8;
    {
        uint32_t wait = 10000u;

        while (!(lc->irq.raw & 1) && --wait) ;
        lc->irq.clr |= 1;
    }
}

void lcd_set_flip(bool yesno)
{
    (void)yesno;
    /* Not implemented (no HAVE_LCD_FLIP). */
}

void lcd_set_invert_display(bool yesno)
{
    (void)yesno;
    /* Not implemented (no HAVE_LCD_INVERT). */
}

int lcd_default_contrast(void)
{
    return 0;   /* no contrast control on the ST7735 */
}

void lcd_set_contrast(int val)
{
    (void)val;
    /* Not implemented. */
}

/* ---- YUV420 -> RGB565 line writers (called by lcd-memframe.c's YUV
 * blit, used by the video/thumbnail code). Plain C, no dithering — a
 * correctness-first fallback for M1 (the asm versions are a performance
 * optimization). Each call writes TWO luma lines (one U/V pair). */

static fb_data yuv_pixel(int y, int u, int v)
{
    int r = y + (351 * (v - 128) >> 8);
    int g = y - ((86 * (u - 128) + 179 * (v - 128)) >> 8);
    int b = y + (443 * (u - 128) >> 8);
    uint16_t c;

    if (r < 0) r = 0; else if (r > 255) r = 255;
    if (g < 0) g = 0; else if (g > 255) g = 255;
    if (b < 0) b = 0; else if (b > 255) b = 255;
    c = (uint16_t)((r & 0xf8) << 8 | (g & 0xfc) << 3 | b >> 3);
    return (fb_data)((c >> 8) | (c << 8));   /* RGB565 -> LCDC little-endian */
}

void lcd_write_yuv420_lines(fb_data *dst,
                            unsigned char const * const src[3],
                            int width, int stride)
{
    int x;

    for (x = 0; x < width; x++) {
        int u = src[1][x >> 1], v = src[2][x >> 1];
        dst[x] = yuv_pixel(src[0][x], u, v);
        dst[LCD_WIDTH + x] = yuv_pixel(src[0][stride + x], u, v);
    }
}

void lcd_write_yuv420_lines_odither(fb_data *dst,
                                    unsigned char const * const src[3],
                                    int width, int stride,
                                    int x_screen, int y_screen)
{
    (void)x_screen;
    (void)y_screen;
    lcd_write_yuv420_lines(dst, src, width, stride);
}
