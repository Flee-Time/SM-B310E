/*
 * B310E-OS — drivers/lcd.h
 *
 * ST7735 128x160 color TFT (Sitronix ST7735S TNM/DTC) driver interface for
 * the Samsung SM-B310E. The panel is attached over the SoC's parallel DBI
 * (LCM controller @ 0x20800000) and is refreshed by the LCDC display
 * controller @ 0x20d00000 with hardware DMA — no software SPI, no
 * interrupts (polled refresh only).
 *
 * Draw into the public framebuffer (lcd_fb, RGB565, row-major), then call
 * lcd_show() once per batch to trigger one LCDC DMA refresh.
 */

#ifndef B310E_OS_DRIVERS_LCD_H
#define B310E_OS_DRIVERS_LCD_H

#include <stdint.h>

#include "module.h"     /* module_t (kernel framework) */

#define LCD_W 128u
#define LCD_H 160u

/* Kernel module registration — wave 8 integration wires it into
 * module_init_all(). Until then --gc-sections drops it; main.c calls
 * lcd_init() directly (see arch/main.c). */
extern const module_t lcd_module;

/* RGB565 framebuffer, 128x160, row-major: lcd_fb[y * LCD_W + x].
 * ~40 KB of BSS — fine in the 4 MB PSRAM. */
extern uint16_t lcd_fb[LCD_W * LCD_H];

/* Power + init the LCM/LCDC + ST7735, set the framebuffer base, clear the
 * screen, enable the backlight. Returns 0 on success, -1 on failure. */
int  lcd_init(void);

/* Fill the whole framebuffer with one RGB565 color. */
void lcd_fill(uint16_t color);

/* Draw one 8x16 glyph cell at (x, y) (top-left), fg on bg. Clips. */
void lcd_putc(int x, int y, char c, uint16_t fg, uint16_t bg);

/* Draw an ASCII string, 8 px advance per char, '\n' -> y += 16. */
void lcd_print(int x, int y, const char *s, uint16_t fg, uint16_t bg);

/* Compact renderers (5x7 native, 6x8 cell — 21 cols x 20 rows). */
void lcd_putc_small(int x, int y, char c, uint16_t fg, uint16_t bg);
void lcd_print_small(int x, int y, const char *s, uint16_t fg, uint16_t bg);

/* Dense console glyph (5x7 in a 6x9 cell — 1px char gap, 2px row gap). */
void lcd_putc_console(int x, int y, char c, uint16_t fg, uint16_t bg);

/* Trigger the LCDC DMA refresh and wait for the frame to complete. */
void lcd_show(void);

/* Bounded-refresh variant of lcd_show() for diagnostics: same page_reset==0
 * pure-DMA refresh, but the completion wait times out after ~1 s instead of
 * spinning forever — the diagnostic image can never hang on a dead DMA. */
void lcd_show_bounded(void);

/* Rotation diagnostic helper (os-diag-rot.bin): switch the panel's
 * MADCTL (ST7735 cmd 0x36) live with a SINGLE command + data byte. The
 * CASET/PASET window and the 0x2c Memory Write stay as set at init —
 * re-sending them mid-stream collapses the panel GRAM window. The diag
 * main repaints a labeled screen per candidate; the user reports which
 * label renders upright. */
void lcd_set_madctl(uint8_t v);

/* Busy-wait on the 1 ms system timer (0x8100300c). */
void lcd_delay_ms(uint32_t ms);

/* Read the 1 ms system timer (0x8100300c) — stable (double-read). */
uint32_t lcd_sys_timer_ms(void);

#endif /* B310E_OS_DRIVERS_LCD_H */
