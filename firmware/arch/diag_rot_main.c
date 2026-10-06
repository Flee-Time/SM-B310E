/*
 * B310E-OS — arch/diag_rot_main.c
 *
 * ROTATION DIAGNOSTIC boot image (`make diag-rot` -> os-diag-rot.bin).
 * Cycles candidate ST7735 MADCTL values on the live panel so the user can
 * pick the upright orientation in ONE run.
 *
 * Why: the BOE panel (id 0x7c89f0) is mounted so the datasheet mac_arg
 * 0xd0 does NOT render upright. User report (2026-08-21): "top of the
 * LCD is always at the left side, bottom at the right" — the panel is
 * mounted 90° ROTATED, so the fix needs MV (MADCTL bit 5 = row/col
 * exchange). The old {0x10, 0x90, 0x50, 0xd0} set never contained MV —
 * "changing madctl does nothing" was expected. This image cycles all 8
 * MY/MX/MV combinations (with the BGR|ML baseline 0x10 kept) and FULLY
 * RE-INITS the panel per candidate (lcd_init, the proven boot path) so
 * every value is actually applied — a bare 0x36 mid-stream is not
 * guaranteed to stick on this LCM.
 *
 * MADCTL bits (ST7735 cmd 0x36): bit7 MY (row order / vertical flip),
 * bit6 MX (col order), bit5 MV (swap axes / 90 deg), bit4 ML, bit3 BGR.
 * Baseline 0x10 (ML) is kept constant; candidates:
 *   0x10         (no flip)        0x90  MY
 *   0x50  MX                      0xd0  MY|MX
 *   0x30  MV                      0xb0  MY|MV
 *   0x70  MX|MV                   0xf0  MY|MX|MV
 *
 * Boot path: usb_debug_init -> per-candidate { lcd_set_madctl(v);
 * lcd_init(); paint_rot_screen(v); dwell }. lcd_set_madctl stores the
 * override so the following lcd_init() applies it through the full
 * init-table path (MADCTL + window + Memory Write + LCDC re-setup) —
 * guaranteed to reach the panel. No chip init, no scheduler.
 *
 * Reading the result: one of the eight labels will show "MADCTL=0x.."
 * with "TOP->BOT" reading top-to-bottom and the ramps running red
 * left->right / green top->bottom — that value is the one to set
 * B310E_MADCTL to in include/arch.h. Each candidate is also echoed over
 * USB via kprintf, so libc_server shows which value is on screen.
 *
 * SAFETY: same as the other diag images — no 0x8c pinmux writes, no
 * interrupts added, no production driver changes beyond the exported
 * lcd_set_madctl helper (the LCD register sequence is untouched).
 */

#include "os.h"
#include "chip.h"
#include "../drivers/usb_debug.h"
#include "../drivers/lcd.h"

/* On-screen dwell per candidate MADCTL value (full re-init + paint). */
#define DIAG_ROT_DWELL_MS 1500u

/* All 8 MY/MX/MV orientations on top of the fixed ML|BGR baseline 0x10.
 * One of these is the physical mount orientation of the B310E panel. */
static const uint8_t s_candidates[8] = {
    0x10, 0x90, 0x50, 0xd0,      /* no MV (flips only)          */
    0x30, 0xb0, 0x70, 0xf0,      /* MV: row/col swap (90 deg)   */
};

/* Paint one orientation-discriminating screen for MADCTL value v: blue
 * background, a "MADCTL=0x.." label, a "TOP->BOT" direction marker, a red
 * ramp running left->right across the middle and a green ramp running
 * top->bottom down the right edge. A flipped/rotated panel turns the
 * labels upside-down or backwards and inverts the ramp directions, so the
 * upright candidate is unmistakable. */
static void paint_rot_screen(uint8_t v)
{
    unsigned x, y;

    lcd_fill(0x001f);               /* BLUE so orientation is visible */

    /* Line 1: the candidate value in hex (white on blue). */
    {
        char buf[12];
        unsigned hi = ((unsigned)v >> 4) & 0xfu;
        unsigned lo = (unsigned)v & 0xfu;

        buf[0] = 'M'; buf[1] = 'A'; buf[2] = 'D'; buf[3] = 'C';
        buf[4] = 'T'; buf[5] = 'L'; buf[6] = '='; buf[7] = '0'; buf[8] = 'x';
        buf[9] = (char)(hi < 10u ? '0' + hi : 'a' + hi - 10u);
        buf[10] = (char)(lo < 10u ? '0' + lo : 'a' + lo - 10u);
        buf[11] = '\0';
        lcd_print(4, 20, buf, 0xffff, 0x001f);
    }

    /* Line 2: direction marker — reads upside-down / backwards / vertical
     * if the panel is flipped or rotated. */
    lcd_print(4, 36, "TOP->BOT", 0xffff, 0x001f);

    /* Red ramp, left -> right, across the middle of the panel. */
    for (y = 60; y < 76; y++)
        for (x = 0; x < LCD_W; x++)
            lcd_fb[y * LCD_W + x] =
                (uint16_t)((x * 31u / (LCD_W - 1u)) << 11);

    /* Green ramp, top -> bottom, down the right edge of the panel. */
    for (y = 0; y < LCD_H; y++)
        for (x = 120; x < LCD_W; x++)
            lcd_fb[y * LCD_W + x] =
                (uint16_t)((y * 63u / (LCD_H - 1u)) << 5);

    lcd_show();
}

void main(void)
{
    unsigned idx = 0;

    /* 1. fdl_ack FIRST — usb_debug_init() sends it, unblocking spd_dump
     *    before anything risky runs. */
    usb_debug_init();

    /* 2. Cycle the candidates forever. lcd_set_madctl(v) stores the
     *    override; the FULL lcd_init() after it re-runs the proven boot
     *    init table (MADCTL + window + 0x2c + LCDC setup) so the value is
     *    guaranteed applied — the live-switch path alone does not stick
     *    on this LCM. The user reads the labels and reports the upright
     *    value; we set B310E_MADCTL to it. */
    for (;;) {
        uint8_t v = s_candidates[idx];

        kprintf("diag-rot: MADCTL=0x%x\n", (unsigned)v);
        lcd_set_madctl(v);
        lcd_init();
        paint_rot_screen(v);
        lcd_delay_ms(DIAG_ROT_DWELL_MS);
        idx = (idx + 1) % (sizeof(s_candidates) / sizeof(s_candidates[0]));
    }
}
