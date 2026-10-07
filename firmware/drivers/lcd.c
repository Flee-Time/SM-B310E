/*
 * B310E-OS — drivers/lcd.c
 *
 * ST7735 128x160 color TFT driver (Sitronix ST7735S TNM/DTC/BOE) for the
 * Samsung SM-B310E. The panel hangs off the SC6530's parallel DBI:
 *
 *   LCM controller @ 0x20800000  — 16-bit DBI command/data window
 *   LCDC controller @ 0x20d00000  — DMA image source -> panel
 *
 * All register facts are replicated from fpdoom's BSP layer (syscode.c,
 * syscode.h, lcd_config.h — Unlicense / public domain, copy freely).
 * Refresh is polled (no interrupts). The D-cache is cleaned before every
 * refresh so the LCDC DMA reads committed framebuffer data (fpdoom
 * sys_start_refresh calls clean_dcache() before the DMA, syscode.c:570);
 * while the D-cache is disabled (ARM926EJ-S with MMU off — see
 * arch/cache.s) the clean is a no-op.
 *
 * SAFETY: this driver writes NO 0x8cxxxxxx pinmux registers (the B310E
 * hangs on 0x8c0002a4=0x231). The LCD uses the LCM/LCDC DBI path only.
 *
 * HOST_TEST: register-touching functions compile to no-ops; the pure
 * logic (font, offset math, clipping) stays testable on the host.
 */

#include "os.h"
#include "../arch/chip.h"
#include "lcd.h"
#include "led.h"

#include <stdint.h>
#include <stddef.h>             /* offsetof (lcdc_t offset locks) */

/* ---- SoC register map (fpdoom, Unlicense) ------------------------------ */

/* LCM controller — parallel DBI bus master @ 0x20800000 (syscode.c:248) */
#define LCM_REG_BASE  0x20800000u
#define LCM_CR(x)     MEM4(LCM_REG_BASE + (x))

/* Panel data window: cs << 26 | 0x60000000; idx << 17 selects
 * cmd(0) / data(1); 16-bit writes (syscode.c:282-285). B310E uses CS0
 * (sys_data.lcd_cs default = 0). */
#define LCM_WIN_CS0   0x60000000u
#define LCM_CS        0u

/* LCD reset pin register — GPIO-style AHB reg, NOT a 0x8c pinmux
 * (syscode.c:295-305). */
#define LCD_RST_REG   0x8b000224u

/* AHB power-on set / soft-reset (syscode.c:77-80, 720-722) */
#define AHB_PWR_ON    0x20500060u
#define AHB_RST_SET   0x20500020u
#define AHB_RST_CLR   0x20500030u

/* 1 ms system timer (syscomm.c:289) */
#define SYS_TIMER_MS  0x8100300cu

/* LCDC display controller @ 0x20d00000 — register map mirror of
 * fpdoom's lcdc_base_t (syscode.h:34-56) with the EXACT same member
 * order and sizes. The layout is offset-critical: img @ 0x20, the three
 * 10-word ocd blocks @ 0x50/0x78/0xa0, cap @ 0xc8, y2r @ 0xe8, irq @
 * 0xf8 (irq.raw = 0x104). A divergence here silently moves every
 * register write, so the offsets are locked by _Static_assert below. */
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

/* Offset lock: every register the LCD path touches must sit at fpdoom's
 * lcdc_base_t offset (syscode.h:34-56). Compile-time, so a layout edit
 * that shifts a used register fails the build instead of the LCD. */
_Static_assert(offsetof(lcdc_t, ctrl) == 0x00, "lcdc_t.ctrl");
_Static_assert(offsetof(lcdc_t, disp_size) == 0x04, "lcdc_t.disp_size");
_Static_assert(offsetof(lcdc_t, lcm_start) == 0x08, "lcdc_t.lcm_start");
_Static_assert(offsetof(lcdc_t, lcm_size) == 0x0c, "lcdc_t.lcm_size");
_Static_assert(offsetof(lcdc_t, bg_color) == 0x10, "lcdc_t.bg_color");
_Static_assert(offsetof(lcdc_t, img.ctrl) == 0x20, "lcdc_t.img.ctrl");
_Static_assert(offsetof(lcdc_t, img.y_base_addr) == 0x24, "lcdc_t.img.y_base_addr");
_Static_assert(offsetof(lcdc_t, img.size_xy) == 0x2c, "lcdc_t.img.size_xy");
_Static_assert(offsetof(lcdc_t, img.pitch) == 0x30, "lcdc_t.img.pitch");
_Static_assert(offsetof(lcdc_t, img.disp_xy) == 0x34, "lcdc_t.img.disp_xy");
_Static_assert(offsetof(lcdc_t, cap.ctrl) == 0xe0, "lcdc_t.cap.ctrl");
_Static_assert(offsetof(lcdc_t, cap.base_addr) == 0xe4, "lcdc_t.cap.base_addr");
_Static_assert(offsetof(lcdc_t, irq.en) == 0x110, "lcdc_t.irq.en");
_Static_assert(offsetof(lcdc_t, irq.clr) == 0x114, "lcdc_t.irq.clr");
_Static_assert(offsetof(lcdc_t, irq.raw) == 0x11c, "lcdc_t.irq.raw");

#define LCDC ((lcdc_t *)0x20d00000u)

/* ---- framebuffer (public) ---------------------------------------------- */
uint16_t lcd_fb[LCD_W * LCD_H];

/* MADCTL override for diag-rot (and any caller that needs a forced panel
 * orientation): 0 = use the normal B310E_MADCTL / mac_arg selection in
 * lcd_init(); non-zero = this exact value wins on the next lcd_init()
 * AND on live lcd_set_madctl() calls. The stock panel is mounted 90° to
 * the datasheet orientation, so the upright value is expected to need
 * MV (bit 5, row/col exchange) — see diag_rot_main.c. */
