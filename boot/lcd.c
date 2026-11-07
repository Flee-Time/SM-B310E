/* B310E boot loader — the ST7735S panel and text drawing.
 *
 * The panel bring-up is B310E OS drivers/lcd.c (from the Rockbox port,
 * verified on the phone): LCM (parallel DBI, 0x20800000) for commands, the
 * LCDC (0x20d00000) to send the frame from PSRAM. One full frame per
 * flush, then the frame's time is waited out before the next drawing
 * (the done flag is unreliable, as Rockbox found).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "boot.h"

#define LCM_BASE      0x20800000u
#define LCM_CR(x)     REG32(LCM_BASE + (x))
#define LCM_WIN_CS0   0x60000000u
#define LCD_RST_REG   0x8b000224u
#define AHB_PWR_ON    0x20500060u
#define AHB_RST_SET   0x20500020u
#define AHB_RST_CLR   0x20500030u

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
#define FB   ((uint16_t *)FRAME_ADDR)

#define LCM_CMD(cmd, len) 0x80 | (len), (cmd)
#define LCM_DELAY(ms)     0x40 | (((ms) >> 8) & 0x1f), ((ms) & 0xff)
#define LCM_END           0

/* ST7735S BOE (id 0x7c89f0), the B310E's panel */
static const uint8_t init_boe[] = {
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

static const uint8_t init_window[] = {
    LCM_CMD(0x36, 1), 0x10,                     /* the BOE panel's mount */
    LCM_CMD(0x2a, 4), 0x00, 0x00, 0x00, LCD_W - 1,
    LCM_CMD(0x2b, 4), 0x00, 0x00, 0x00, LCD_H - 1,
    LCM_CMD(0x2c, 0),
    LCM_END
};

static uint32_t frame_ms = 16;
static uint32_t lcm_poweron[8];         /* LCM 0x00..0x1c before lcd_start */
static bool lcm_saved;
static uint32_t last_kick;
static bool kicked;

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
            ms_wait((len << 8) | *p++);
        }
    }
}

/* DBI timing for the BOE panel's 150 ns cycles at the AHB clock (104 MHz
 * after chip_init): B310E OS lcm_set_freq() with its numbers. */
static void lcm_timing(void)
{
    const uint32_t clk = 104;
    int t1, t2, t3;
    uint32_t sum;

#define CYC(r, v, max) r = (int)((clk * (v) - 1) / 1000 + 1); if (r > (max)) r = (max);
    LCM_CR(0) = 0x11110000;
    CYC(t1, 150, 6) sum = (uint32_t)t1;
    CYC(t2, 150, 14) sum += (uint32_t)t2;
    CYC(t3, 150, 14) sum += (uint32_t)t3;
    if (sum > 30) sum = 30;
    sum = sum << 16 | (uint32_t)t1 | 0x80;
    CYC(t1, 150, 6) sum |= (uint32_t)t1 << 4;
    CYC(t2, 150, 14) sum |= (uint32_t)t2 << 8;
    CYC(t3, 150, 14)
    t2 += t3 - 1 > t1 ? t3 - 1 : t1 + 1;
    sum |= (uint32_t)t2 << 21;
#undef CYC
    lcm_wait_idle();
    LCM_CR(0x14) = sum;
    frame_ms = (uint32_t)t2 * 2u * LCD_W * LCD_H / clk / 1000u;
    frame_ms += frame_ms / 4 + 2;
}

static void lcdc_setup(void)
{
    lcdc_t *lc = LCDC;
    uint32_t n = 1000000, a;

    REG32(AHB_PWR_ON) = 0x1000;          /* LCDC clock */
    REG32(AHB_RST_SET) = 0x200;
    ms_wait(10);
    REG32(AHB_RST_CLR) = 0x200;

    lc->ctrl |= 1;
    lc->ctrl |= 2;
    lc->ctrl &= ~4u;
    lc->bg_color = 0;
    lc->disp_size = LCD_W | LCD_H << 16;
    lc->lcm_start = 0;
    lc->lcm_size = LCD_W | LCD_H << 16;

    lcm_wait_idle();
    LCM_CR(0x10) = 0x28;                 /* DBI sink: 8x2, DMA to CS0 data */
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
    lc->img.pitch = LCD_W;
    lc->img.size_xy = LCD_W | LCD_H << 16;
    lc->img.disp_xy = 0;
}

/* Returns the panel id. Only the BOE table is carried: the B310E has
 * that panel (B310E OS reads 0x7c89f0 on the phone); another id still
 * gets it, and the id goes in the status. */
uint32_t lcd_start(void)
{
    uint32_t id = 0;
    unsigned i;

    memset(FB, 0, LCD_W * LCD_H * 2);
    REG32(AHB_PWR_ON) = 0x40;            /* LCM clock */
    for (i = 0; i < 8; i++)              /* as the stock firmware will find it */
        lcm_poweron[i] = LCM_CR(4 * i);
    lcm_saved = true;
    LCM_CR(0) = 0;
    LCM_CR(0x10) = 1;
    LCM_CR(0x14) = 0xa50100;
    for (i = 0; i < 3; i++) {            /* panel reset pulses */
        REG32(LCD_RST_REG) = (REG32(LCD_RST_REG) & ~1u) | (i & 1 ? 0 : 1);
        ms_wait(32);
    }
    LCM_CR(0x10) = 1;                    /* safe timing for the id read */
    LCM_CR(0x14) = 30 << 16 | 6 | 0x80 | 30 << 21 | 14 << 8 | 6 << 4;
    lcm_send(0, 0x04);
    for (i = 0; i < 4; i++)
        id = id << 8 | (lcm_recv(1) & 0xff);
    id &= 0xffffff;
    lcm_timing();
    lcm_exec(init_boe);
    lcm_exec(init_window);
    lcdc_setup();
    LCDC->img.y_base_addr = FRAME_ADDR >> 2;
    LCDC->img.ctrl |= 1;
    lcd_flush();
    return id;
}

static void frame_wait(void)
{
    if (kicked)
        while (ms_now() - last_kick < frame_ms)
            ;
}

void lcd_flush(void)
{
    lcdc_t *lc = LCDC;

    frame_wait();
    lc->irq.clr |= 1;
    lc->ctrl |= 8;
    last_kick = ms_now();
    kicked = true;
    frame_wait();                        /* drawing may follow at once */
}

/* Before a program starts: the last frame out, the panel keeps showing it. */
void lcd_stop(void)
{
    frame_wait();
}

/* Before the stock firmware (the jump): the display left exactly as the
 * menu runs it, "warm": the panel awake and on, the LCM in its DMA data
 * mode with its bus timing, the LCDC set up, the clocks on; only the last
 * frame goes out. On the phone the stock firmware then ran, lit the
 * backlight and set its brightness, but the panel showed nothing until its
 * own sleep and wake (which sets the whole display path up again):
 *   v1.4 the LCDC/LCM clocks gated; v1.5 clocks on, the LCM switched to
 *   command mode; v1.8 the LCM's power-on registers, the LCDC reset;
 *   v1.9/v1.10 the panel reset, the backlight restored.
 * The stock's normal boot drives a display that is already set up: it
 * sends window commands and kicks the LCDC (QEMU's trace of a cold stock
 * boot), and never sets up the LCM or the panel on that path. */
void lcd_off(void)
{
    frame_wait();
}
