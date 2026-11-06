/* B310E OS — ST7735S panel on the SC6530 LCM (parallel DBI) and LCDC.
 *
 * Ported from the Rockbox port's lcd-sc6530c.c (GPLv2-or-later, verified
 * on the phone), whose panel tables and DBI timing come from fpdoom
 * (Unlicense):
 *   LCM  0x20800000: command/data window at 0x60000000 (CS0)
 *   LCDC 0x20d00000: DMA from the frame buffer to the panel
 * The frame buffer is FRAME_ADDR, cached; lcd_update() cleans it from the
 * D-cache before the transfer. Every wait is bounded.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/arch.h"
#include "os/drivers.h"
#include "os/kernel.h"
#include "os/lib.h"

#define LCM_BASE      0x20800000u
#define LCM_CR(x)     REG32(LCM_BASE + (x))
#define LCM_WIN_CS0   0x60000000u
#define LCD_RST_REG   0x8b000224u
#define AHB_PWR_ON    0x20500060u
#define AHB_RST_SET   0x20500020u
#define AHB_RST_CLR   0x20500030u
#define B310E_MADCTL  0x10u          /* the BOE panel's mount orientation */

typedef volatile struct {
    uint32_t ctrl, disp_size, lcm_start, lcm_size, bg_color, fifo_status;
    uint32_t sync_delay, dummy0;
    struct {
        uint32_t ctrl, y_base_addr, uv_base_addr, size_xy, pitch, disp_xy;
        uint32_t dummy[6];
    } img;
    struct {
        uint32_t ctrl, base_addr, alpha_base_addr, size_xy, pitch, disp_xy;
        uint32_t alpha, grey_rgb, ck, dummy[3];
    } ocd[3];
    struct {
        uint32_t ctrl, base_addr, start_xy, size_xy, pitch, dummy[3];
    } cap;
    struct {
        uint32_t ctrl, contrast, saturation, brightness;
    } y2r;
    struct {
        uint32_t en, clr, status, raw;
    } irq;
} lcdc_t;

#define LCDC ((lcdc_t *)0x20d00000u)

#define LCM_CMD(cmd, len) 0x80 | (len), (cmd)
#define LCM_DELAY(ms)     0x40 | (((ms) >> 8) & 0x1f), ((ms) & 0xff)
#define LCM_END           0

static const uint8_t init_5ca1f1[] = {      /* ST7735 TNM */
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
    LCM_CMD(0xe0, 16), 0x13, 0x14, 0x06, 0x11, 0x29, 0x26, 0x21, 0x26,
        0x25, 0x26, 0x2e, 0x3b, 0x00, 0x03, 0x02, 0x06,
    LCM_CMD(0xe1, 16), 0x02, 0x25, 0x06, 0x11, 0x29, 0x26, 0x21, 0x26,
        0x26, 0x26, 0x2e, 0x3b, 0x00, 0x03, 0x02, 0x06,
    LCM_CMD(0x29, 0),
    LCM_END
};

static const uint8_t init_5cc0f1[] = {      /* ST7735 DTC */
    LCM_CMD(0x11, 0), LCM_DELAY(120),
    LCM_CMD(0xb1, 3), 0x04, 0x10, 0x10,
    LCM_CMD(0xb4, 1), 0x03,
    LCM_CMD(0xb6, 2), 0x17, 0x00,
    LCM_CMD(0xc0, 3), 0xa4, 0x04, 0x04,
    LCM_CMD(0xc1, 1), 0xc0,
    LCM_CMD(0xc2, 2), 0x0a, 0x00,
    LCM_CMD(0xc5, 1), 0x0f,
    LCM_CMD(0x3a, 1), 0x05,
    LCM_CMD(0xe0, 16), 0x02, 0x1f, 0x0b, 0x12, 0x36, 0x33, 0x2d, 0x31,
        0x2f, 0x2c, 0x33, 0x3b, 0x00, 0x02, 0x01, 0x02,
    LCM_CMD(0xe1, 16), 0x02, 0x1f, 0x0b, 0x12, 0x36, 0x32, 0x2d, 0x30,
        0x2f, 0x2c, 0x33, 0x3b, 0x00, 0x01, 0x00, 0x02,
    LCM_CMD(0x29, 0),
    LCM_END
};