static uint8_t s_madctl_override;

/* ---- ST7735 init sequences (fpdoom lcd_config.h, Unlicense) ------------ */

#define LCM_CMD(cmd, len) 0x80 | (len), (cmd)
#define LCM_DELAY(ms)     0x40 | (((ms) >> 8) & 0x1f), ((ms) & 0xff)
#define LCM_END           0

/* ST7735 TNM — the B310E's panel (author's log: id 0x5ca1f1).
 * lcd_config.h:1205-1231 */
static const uint8_t cmd5ca1f1_init[] = {
    LCM_CMD(0x11, 0), /* Sleep Out Mode */
    LCM_DELAY(120),
    /* Frame Rate Control 1-3 */
    LCM_CMD(0xb1, 3), 0x0f, 0x04, 0x04,
    LCM_CMD(0xb2, 3), 0x05, 0x3a, 0x3a,
    LCM_CMD(0xb3, 6), 0x05, 0x3a, 0x3a, 0x05, 0x3a, 0x3a,
    LCM_CMD(0xb4, 1), 0x00, /* Display Inversion Control */
    /* Power Control 1-5 */
    LCM_CMD(0xc0, 3), 0xc3, 0x06, 0x44,
    LCM_CMD(0xc1, 1), 0xc2,
    LCM_CMD(0xc2, 2), 0x0d, 0x00,
    LCM_CMD(0xc3, 2), 0xdb, 0x2a,
    LCM_CMD(0xc4, 2), 0x8b, 0xee,
    LCM_CMD(0xc5, 1), 0x0d, /* VCOM Control 1 */
    LCM_CMD(0x3a, 1), 0x05, /* Pixel Format Set (RGB565) */
    /* Set Gamma 1 */
    LCM_CMD(0xe0, 16), 0x13, 0x14, 0x06, 0x11,
        0x29, 0x26, 0x21, 0x26, 0x25, 0x26, 0x2e, 0x3b,
        0x00, 0x03, 0x02, 0x06,
    /* Set Gamma 2 */
    LCM_CMD(0xe1, 16), 0x02, 0x25, 0x06, 0x11,
        0x29, 0x26, 0x21, 0x26, 0x26, 0x26, 0x2e, 0x3b,
        0x00, 0x03, 0x02, 0x06,
    LCM_CMD(0x29, 0), /* Display ON */
    LCM_END
};

/* ST7735 DTC — fallback (id 0x5cc0f1). lcd_config.h:1180-1203 */
static const uint8_t cmd5cc0f1_init[] = {
    LCM_CMD(0x11, 0), /* Sleep Out Mode */
    LCM_DELAY(120),
    /* Frame Rate Control 1 */
    LCM_CMD(0xb1, 3), 0x04, 0x10, 0x10,
    LCM_CMD(0xb4, 1), 0x03, /* Display Inversion Control */
    LCM_CMD(0xb6, 2), 0x17, 0x00, /* Display Function Setting */
    /* Power Control 1-3 */
    LCM_CMD(0xc0, 3), 0xa4, 0x04, 0x04,
    LCM_CMD(0xc1, 1), 0xc0,
    LCM_CMD(0xc2, 2), 0x0a, 0x00,
    LCM_CMD(0xc5, 1), 0x0f, /* VCOM Control 1 */
    LCM_CMD(0x3a, 1), 0x05, /* Pixel Format Set (RGB565) */
    /* Set Gamma 1 */
    LCM_CMD(0xe0, 16), 0x02, 0x1f, 0x0b, 0x12,
        0x36, 0x33, 0x2d, 0x31, 0x2f, 0x2c, 0x33, 0x3b,
        0x00, 0x02, 0x01, 0x02,
    /* Set Gamma 2 */
    LCM_CMD(0xe1, 16), 0x02, 0x1f, 0x0b, 0x12,
        0x36, 0x32, 0x2d, 0x30, 0x2f, 0x2c, 0x33, 0x3b,
        0x00, 0x01, 0x00, 0x02,
    LCM_CMD(0x29, 0), /* Display ON */
    LCM_END
};

/* Sitronix ST7735S BOE — this unit's panel (id 0x7c89f0), verified on
 * hardware via fpdoom's fptest. lcd_config.h:917-948 (verbatim). */
static const uint8_t cmd7c89f0_init[] = {
    LCM_CMD(0x11, 0), /* Sleep Out Mode */
    LCM_DELAY(120),
    /* Frame Rate Control 1-3 */
    LCM_CMD(0xb1, 3), 0x05, 0x3c, 0x3c,
    LCM_CMD(0xb2, 3), 0x05, 0x3c, 0x3c,
    LCM_CMD(0xb3, 6), 0x05, 0x3c, 0x3c, 0x05, 0x3c, 0x3c,
    LCM_CMD(0xb4, 2), 0x03, 0x02, /* Display Inversion Control */
    /* Power Control 1-5 */
    LCM_CMD(0xc0, 3), 0xa4, 0x04, 0x84,
    LCM_CMD(0xc1, 1), 0xc4,
    LCM_CMD(0xc2, 2), 0x0d, 0x00,
    LCM_CMD(0xc3, 2), 0x8d, 0x2a,
    LCM_CMD(0xc4, 2), 0x8d, 0xee,
    LCM_CMD(0xc5, 1), 0x04, /* VCOM Control 1 */
    /* Set Gamma 1 */
    LCM_CMD(0xe0, 16), 0x05, 0x19, 0x14, 0x17,
        0x3d, 0x38, 0x2e, 0x2f, 0x2d, 0x29, 0x31, 0x3b,
        0x00, 0x03, 0x00, 0x10,
    /* Set Gamma 2 */
    LCM_CMD(0xe1, 16), 0x04, 0x15, 0x0e, 0x10,
        0x31, 0x2d, 0x29, 0x2d, 0x2b, 0x28, 0x2e, 0x39,
        0x00, 0x01, 0x02, 0x10,
    LCM_CMD(0x35, 1), 0x00, /* Tearing Effect Line ON */
    LCM_CMD(0x3a, 1), 0x05, /* Pixel Format Set (RGB565) */
    LCM_CMD(0x21, 0), /* Display Inversion ON */
    LCM_CMD(0x29, 0), /* Display ON */
    LCM_END
};

