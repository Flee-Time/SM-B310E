/* B310E OS — text console on the LCD: 21 x 20 cells of 6 x 8 pixels,
 * scrolling, colours, fixed status lines. Thread-safe through one mutex;
 * after a fault (console_unlocked) it draws without locking.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdarg.h>
#include "os/arch.h"
#include "os/drivers.h"
#include "os/kernel.h"
#include "os/lib.h"

extern const uint8_t font5x7[95][5];

bool console_unlocked;
static mutex_t lock;
static int cur_col, cur_row, top_row;   /* rows above top_row don't scroll */
static uint16_t fg = RGB(255, 255, 255), bg = RGB(0, 0, 0);

static void enter(void)
{
    if (!console_unlocked)
        mutex_lock(&lock, WAIT_FOREVER);
}

static void leave(void)
{
    if (!console_unlocked)
        mutex_unlock(&lock);
}

static void draw_char(int col, int row, char c)
{
    const uint8_t *g;
    uint16_t *p = lcd_fb + row * 8 * LCD_WIDTH + col * 6;

    if ((unsigned char)c < 0x20 || (unsigned char)c > 0x7e)
        c = '?';
    g = font5x7[c - 0x20];
    for (int y = 0; y < 8; y++, p += LCD_WIDTH)
        for (int x = 0; x < 6; x++)
            p[x] = (x < 5 && y < 7 && (g[x] >> y) & 1) ? fg : bg;
}

static void clear_rows(int from, int to)
{
    for (int y = from * 8; y < to * 8; y++)
        for (int x = 0; x < LCD_WIDTH; x++)
            lcd_fb[y * LCD_WIDTH + x] = bg;
}

static void scroll(void)
{
    int rows = CON_ROWS - top_row;

    if (rows > 1)
        memmove(lcd_fb + top_row * 8 * LCD_WIDTH,
                lcd_fb + (top_row + 1) * 8 * LCD_WIDTH,
                (size_t)(rows - 1) * 8 * LCD_WIDTH * 2);
    clear_rows(CON_ROWS - 1, CON_ROWS);
    cur_row = CON_ROWS - 1;
}

static void put(char c)
{
    if (c == '\n') {
        cur_col = 0;
        if (++cur_row >= CON_ROWS)
            scroll();
        return;
    }
    if (c == '\r') {
        cur_col = 0;
        return;
    }
    if (cur_col >= CON_COLS) {
        cur_col = 0;
        if (++cur_row >= CON_ROWS)
            scroll();
    }
    draw_char(cur_col++, cur_row, c);
}

void console_init(void)
{
    mutex_init(&lock);
    cur_col = cur_row = 0;
    top_row = 0;
    clear_rows(0, CON_ROWS);
}

/* Rows above `rows` stay fixed (status lines); the rest scrolls. */
void console_reserve(int rows)
{
    enter();
    top_row = rows;
    if (cur_row < rows)
        cur_row = rows;
    leave();
}

void console_set_color(uint16_t f, uint16_t b)
{
    fg = f;
    bg = b;
}

void console_clear(void)
{
    enter();
    clear_rows(top_row, CON_ROWS);
    cur_col = 0;
    cur_row = top_row;
    leave();
}

void console_goto(int col, int row)
{
    cur_col = col;
    cur_row = row;
}

void console_write(const char *s)
{
    enter();
    while (*s)
        put(*s++);
    leave();
}

void console_printf(const char *fmt, ...)
{
    char buf[128];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    console_write(buf);
}

void console_line(int row, const char *fmt, ...)
{
    char buf[CON_COLS + 1];
    va_list ap;
    int i;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    enter();
    for (i = 0; buf[i] && i < CON_COLS; i++)
        draw_char(i, row, buf[i]);
    for (; i < CON_COLS; i++)
        draw_char(i, row, ' ');
    leave();
}

void console_flush(void)
{
    enter();
    lcd_update();
    leave();
}
