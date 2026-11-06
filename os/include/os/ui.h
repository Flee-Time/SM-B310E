/* B310E OS — user interface service.
 *
 * One UI thread owns the screen. Screens form a stack; the top one gets
 * keys and draws the content area between the status bar (clock, card,
 * battery), the title bar and the soft-key bar (left / centre / right
 * labels for LSoft, Center, RSoft). Keys: the D-pad moves (held Up/Down
 * repeat), Center and LSoft act, RSoft goes back, END goes home, END held
 * 2 s powers off. The backlight dims and then goes dark after the screen
 * timeout; the first key only wakes it.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef OS_UI_H
#define OS_UI_H

#include <stdbool.h>
#include <stdint.h>
#include "os/drivers.h"

/* ---- layout ------------------------------------------------------------- */
#define UI_STATUS_H   12
#define UI_TITLE_H    16
#define UI_SOFT_H     16
#define UI_TOP        (UI_STATUS_H + UI_TITLE_H)
#define UI_BOTTOM     (LCD_HEIGHT - UI_SOFT_H)
#define UI_CONTENT_H  (UI_BOTTOM - UI_TOP)
#define UI_ROW_H      16

/* ---- theme -------------------------------------------------------------- */
#define C_BG       RGB(14, 18, 26)
#define C_BAR      RGB(32, 40, 56)
#define C_TITLE    RGB(24, 30, 44)
#define C_TEXT     RGB(235, 238, 245)
#define C_DIM      RGB(140, 150, 170)
#define C_ACCENT   RGB(60, 140, 255)
#define C_SEL_TEXT RGB(255, 255, 255)
#define C_OK       RGB(110, 220, 120)
#define C_WARN     RGB(255, 180, 60)
#define C_BAD      RGB(255, 90, 90)

/* ---- drawing (ui/gfx.c) ------------------------------------------------- */
enum { FONT_SMALL, FONT_LARGE };    /* 6 x 8 and 8 x 16 cells */

void gfx_fill(int x, int y, int w, int h, uint16_t c);
void gfx_hline(int x, int y, int w, uint16_t c);
void gfx_rect(int x, int y, int w, int h, uint16_t c);
/* Draws transparent text; stops at max_w pixels (0: no limit) with "..".
 * Returns the width drawn. */
int gfx_text(int x, int y, const char *s, uint16_t c, int font, int scale,
             int max_w);
int gfx_text_width(const char *s, int font, int scale);
int gfx_text_centered(int y, const char *s, uint16_t c, int font, int scale);

/* ---- screens (ui/ui.c) -------------------------------------------------- */
typedef struct screen screen_t;
struct screen {
    const char *title;
    const char *left, *center, *right;          /* soft keys */
    void (*enter)(screen_t *s);                 /* pushed */
    void (*leave)(screen_t *s);                 /* popped */
    void (*draw)(screen_t *s);                  /* the content area */
    void (*key)(screen_t *s, const key_event_t *ev);  /* presses, repeats */
    void (*tick)(screen_t *s);                  /* every tick_ms */
    void *data;
    bool free_on_pop;                           /* kfree(s) when popped */
    uint32_t tick_ms;                           /* 0: once a second */
    struct app *app;                            /* owner (set by ui_push) */
};

/* screen_t is part of the app API (os/app.h): add members only at the end. */

void ui_push(screen_t *s);
void ui_pop(void);
void ui_home(void);                 /* back to the bottom screen */
void ui_redraw(void);
int ui_depth(void);
void ui_run(screen_t *home) __attribute__((noreturn));   /* the UI thread */
void ui_power_off(void) __attribute__((noreturn));
void ui_reboot(void) __attribute__((noreturn));

/* settings, kept in /b310e-os/settings.txt */
typedef struct {
    int brightness;                 /* percent */
    int screen_timeout_s;           /* 0: never */
    int cpu_max;                    /* 1: always 208 MHz, 0: governor */
} ui_settings_t;

extern ui_settings_t ui_settings;
void ui_settings_save(void);
void ui_apply_brightness(void);

/* ---- widgets (ui/widgets.c) --------------------------------------------- */
typedef struct {
    int count, sel, top;
    /* text of item i; detail (right-aligned, dim) may stay empty */
    void (*item)(void *ctx, int i, char *text, int tn, char *detail, int dn);
    void *ctx;
} list_t;

void list_draw(list_t *l);
bool list_key(list_t *l, const key_event_t *ev);   /* Up/Down; true if used */

/* A confirm dialog: LSoft runs yes() and closes; RSoft just closes. */
void ui_confirm(const char *title, const char *message, void (*yes)(void));
/* A message box until any key. */
void ui_message(const char *title, const char *message);
/* A scrolling text view of a copy of text (lines split at '\n' and wrapped). */
void ui_text_view(const char *title, const char *text);

/* ---- apps (os/apps/) ---------------------------------------------------- */
screen_t *app_home(void);
screen_t *app_menu(void);
screen_t *app_files(const char *path);
screen_t *app_system(void);
screen_t *app_settings(void);
screen_t *app_power(void);
screen_t *app_sound(void);
screen_t *app_radio(void);
screen_t *app_tools(void);
screen_t *app_sim(void);            /* apps/sim.c */
screen_t *app_clock(void);          /* apps/clock.c: Date & time */

#endif