/* Memory Access Control + window + Memory Write (fpdoom cmd_init2,
 * syscode.c:689-703). MADCTL byte [2] is patched per panel in lcd_init():
 * 0x00 TNM / 0xc8 DTC / 0xd0 BOE (mount orientation). Lives in .data
 * (writable). */
static uint8_t cmd_init2[] = {
    LCM_CMD(0x36, 1), 0x00,          /* MADCTL (patched in lcd_init) */
    LCM_CMD(0x2a, 4), 0x00, 0x00, 0x00, (uint8_t)(LCD_W - 1),
    LCM_CMD(0x2b, 4), 0x00, 0x00, 0x00, (uint8_t)(LCD_H - 1),
    LCM_CMD(0x2c, 0),                /* Memory Write */
    LCM_END
};

/* ---- 8x16 console font: classic public-domain 5x7 bitmap rendered at
 * 2x vertical scale in an 8x16 cell (95 chars, 0x20-0x7e). Each char is
 * 5 bytes = 5 columns; bit 0 = top row, bit 6 = bottom row. ------------ */
static const uint8_t font5x7[95][5] = {
    {0x00,0x00,0x00,0x00,0x00}, /* space */
    {0x00,0x00,0x5f,0x00,0x00}, /* ! */
    {0x00,0x07,0x00,0x07,0x00}, /* " */
    {0x14,0x7f,0x14,0x7f,0x14}, /* # */
    {0x24,0x2a,0x7f,0x2a,0x12}, /* $ */
    {0x23,0x13,0x08,0x64,0x62}, /* % */
    {0x36,0x49,0x55,0x22,0x50}, /* & */
    {0x00,0x05,0x03,0x00,0x00}, /* ' */
    {0x00,0x1c,0x22,0x41,0x00}, /* ( */
    {0x00,0x41,0x22,0x1c,0x00}, /* ) */
    {0x08,0x2a,0x1c,0x2a,0x08}, /* * */
    {0x08,0x08,0x3e,0x08,0x08}, /* + */
    {0x00,0x50,0x30,0x00,0x00}, /* , */
    {0x08,0x08,0x08,0x08,0x08}, /* - */
    {0x00,0x60,0x60,0x00,0x00}, /* . */
    {0x20,0x10,0x08,0x04,0x02}, /* / */
    {0x3e,0x51,0x49,0x45,0x3e}, /* 0 */
    {0x00,0x42,0x7f,0x40,0x00}, /* 1 */
    {0x42,0x61,0x51,0x49,0x46}, /* 2 */
    {0x21,0x41,0x45,0x4b,0x31}, /* 3 */
    {0x18,0x14,0x12,0x7f,0x10}, /* 4 */
    {0x27,0x45,0x45,0x45,0x39}, /* 5 */
    {0x3c,0x4a,0x49,0x49,0x30}, /* 6 */
    {0x01,0x71,0x09,0x05,0x03}, /* 7 */
    {0x36,0x49,0x49,0x49,0x36}, /* 8 */
    {0x06,0x49,0x49,0x29,0x1e}, /* 9 */
    {0x00,0x36,0x36,0x00,0x00}, /* : */
    {0x00,0x56,0x36,0x00,0x00}, /* ; */
    {0x00,0x08,0x14,0x22,0x41}, /* < */
    {0x14,0x14,0x14,0x14,0x14}, /* = */
    {0x41,0x22,0x14,0x08,0x00}, /* > */
    {0x02,0x01,0x51,0x09,0x06}, /* ? */
    {0x32,0x49,0x79,0x41,0x3e}, /* @ */
    {0x7e,0x11,0x11,0x11,0x7e}, /* A */
    {0x7f,0x49,0x49,0x49,0x36}, /* B */
    {0x3e,0x41,0x41,0x41,0x22}, /* C */
    {0x7f,0x41,0x41,0x22,0x1c}, /* D */
    {0x7f,0x49,0x49,0x49,0x41}, /* E */
    {0x7f,0x09,0x09,0x01,0x01}, /* F */
    {0x3e,0x41,0x41,0x51,0x32}, /* G */
    {0x7f,0x08,0x08,0x08,0x7f}, /* H */
    {0x00,0x41,0x7f,0x41,0x00}, /* I */
    {0x20,0x40,0x41,0x3f,0x01}, /* J */
    {0x7f,0x08,0x14,0x22,0x41}, /* K */
    {0x7f,0x40,0x40,0x40,0x40}, /* L */
    {0x7f,0x02,0x04,0x02,0x7f}, /* M */
    {0x7f,0x04,0x08,0x10,0x7f}, /* N */
    {0x3e,0x41,0x41,0x41,0x3e}, /* O */
    {0x7f,0x09,0x09,0x09,0x06}, /* P */
    {0x3e,0x41,0x51,0x21,0x5e}, /* Q */
    {0x7f,0x09,0x19,0x29,0x46}, /* R */
    {0x46,0x49,0x49,0x49,0x31}, /* S */
    {0x01,0x01,0x7f,0x01,0x01}, /* T */
    {0x3f,0x40,0x40,0x40,0x3f}, /* U */
    {0x1f,0x20,0x40,0x20,0x1f}, /* V */
    {0x7f,0x20,0x18,0x20,0x7f}, /* W */
    {0x63,0x14,0x08,0x14,0x63}, /* X */
    {0x03,0x04,0x78,0x04,0x03}, /* Y */
    {0x61,0x51,0x49,0x45,0x43}, /* Z */
    {0x00,0x00,0x7f,0x41,0x41}, /* [ */
    {0x02,0x04,0x08,0x10,0x20}, /* \ */
    {0x41,0x41,0x7f,0x00,0x00}, /* ] */
    {0x04,0x02,0x01,0x02,0x04}, /* ^ */
    {0x40,0x40,0x40,0x40,0x40}, /* _ */
    {0x00,0x01,0x02,0x04,0x00}, /* ` */
    {0x20,0x54,0x54,0x54,0x78}, /* a */
    {0x7f,0x48,0x44,0x44,0x38}, /* b */
    {0x38,0x44,0x44,0x44,0x20}, /* c */
    {0x38,0x44,0x44,0x48,0x7f}, /* d */
    {0x38,0x54,0x54,0x54,0x18}, /* e */
    {0x08,0x7e,0x09,0x01,0x02}, /* f */
    {0x0c,0x52,0x52,0x52,0x3e}, /* g */
    {0x7f,0x08,0x04,0x04,0x78}, /* h */
    {0x00,0x44,0x7d,0x40,0x00}, /* i */
    {0x20,0x40,0x44,0x3d,0x00}, /* j */
    {0x7f,0x10,0x28,0x44,0x00}, /* k */
    {0x00,0x41,0x7f,0x40,0x00}, /* l */
    {0x7c,0x04,0x18,0x04,0x78}, /* m */
    {0x7c,0x08,0x04,0x04,0x78}, /* n */
    {0x38,0x44,0x44,0x44,0x38}, /* o */
    {0x7c,0x14,0x14,0x14,0x08}, /* p */
    {0x08,0x14,0x14,0x18,0x7c}, /* q */
    {0x7c,0x08,0x04,0x04,0x08}, /* r */
    {0x48,0x54,0x54,0x54,0x20}, /* s */
    {0x04,0x3f,0x44,0x40,0x20}, /* t */
    {0x3c,0x40,0x40,0x20,0x7c}, /* u */
    {0x1c,0x20,0x40,0x20,0x1c}, /* v */
    {0x3c,0x40,0x30,0x40,0x3c}, /* w */
    {0x44,0x28,0x10,0x28,0x44}, /* x */
    {0x0c,0x50,0x50,0x50,0x3c}, /* y */
    {0x44,0x64,0x54,0x4c,0x44}, /* z */
    {0x00,0x08,0x36,0x41,0x00}, /* { */
    {0x00,0x00,0x7f,0x00,0x00}, /* | */
    {0x00,0x41,0x36,0x08,0x00}, /* } */
    {0x08,0x1c,0x2a,0x08,0x08}, /* ~ */
};