static const uint8_t init_7c89f0[] = {      /* ST7735S BOE: the B310E's */
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
    LCM_CMD(0xe0, 16), 0x05, 0x19, 0x14, 0x17, 0x3d, 0x38, 0x2e, 0x2f,
        0x2d, 0x29, 0x31, 0x3b, 0x00, 0x03, 0x00, 0x10,
    LCM_CMD(0xe1, 16), 0x04, 0x15, 0x0e, 0x10, 0x31, 0x2d, 0x29, 0x2d,
        0x2b, 0x28, 0x2e, 0x39, 0x00, 0x01, 0x02, 0x10,
    LCM_CMD(0x35, 1), 0x00,
    LCM_CMD(0x3a, 1), 0x05,
    LCM_CMD(0x21, 0),
    LCM_CMD(0x29, 0),
    LCM_END
};

/* MADCTL (byte 2, per panel), full window, memory write */
static uint8_t init_window[] = {
    LCM_CMD(0x36, 1), 0x00,
    LCM_CMD(0x2a, 4), 0x00, 0x00, 0x00, LCD_WIDTH - 1,
    LCM_CMD(0x2b, 4), 0x00, 0x00, 0x00, LCD_HEIGHT - 1,
    LCM_CMD(0x2c, 0),
    LCM_END
};

typedef struct {
    uint32_t id;
    uint16_t mac_arg;
    uint8_t rcss, rlpw, rhpw, wcss, wlpw, whpw;
    const uint8_t *init;
} panel_t;

static const panel_t panels[] = {
    { 0x5cc0f1, 0xc8, 15, 45, 90, 5, 15, 40, init_5cc0f1 },
    { 0x7c89f0, 0xd0, 150, 150, 150, 150, 150, 150, init_7c89f0 },
    { 0x5ca1f1, 0x00, 15, 45, 90, 5, 15, 40, init_5ca1f1 },
};

static uint32_t panel_id;

