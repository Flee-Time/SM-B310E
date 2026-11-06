/* B310E OS — driver interfaces.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef OS_DRIVERS_H
#define OS_DRIVERS_H

#include <stdbool.h>
#include <stdint.h>
#include "os/arch.h"

/* ---- LCD (ST7735S through the LCM/LCDC) -------------------------------- */
/* Pixels are RGB565 with the bytes swapped (the LCDC reads them that way). */
#define RGB(r, g, b) ((uint16_t)( \
    (((r) & 0xf8) | ((g) >> 5)) | \
    (((((g) & 0x1c) << 3) | ((b) >> 3)) << 8)))

#define lcd_fb ((uint16_t *)FRAME_ADDR)

void lcd_init(void);
void lcd_update(void);          /* push the whole frame to the panel */

/* ---- backlight, keypad light ------------------------------------------- */
void backlight_set(int percent);
void keylight_set(bool on);

/* ---- keypad ------------------------------------------------------------ */
/* Key codes are the stock keymap's (fpdoom KEYPAD_ENUM). */
enum {
    KEY_DIAL = 0x01, KEY_END = 0x02, KEY_UP = 0x04, KEY_DOWN = 0x05,
    KEY_LEFT = 0x06, KEY_RIGHT = 0x07, KEY_LSOFT = 0x08, KEY_RSOFT = 0x09,
    KEY_CENTER = 0x0d, KEY_HASH = 0x23, KEY_STAR = 0x2a,
    KEY_0 = 0x30, KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9,
};

typedef struct {
    uint8_t code;
    uint8_t down;               /* 1 press, 0 release */
    uint32_t time;              /* kernel_ticks() */
} key_event_t;

void keypad_init(void);
int keypad_get(key_event_t *ev, uint32_t timeout_ms);
const char *keypad_name(uint8_t code);

/* ---- text console on the LCD ------------------------------------------- */
#define CON_COLS (LCD_WIDTH / 6)    /* 21 */
#define CON_ROWS (LCD_HEIGHT / 8)   /* 20 */

void console_init(void);
void console_reserve(int rows);     /* fixed status rows at the top */
void console_set_color(uint16_t fg, uint16_t bg);
void console_clear(void);
void console_goto(int col, int row);
void console_write(const char *s);
void console_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* Print at a fixed row without moving the cursor (status lines). */
void console_line(int row, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void console_flush(void);

#endif