/* ---- low-level register access ----------------------------------------- */

/* DBI busy-wait with a hard budget (a wedged LCM controller must not hang
 * the cooperative scheduler — the refresh path is polled during task
 * execution). The waits complete in nanoseconds on healthy hardware. */
#define LCM_BUSY_BUDGET 1000000u

static void lcm_wait_idle(void)
{
#if !defined(HOST_TEST)
    uint32_t n = LCM_BUSY_BUDGET;

    while (LCM_CR(0) & 2)
        if (--n == 0) break;
#endif
}

/* 16-bit DBI write: idx 0 = command, 1 = data (syscode.c:282-285). */
static void lcm_send(unsigned idx, unsigned val)
{
#if !defined(HOST_TEST)
    lcm_wait_idle();
    MEM2(LCM_WIN_CS0 | (idx << 17)) = (uint16_t)val;
#else
    (void)idx;
    (void)val;
#endif
}

/* 16-bit DBI read (ID fetch, syscode.c:287-290). */
static uint16_t lcm_recv(unsigned idx)
{
#if !defined(HOST_TEST)
    lcm_wait_idle();
    return MEM2(LCM_WIN_CS0 | (idx << 17));
#else
    (void)idx;
    return 0;
#endif
}

static inline void lcm_send_cmd(uint16_t cmd) { lcm_send(0, cmd); }
static inline void lcm_send_data(uint16_t val) { lcm_send(1, val); }

/* DBI mode register (fpdoom lcm_set_mode, syscode.c:276-280, verbatim):
 * busy-wait the DBI, then write the mode to LCM_CR(0x10 + cs<<4). B310E
 * uses chip select 0, so the cs<<4 term vanishes. */
static void lcm_set_mode(uint32_t val)
{
#if !defined(HOST_TEST)
    lcm_wait_idle();
    LCM_CR(0x10 + (LCM_CS << 4)) = val;
#else
    (void)val;
#endif
}

/* ---- DBI clock timing (fpdoom syscode.c:307-370, 376-392, Unlicense) --- */

/* Panel descriptor (fpdoom lcd_config_t, syscode.h:251-265 — the fields
 * lcm_set_freq needs). Entries mirror lcd_config.h:2366/2253/2368. */
typedef struct {
    uint32_t id;
    uint16_t width, height;
    uint16_t mac_arg;
    struct {                    /* ns */
        uint8_t rcss, rlpw, rhpw, wcss, wlpw, whpw;
    } mcu_timing;
    struct { uint32_t freq; } spi;
    const uint8_t *cmd_init;
} lcd_config_t;

