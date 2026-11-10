/* B310E OS — driver interfaces.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef OS_DRIVERS_H
#define OS_DRIVERS_H

#include <stdbool.h>
#include <stdint.h>
#include "os/arch.h"

/* ---- LCD (ST7735S through the LCM/LCDC) -------------------------------- */
/* Pixels are plain RGB565 in memory (the LCDC's endian mode 2 reads
 * little-endian halfwords; Rockbox's LCD_PIXELFORMAT is RGB565 too). */
#define RGB(r, g, b) ((uint16_t)((((r) & 0xf8) << 8) | (((g) & 0xfc) << 3) | \
                                 ((b) >> 3)))

/* Everything draws into lcd_fb, a back buffer; lcd_update() copies it to
 * FRAME_ADDR, which the LCDC reads, and starts the transfer. */
extern uint16_t lcd_back[LCD_WIDTH * LCD_HEIGHT];
#define lcd_fb lcd_back

void lcd_init(void);
void lcd_update(void);          /* push the whole frame (not while asleep) */
uint32_t lcd_busy_ms(void);     /* the last push: how long it still takes */
void lcd_present(const uint16_t *px);  /* a whole frame, sent where it lies; NULL: as it is */
const uint16_t *lcd_source(void);    /* what it shows: a game's frame, or the OS's */
void lcd_sleep(void);           /* display off, panel sleep, LCDC clock off */
void lcd_wake(void);            /* back on, then pushes the frame */
bool lcd_asleep(void);
uint32_t lcd_transfer_ms(void);     /* the spacing between transfers */
extern volatile uint32_t lcd_kicks; /* transfers started */
uint32_t lcd_measure(void);         /* one transfer by the done flag, us */

/* ---- backlight, keypad light ------------------------------------------- */
void backlight_set(int percent);
void keylight_set(bool on);
void vibrator_set(int level);       /* 0 off, 1..7 */

/* ---- system DMA (drivers/dma.c) ------------------------------------------ */
void dma_quiesce(void);             /* every channel off, every request unbound */

/* ---- GPIO, torch (drivers/gpio.c) --------------------------------------- */
void gpio_clock_on(void);
void torch_set(bool on);
bool torch_get(void);

/* ---- keypad ------------------------------------------------------------ */
/* Key codes are the stock keymap's (fpdoom KEYPAD_ENUM). */
enum {
    KEY_DIAL = 0x01, KEY_END = 0x02, KEY_UP = 0x04, KEY_DOWN = 0x05,
    KEY_LEFT = 0x06, KEY_RIGHT = 0x07, KEY_LSOFT = 0x08, KEY_RSOFT = 0x09,
    KEY_CENTER = 0x0d, KEY_HASH = 0x23, KEY_STAR = 0x2a,
    KEY_0 = 0x30, KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9,
    /* headset in-line buttons (4-pole headsets, drivers/audio.c) */
    KEY_HS_CENTER = 0x70, KEY_HS_PLUS = 0x71, KEY_HS_MINUS = 0x72,
};

typedef struct {
    uint8_t code;
    uint8_t down;               /* 1 press, 0 release */
    uint32_t time;              /* kernel_ticks() */
    uint8_t repeat;             /* the UI's auto-repeat of a held key (appended) */
} key_event_t;

void keypad_init(void);
int keypad_get(key_event_t *ev, uint32_t timeout_ms);
void keypad_inject(uint8_t code, bool down);    /* code 0: a wake-up only */
const char *keypad_name(uint8_t code);
/* Stop / restart the matrix scan around SDIO DMA (a held key during it
 * hangs the phone); resume first collects edges that came meanwhile. */
void keypad_pause(void);
void keypad_resume(void);

/* ---- battery, charger, clock (drivers/battery.c) ------------------------ */
uint32_t adc_read(unsigned ch);     /* raw 10-bit, or 0xffffffff */
bool charger_present(void);
enum { CHARGE_NONE, CHARGE_CHARGING, CHARGE_DONE };
int charge_state(void);             /* CHARGE_* from the charger's lines */
int battery_mv(void);               /* last unplugged reading, -1 unknown */
extern volatile uint32_t ui_alive_at;   /* the UI loop's last pass: is it stuck? */
extern volatile bool ui_end_held;       /* END still held long after: power off */
int battery_percent(int mv);        /* -1 unknown */
void rtc_time(int *h, int *m, int *s);

/* ---- SD card ------------------------------------------------------------ */
typedef struct {
    uint32_t blocks_read, blocks_written, retries, failures, reinits;
} sd_stats_t;

int sd_init(void);              /* 0, or the probe step that failed (1-8) */
bool sd_present(void);
uint32_t sd_sectors(void);      /* 512-byte blocks */
int sd_read(uint32_t lba, void *buf, unsigned count);
int sd_write(uint32_t lba, const void *buf, unsigned count);
void sd_get_stats(sd_stats_t *out);

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