static void lcm_wait_idle(void)
{
    uint32_t n = 1000000;

    while ((LCM_CR(0) & 2) && --n)
        ;
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

static void lcm_set_mode(uint32_t val)
{
    lcm_wait_idle();
    LCM_CR(0x10) = val;
}

static uint32_t ahb_freq(void)
{
#define X(a) ((a) / 26)
    uint32_t t = X(260) | X(2080) << 8 | X(1040) << 16 | X(1560) << 24;
#undef X
    uint32_t f = (t >> (REG32(0x8b000040) & 3) * 8 & 0xff) * 2600000;

    if (REG32(0x8b000040) & 1 << 2)
        f >>= 1;
    return f;
}

static void lcm_set_safe_freq(void)
{
    uint32_t sum = 30 << 16 | 6 | 0x80 | 30 << 21 | 14 << 8 | 6 << 4;

    LCM_CR(0x10) = 1;
    LCM_CR(0x14) = sum;
}

static void lcm_set_freq(uint32_t clk, const panel_t *p)
{
    unsigned sum;
    int t1, t2, t3;

    clk /= 1000000;
    LCM_CR(0) = 0x11110000;
#define CYC(r, v, max) r = (int)((clk * (v) - 1) / 1000 + 1); if (r > (max)) r = (max);
    CYC(t1, p->rcss, 6) sum = t1;
    CYC(t2, p->rlpw, 14) sum += t2;
    CYC(t3, p->rhpw, 14) sum += t3;
    if (sum > 30) sum = 30;
    sum = sum << 16 | t1 | 0x80;
    CYC(t1, p->wcss, 6) sum |= t1 << 4;
    CYC(t2, p->wlpw, 14) sum |= t2 << 8;
    CYC(t3, p->whpw, 14)
    t2 += t3 - 1 > t1 ? t3 - 1 : t1 + 1;
    sum |= t2 << 21;
#undef CYC
    lcm_wait_idle();
    LCM_CR(0x14) = sum;
}

static void lcm_exec(const uint8_t *p)
{
    for (;;) {
        uint32_t a = *p++, len;

        if (!a)
            break;
        len = a & 0x1f;
        a >>= 5;
        if (a == 4) {
            lcm_send(0, *p++);
            a = 0;
        }
        if (a == 0) {
            while (len--)
                lcm_send(1, *p++);
        } else if (a == 2) {
            udelay(1000 * ((len << 8) | *p++));
        }
    }
}

static void lcdc_setup(void)
{
    lcdc_t *lc = LCDC;
    uint32_t n = 1000000, a;

    REG32(AHB_PWR_ON) = 0x1000;          /* LCDC clock */
    REG32(AHB_RST_SET) = 0x200;
    udelay(10000);
    REG32(AHB_RST_CLR) = 0x200;

    lc->ctrl |= 1;
    lc->ctrl |= 2;
    lc->ctrl &= ~4u;
    lc->bg_color = 0;
    lc->disp_size = LCD_WIDTH | LCD_HEIGHT << 16;
    lc->lcm_start = 0;
    lc->lcm_size = LCD_WIDTH | LCD_HEIGHT << 16;

    lcm_set_mode(0x28);                  /* DBI sink: 8x2, DMA to CS0 data */
    lc->cap.ctrl |= 0x20;
    lc->cap.ctrl |= (lc->cap.ctrl & ~(3u << 6)) | 2u << 6;
    lc->ctrl &= ~(7u << 5);
    lc->cap.base_addr = (LCM_WIN_CS0 | 1u << 17) >> 2;

    lc->irq.en |= 1;                     /* one priming refresh */
    lc->ctrl |= 8;
    while (!(lc->irq.raw & 1) && --n)
        ;
    lc->irq.clr |= 1;
    lc->irq.en &= ~1u;

    a = lc->img.ctrl;                    /* source: RGB565, little endian */
    a &= ~2u;
    a = (a & ~(15u << 4)) | 5u << 4;
    a = (a & ~(3u << 8)) | 2u << 8;
    lc->img.ctrl = a;
    lc->img.pitch = LCD_WIDTH;
    lc->img.size_xy = LCD_WIDTH | LCD_HEIGHT << 16;
    lc->img.disp_xy = 0;
}

void lcd_init(void)
{
    const panel_t *p = &panels[0];
    uint32_t clk;
    unsigned i;

    memset(lcd_fb, 0, LCD_WIDTH * LCD_HEIGHT * 2);

    REG32(AHB_PWR_ON) = 0x40;            /* LCM clock */
    LCM_CR(0) = 0;
    LCM_CR(0x10) = 1;
    LCM_CR(0x14) = 0xa50100;
    for (i = 0; i < 3; i++) {            /* panel reset pulses */
        REG32(LCD_RST_REG) = (REG32(LCD_RST_REG) & ~1u) | (i & 1 ? 0 : 1);
        udelay(32000);
    }
    clk = ahb_freq();
    lcm_set_safe_freq();

    lcm_send(0, 0x04);                   /* read the panel id */
    panel_id = 0;
    for (i = 0; i < 4; i++)
        panel_id = panel_id << 8 | (lcm_recv(1) & 0xff);
    panel_id &= 0xffffff;
    for (i = 0; i < ARRAY_SIZE(panels); i++)
        if (panels[i].id == panel_id)
            p = &panels[i];
    lcm_set_freq(clk, p);

    lcm_exec(p->init);
    init_window[2] = panel_id == 0x7c89f0 ? B310E_MADCTL : (uint8_t)p->mac_arg;
    lcm_exec(init_window);

    lcdc_setup();
    LCDC->img.y_base_addr = FRAME_ADDR >> 2;
    LCDC->img.ctrl |= 1;
    lcd_update();
}

/* Start one transfer of the frame and return. The panel takes about 6 ms;
 * a new transfer waits (sleeping) until the previous one has had 8 ms,
 * so two never overlap. Polling for the LCDC's done flag instead cost
 * up to half the CPU (it does not report reliably, as Rockbox found). */
#define TRANSFER_MS 8

void lcd_update(void)
{
    static uint32_t last_kick;
    static bool kicked;
    lcdc_t *lc = LCDC;

    if (kicked) {
        uint32_t since = kernel_ticks() - last_kick;

        if (since < TRANSFER_MS) {
            if (kernel_running() && !arch_in_irq())
                thread_sleep(TRANSFER_MS - since);
            else
                udelay((TRANSFER_MS - since) * 1000);
        }
    }
    dcache_clean_range(lcd_fb, LCD_WIDTH * LCD_HEIGHT * 2);
    lc->irq.clr |= 1;
    lc->ctrl |= 8;
    last_kick = kernel_ticks();
    kicked = true;
}