/* The three ST7735 variants this B310E can carry. BOE (id 0x7c89f0, this
 * unit) has all mcu_timing 150 ns; TNM/DTC 15/45/90/5/15/40 ns (lcd_config.h
 * 2253/2366/2368 — id_mask is ~0, so exact 24-bit id match). */
static const lcd_config_t lcd_config[] = {
    { 0x5cc0f1, 128, 160, 0xc8, { 15, 45, 90, 5, 15, 40 }, { 0 }, cmd5cc0f1_init },
    { 0x7c89f0, 128, 160, 0xd0, { 150, 150, 150, 150, 150, 150 }, { 0 }, cmd7c89f0_init },
    { 0x5ca1f1, 128, 160, 0x00, { 15, 45, 90, 5, 15, 40 }, { 0 }, cmd5ca1f1_init },
};

/* fpdoom lcd_find_conf, mode 0 (mcu path, syscode.c:401-418): exact id
 * match + mcu timing present. Unknown id falls back to the TNM entry. */
static const lcd_config_t *lcd_find_conf(uint32_t id)
{
    unsigned i;

    for (i = 0; i < sizeof(lcd_config) / sizeof(*lcd_config); i++)
        if (id == lcd_config[i].id && lcd_config[i].mcu_timing.rcss)
            return &lcd_config[i];
    return &lcd_config[2];
}

/* CPU core frequency (fpdoom get_cpu_freq, syscode.c:307-321, SC6530 path
 * verbatim). The PLL selector at 0x8b000040 [1:0] indexes a byte table of
 * MHz values; sc6530_init_freq sets selector 1 -> 208 MHz. */
static uint32_t lcd_get_cpu_freq(void)
{
#if !defined(HOST_TEST)
#define X(a) (a / 26)
#define X2(x, a, b, c, d) uint32_t x = \
    X(a) | X(b) << 8 | X(c) << 16 | X(d) << 24;
    X2(t1, 260, 1040, 2080, 2080)
    X2(t2, 260, 2080, 2496, 3120)
    X2(t3, 260, 2080, 1040, 1560)
#undef X2
#undef X
    uint32_t t = t1, addr = 0x8b00004c;

    (void)t2;                   /* only the other chip paths use t2 */
    t = t3; addr -= 0xc;        /* SC6530 path only (this port) */
    return (t >> (MEM4(addr) & 3) * 8 & 0xff) * 2600000;
#else
    return 0;
#endif
}

/* AHB clock (fpdoom get_ahb_freq, syscode.c:323-331, SC6530 path verbatim).
 * On this phone: 208 MHz >> 1 = 104 MHz (sc6530_init_freq sets bit 2 of
 * 0x8b000040; fptest log "LCD: clk_rate = 104000000"). */
static uint32_t lcd_get_ahb_freq(void)
{
#if !defined(HOST_TEST)
    uint32_t freq = lcd_get_cpu_freq();
    if (MEM4(0x8b000040) & 1 << 2) freq >>= 1;
    return freq;
#else
    return 0;
#endif
}

/* Worst-case DBI timing, clock-independent (fpdoom lcm_set_safe_freq,
 * syscode.c:334-340, verbatim): LCM_CR(0x10) = 1 and
 * LCM_CR(0x14) = 30<<21|30<<16|14<<8|6<<4|6|0x80 = 0x03de0ee6. Pre-clock
 * baseline written before the panel id read. */
static void lcm_set_safe_freq(void)
{
#if !defined(HOST_TEST)
    uint32_t addr = LCM_REG_BASE + 0x10 + (LCM_CS << 4);
    uint32_t sum = 30 << 16 | 6 | 0x80;

    sum |= 30 << 21 | 14 << 8 | 6 << 4;
    MEM4(addr) = 1;                         /* lcm_set_mode(1) */
    MEM4(addr + 4) = sum;
#else
    (void)0;
#endif
}

/* REAL DBI clock timing (fpdoom lcm_set_freq, syscode.c:342-370,
 * verbatim). Computes per-phase cycle counts from the AHB clock and the
 * panel's mcu_timing; for the BOE at 104 MHz this writes
 * LCM_CR(0x14) = 0x037e0ee6. Without it the LCDC DMA refresh never
 * completes and the panel shows its own static GRAM. */
static void lcm_set_freq(uint32_t clk_rate, const lcd_config_t *lcd)
{
#if !defined(HOST_TEST)
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
#else
    (void)clk_rate;
    (void)lcd;
#endif
}

/* 16-bit DBI read-back (fpdoom lcd_cmdret/lcd_getid, syscode.c:376-392,
 * verbatim). lcd_getid = 0x04 RDDID, 4 reads, mask 24. */
static uint32_t lcd_cmdret(int cmd, unsigned n)
{
    uint32_t ret = 0;

    lcm_send_cmd(cmd);
    do ret = ret << 8 | (lcm_recv(1) & 0xff); while (--n);
    return ret;
}

static uint32_t lcd_getid(void)
{
    return lcd_cmdret(0x04, 4) & 0xffffff;
}

/* Execute a packed init table (syscode.c:423-442). */
static void lcm_exec(const uint8_t *p)
{
    for (;;) {
        uint32_t a = *p++, len;

        if (!a) break;
        len = a & 0x1f;
        a >>= 5;
        if (a == 4) {                       /* command byte follows */
            lcm_send_cmd(*p++);
            a = 0;
        }
        if (a == 0) {                       /* data bytes */
            while (len--) lcm_send_data(*p++);
        } else if (a == 2) {                /* delay: len<<8 | next byte */
            lcd_delay_ms((len << 8) | *p++);
        }
    }
}

/* 1 ms system timer (syscomm.c:289-293). */
uint32_t lcd_sys_timer_ms(void)
{
#if !defined(HOST_TEST)
    uint32_t a, b = MEM4(SYS_TIMER_MS);

    do a = b, b = MEM4(SYS_TIMER_MS); while (a != b);
    return a;
#else
    return 0;
#endif
}

void lcd_delay_ms(uint32_t ms)
{
#if !defined(HOST_TEST)
    uint32_t start = lcd_sys_timer_ms();

    while (lcd_sys_timer_ms() - start < ms) ;
#else
    (void)ms;
#endif
}

/* Panel reset: fpdoom's lcm_reset (syscode.c:295-305), method=0, delay=32.
 * Bit 0 of 0x8b000224 goes 1,0,1 — the ACTIVE-LOW panel RESX ends
 * DEASSERTED. (The old 0,1,0 sequence left the panel held in reset and was
 * a cause of the black screen.) */
static void lcd_reset(void)
{
#if !defined(HOST_TEST)
    uint32_t addr = LCD_RST_REG, t;
    int i, method = 0;

    method &= 1;
    for (i = 0; i < 3; i++) {
        t = MEM4(addr) & ~(uint32_t)method;
        t |= (uint32_t)(method ^= 1);
        MEM4(addr) = t;
        lcd_delay_ms(32);
    }
#else
    /* no-op on host */
#endif
}

/* LCDC engine setup (fpdoom lcdc_init, syscode.c:707-776, verbatim order;
 * full-panel 128x160, no scaling). */
static void lcdc_init_regs(void)
{
#if !defined(HOST_TEST)
    lcdc_t *lcdc = LCDC;
    unsigned w = LCD_W, h = LCD_H;
    unsigned w2 = LCD_W, h2 = LCD_H;

    MEM4(AHB_PWR_ON) = 0x1000;              /* LCDC enable (syscode.c:718) */

    /* AHB soft reset bit 9 (syscode.c:720-722) */
    MEM4(AHB_RST_SET) = 0x200;
    lcd_delay_ms(10);
    MEM4(AHB_RST_CLR) = 0x200;

    lcdc->ctrl |= 1;                        /* lcdc_enable = 1 */
    lcdc->ctrl |= 2;                        /* fmark_mode = 1 */
    lcdc->ctrl &= ~4u;                      /* fmark_pol = 0 */
    lcdc->bg_color = 0x000000;

    lcdc->disp_size = w | h << 16;
    lcdc->lcm_start = 0;
    lcdc->lcm_size = w | h << 16;

    /* DBI sink (syscode.c:739-750): 8x2 BE mode, DMA writes the CS0 data
     * window at 0x60020000. */
    lcm_set_mode(0x28);
    lcdc->cap.ctrl |= 0x20;
    lcdc->cap.ctrl |= (lcdc->cap.ctrl & ~(3u << 6)) | 2u << 6;
    lcdc->ctrl &= ~(7u << 5);
    lcdc->cap.base_addr = (LCM_WIN_CS0 | 1u << 17) >> 2;

    /* one dummy refresh to clock the panel pipeline (syscode.c:752-753:
     * sys_start_refresh + sys_wait_refresh). page_reset == 0 (ST7735S BOE)
     * means PURE DMA: no lcm_set_mode(1)/0x2c handshake — the panel is
     * already in memory-write from cmd_init2's 0x2c and auto-resets its
     * GRAM page counter per full-frame DMA write. Bounded wait (robustness,
     * per task: scheduler must never freeze). */
    {
        uint32_t n = 100000000u;

        lcdc->irq.en |= 1;
        lcdc->ctrl |= 8;
        while (!(lcdc->irq.raw & 1) && --n) ;
        lcdc->irq.clr |= 1;
    }
    lcdc->irq.en &= ~1u;

    /* image source config (syscode.c:756-775) */
    {
        uint32_t a = lcdc->img.ctrl;
        int fmt = 5;

        a &= ~2u;                           /* disable color key */
        a = (a & ~(15u << 4)) | fmt << 4;   /* RGB565 */
        a = (a & ~(3u << 8)) | 2u << 8;     /* little endian */
        lcdc->img.ctrl = a;
    }

    lcdc->img.pitch = w2;
    w2 = w2 > w ? w : w2;
    h2 = h2 > h ? h : h2;
    w2 &= ~1u;                              /* must be aligned */
    lcdc->img.size_xy = w2 | h2 << 16;
    lcdc->img.disp_xy = ((w - w2) >> 1) | ((h - h2) >> 1) << 16;
#else
    (void)0;
#endif
}

/* ---- public API --------------------------------------------------------- */

int lcd_init(void)
{
#if !defined(HOST_TEST)
    uint32_t id, clk_rate;
    const lcd_config_t *lcd;

    /* fpdoom lcm_init (syscode.c:507-551): LCM power, panel reset, safe
     * DBI baseline, id read, then the REAL DBI timing from the AHB clock. */
    MEM4(AHB_PWR_ON) = 0x40;                /* LCM enable (syscode.c:507) */
    LCM_CR(0) = 0;
    LCM_CR(0x10) = 1;
    LCM_CR(0x14) = 0xa50100;
    lcd_reset();                            /* lcm_reset(32, 0) */
    clk_rate = lcd_get_ahb_freq();
    lcm_set_safe_freq();                    /* LCM_CR(0x10)=1, LCM_CR(0x14)=0x03de0ee6 */
    /* lcm_config_addr(0): data window = LCM_WIN_CS0 (no register write) */
    id = lcd_getid();                       /* RDDID 0x04, 4 reads & 0xffffff */
    lcd = lcd_find_conf(id);                /* BOE 0x7c89f0 on this unit */
    lcm_set_freq(clk_rate, lcd);            /* LCM_CR(0)=0x11110000, LCM_CR(0x14)=0x037e0ee6 */

    /* fpdoom lcd_init (syscode.c:586-705): panel init table + window/0x2c.
     * cmd_init2 (MADCTL + CASET + PASET + 0x2c Memory Write) is the ONLY
     * place the 0x2c handshake appears — never in lcd_show. */
    lcm_exec(lcd->cmd_init);
    /* MADCTL: panel mount orientation. The BOE (id 0x7c89f0) datasheet
     * mac_arg is 0xd0, but the B310E panel is mounted 90° rotated to the
     * datasheet scan (user report: "top of the LCD is always at the left
     * side, bottom at the right") — so the upright value needs MV (bit 5,
     * row/col exchange). s_madctl_override (set by diag-rot) wins when
     * non-zero; otherwise the BOE uses B310E_MADCTL, DTC 0xc8 / TNM 0x00. */
    if (s_madctl_override != 0)
        cmd_init2[2] = s_madctl_override;
    else if (id == 0x7c89f0)
        cmd_init2[2] = B310E_MADCTL;   /* BOE: mount orientation */
    else
        cmd_init2[2] = (uint8_t)lcd->mac_arg;   /* DTC 0xc8 / TNM 0x00 */
    lcm_exec(cmd_init2);

    /* fpdoom lcd_appinit (fptest/main.c:20-25): w2 = w = 128, h2 = h = 160
     * (full panel, offset 0 in sys_framebuffer below). */

    /* fpdoom lcdc_init (syscode.c:707-776) */
    lcdc_init_regs();

    /* fpdoom sys_framebuffer (syscode.c:778-824, offset 0 since w2 == w) */
    LCDC->img.y_base_addr = (uint32_t)lcd_fb >> 2;
    LCDC->img.ctrl |= 1;                    /* img enable */

    /* deterministic first frame, then backlight (fpdoom sys_start) */
    lcd_fill(0x0000);
    lcd_show();
    led_backlight_set(100);

    return 0;
#else
    lcd_fill(0x0000);
    return 0;
#endif
}

void lcd_fill(uint16_t color)
{
    uint16_t *p = lcd_fb;
    uint16_t *const end = lcd_fb + LCD_W * LCD_H;

    while (p < end) *p++ = color;
}

void lcd_putc(int x, int y, char c, uint16_t fg, uint16_t bg)
{
    const uint8_t *g;
    int col, row, xx, yy;

    if (c < 0x20 || c > 0x7e) c = '?';
    g = font5x7[c - 0x20];

    /* background: the full 8x16 cell */
    for (row = 0; row < 16; row++) {
        yy = y + row;
        if (yy < 0 || yy >= (int)LCD_H) continue;
        for (col = 0; col < 8; col++) {
            xx = x + col;
            if (xx < 0 || xx >= (int)LCD_W) continue;
            lcd_fb[yy * (int)LCD_W + xx] = bg;
        }
    }

    /* glyph: 5x7 bitmap, 2x vertical scale, 1 px padding.
     * Font data is bit 0 = TOP row (see the font table comment). The old
     * `bit = 6 - (row >> 1)` read the byte upside down, so every glyph
     * rendered VERTICALLY MIRRORED in the framebuffer — the "text is
     * mirrored" report that MADCTL changes were chasing. */
    for (row = 0; row < 14; row++) {
        int bit = row >> 1;

        yy = y + 1 + row;
        if (yy < 0 || yy >= (int)LCD_H) continue;
        for (col = 0; col < 5; col++) {
            xx = x + 1 + col;
            if (xx < 0 || xx >= (int)LCD_W) continue;
            if (g[col] & (1u << bit))
                lcd_fb[yy * (int)LCD_W + xx] = fg;
        }
    }
}

void lcd_print(int x, int y, const char *s, uint16_t fg, uint16_t bg)
{
    int x0 = x;

    while (*s) {
        if (*s == '\n') {
            x = x0;
            y += 16;
        } else {
            lcd_putc(x, y, *s, fg, bg);
            x += 8;
        }
        s++;
    }
}

/* Compact renderer: the same 5x7 glyphs at NATIVE size (no 2x vertical
 * scale) in a 6x8 cell — 21 cols x 20 rows on the 128x160 panel. Used for
 * dense readouts (the 16-channel ADC sweep). Font data bit 0 = top row. */
void lcd_putc_small(int x, int y, char c, uint16_t fg, uint16_t bg)
{
    const uint8_t *g;
    int col, row, xx, yy;

    if (c < 0x20 || c > 0x7e) c = '?';
    g = font5x7[c - 0x20];

    for (row = 0; row < 8; row++) {
        yy = y + row;
        if (yy < 0 || yy >= (int)LCD_H) continue;
        for (col = 0; col < 6; col++) {
            xx = x + col;
            if (xx < 0 || xx >= (int)LCD_W) continue;
            lcd_fb[yy * (int)LCD_W + xx] = bg;
        }
    }
    for (row = 0; row < 7; row++) {
        yy = y + row;
        if (yy < 0 || yy >= (int)LCD_H) continue;
        for (col = 0; col < 5; col++) {
            xx = x + col;
            if (xx < 0 || xx >= (int)LCD_W) continue;
            if (g[col] & (1u << row))
                lcd_fb[yy * (int)LCD_W + xx] = fg;
        }
    }
}

void lcd_print_small(int x, int y, const char *s, uint16_t fg, uint16_t bg)
{
    int x0 = x;

    while (*s) {
        if (*s == '\n') {
            x = x0;
            y += 8;
        } else {
            lcd_putc_small(x, y, *s, fg, bg);
            x += 6;
        }
        s++;
    }
}

/* Dense console glyph: the 5x7 bitmap in a 6x9 cell — 1 px right/bottom
 * padding between chars and rows for readability (21 cols x ~17 rows on
 * the 128x160 panel). Clears the FULL cell (fg + bg) so partial redraws
 * never leave ghost pixels from a previous glyph. Font data bit 0 = top. */
void lcd_putc_console(int x, int y, char c, uint16_t fg, uint16_t bg)
{
    const uint8_t *g;
    int col, row;

    if (c < 0x20 || c > 0x7e) c = '?';
    g = font5x7[c - 0x20];

    for (row = 0; row < 9; row++) {
        int yy = y + row;
        if (yy < 0 || yy >= (int)LCD_H) continue;
        for (col = 0; col < 6; col++) {
            int xx = x + col;
            if (xx < 0 || xx >= (int)LCD_W) continue;
            lcd_fb[yy * (int)LCD_W + xx] =
                (row < 7 && col < 5 && (g[col] & (1u << row))) ? fg : bg;
        }
    }
}

void lcd_show(void)
{
#if !defined(HOST_TEST)
    lcdc_t *lc = LCDC;
    uint32_t n = 100000000u;                /* ~0.5 s @ 208 MHz */

    /* DBI panels with page_reset == 0 (our ST7735S BOE, id 0x7c89f0)
     * auto-reset their GRAM page counter on a full-frame DMA write, so the
     * CASET/PASET window + 0x2c Memory Write are sent ONCE at init
     * (cmd_init2) — NOT on every refresh. fpdoom sys_start_refresh
     * (syscode.c:553-564) skips the lcm_set_mode(1) -> 0x2c ->
     * lcm_set_mode(0x28) handshake for these panels; re-sending it per
     * refresh wedges the LCM controller between the handshake and the
     * LCDC DMA, and refresh #2+ never completes. This is pure DMA. */
    /* Clean the D-cache before the LCDC DMA starts the refresh: the CPU
     * writes lcd_fb into cache lines while the DMA reads SDRAM directly —
     * without this the DMA can stream stale pixels (tearing/garbage).
     * Mirrors fpdoom sys_start_refresh (syscode.c:570); a no-op while the
     * D-cache is disabled. */
    clean_dcache();
    lc->irq.en |= 1;                        /* sys_start_refresh */
    lc->ctrl |= 8;                          /* start refresh */

    /* Bounded sys_wait_refresh: a stalled refresh must never freeze the
     * cooperative scheduler (deliberately more robust than fpdoom, which
     * spins forever). On expiry log once and return anyway; the panel
     * keeps its last streamed frame and the demo tasks keep running. */
    while (!(lc->irq.raw & 1) && --n) ;     /* bounded sys_wait_refresh */
    lc->irq.clr |= 1;
    if (!n) kprintf("lcd: refresh timeout\n");
#else
    /* no-op: host tests only exercise framebuffer logic */
#endif
}

/* Bounded-refresh variant of lcd_show() for the LCD diagnostic image
 * (os-diag-lcd.bin). SAME page_reset==0 pure-DMA refresh as lcd_show()
 * (no per-refresh 0x2c handshake) and the identical irq.en/ctrl/clr
 * sequence; ONLY the completion wait differs: it times out after ~1 s
 * instead of spinning forever. If a refresh does not complete, we clear
 * the irq and return anyway and the panel stays on whatever frame was
 * last streamed (this is exactly the failure mode the diagnostic is built
 * to expose). */
void lcd_show_bounded(void)
{
#if !defined(HOST_TEST)
    lcdc_t *lc = LCDC;
    uint32_t n = 20000000u;                 /* ~1 s @ 208 MHz */

    clean_dcache();                         /* DMA coherency (see lcd_show) */
    lc->irq.en |= 1;                        /* sys_start_refresh */
    lc->ctrl |= 8;                          /* start refresh */
    while (!(lc->irq.raw & 1) && --n) ;     /* bounded sys_wait_refresh */
    lc->irq.clr |= 1;
#else
    /* no-op: host tests only exercise framebuffer logic */
#endif
}

/* Rotation diagnostic helper (os-diag-rot.bin): change the panel's
 * MADCTL (Memory Access Control, ST7735 cmd 0x36) LIVE while the LCDC
 * DMA is streaming. Sends ONLY the single 0x36 command + one data byte —
 * the CASET/PASET window and the 0x2c Memory Write were set once at init
 * (cmd_init2) and MUST NOT be re-sent mid-stream: re-running the whole
 * table collapses the panel GRAM window to a tiny region, so only the
 * first pixel of each refresh lands (the old full-table re-send showed
 * one blue pixel). cmd_init2[2] is kept in sync so any later re-init
 * (lcd_init) uses the latest value. The diag main cycles candidate
 * values and paints the labeled screen; the user reports which label is
 * upright. */
void lcd_set_madctl(uint8_t v)
{
#if !defined(HOST_TEST)
    s_madctl_override = v;       /* force this value on the next lcd_init() */
    lcm_send_cmd(0x36);          /* Memory Access Control */
    lcm_send_data(v);
    /* Any command after cmd_init2's 0x2c ends the panel's memory-write
     * stream — re-arm it so the next LCDC DMA refresh actually lands.
     * Without this, diag-rot's first MADCTL switch leaves the panel
     * waiting for commands while the DMA streams pixels -> black screen. */
    lcm_send_cmd(0x2c);          /* Memory Write (re-arm) */
    cmd_init2[2] = v;            /* keep in sync for any re-init */
#else
    (void)v;
#endif
}

/* ---- kernel module registration (wave 8 runs it via module_init_all) --- */
const module_t lcd_module = { "lcd", lcd_init };
