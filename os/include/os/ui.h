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
#define UI_STATUS_H   11
#define UI_TITLE_H    15
#define UI_SOFT_H     14
#define UI_TOP        (UI_STATUS_H + UI_TITLE_H)
#define UI_BOTTOM     (LCD_HEIGHT - UI_SOFT_H)
#define UI_CONTENT_H  (UI_BOTTOM - UI_TOP)
#define UI_ROW_H      18                /* a list card (16) and its gap */
#define UI_PAD        3                 /* the screen's side margin */
#define UI_RADIUS     2                 /* cards and buttons: barely round */

/* ---- theme: dark and warm, a little retro; one amber accent ------------- */
#define C_BG       RGB(28, 26, 34)
#define C_BAR      RGB(22, 20, 27)      /* status and soft-key bars */
#define C_TITLE    RGB(28, 26, 34)
#define C_CARD     RGB(46, 43, 56)      /* list rows, panels, dialogs */
#define C_CARD_HI  RGB(70, 66, 82)      /* tracks, pressed */
#define C_TEXT     RGB(236, 228, 208)   /* cream */
#define C_DIM      RGB(156, 146, 126)   /* warm grey */
#define C_ACCENT   RGB(232, 163, 61)    /* amber */
#define C_SEL_TEXT RGB(28, 26, 34)      /* dark on the amber */
#define C_OK       RGB(143, 181, 115)   /* sage */
#define C_WARN     RGB(232, 163, 61)
#define C_BAD      RGB(208, 101, 79)    /* brick */

/* ---- drawing (ui/gfx.c) ------------------------------------------------- */
/* Montserrat, anti-aliased (ui/font_montserrat.c): FONT_SMALL 9 px,
 * FONT_BODY 10 px, FONT_LARGE 12 px semibold, FONT_HUGE 24 px (digits,
 * ':', '.', '-', '+', '%'); FONT_LARGE at scale 2 or more is FONT_HUGE.
 * y is the top of the capitals; text_height() is their height. */
enum { FONT_SMALL, FONT_LARGE, FONT_BODY, FONT_HUGE };

void gfx_fill(int x, int y, int w, int h, uint16_t c);
void gfx_hline(int x, int y, int w, uint16_t c);
void gfx_rect(int x, int y, int w, int h, uint16_t c);
/* Draws transparent text; stops at max_w pixels (0: no limit) with "..".
 * Returns the width drawn. */
int gfx_text(int x, int y, const char *s, uint16_t c, int font, int scale,
             int max_w);
int gfx_text_width(const char *s, int font, int scale);
int gfx_text_centered(int y, const char *s, uint16_t c, int font, int scale);
int gfx_text_height(int font, int scale);       /* capital height */

/* Rounded shapes with anti-aliased corners; alpha 0..255 (shadows, dims) */
void gfx_round(int x, int y, int w, int h, int r, uint16_t c);
void gfx_round_alpha(int x, int y, int w, int h, int r, uint16_t c, int alpha);
void gfx_shade(int x, int y, int w, int h, uint16_t c, int alpha);

typedef struct { uint8_t a[72]; } icon_t;      /* 12 x 12, 4-bit alpha */
#define ICON_SIZE 12
void gfx_icon(int x, int y, const icon_t *icon, uint16_t c);
void gfx_blit(int x, int y, int w, int h, const uint16_t *px);  /* RGB565 rows of w */

/* widgets drawn in place */
void gfx_bar(int x, int y, int w, int h, int value, int max, uint16_t c);
void gfx_switch(int x, int y, bool on);         /* 20 x 10 */
/* history: n samples in a ring, the newest at v[head - 1]; max 0: auto */
int gfx_graph(int x, int y, int w, int h, const uint16_t *v, int n, int head,
              int max, uint16_t c);

extern const icon_t icon_apps, icon_folder, icon_file, icon_music, icon_radio,
    icon_tools, icon_chart, icon_gear, icon_power, icon_torch, icon_vibrate,
    icon_sim, icon_usb, icon_clock, icon_sun, icon_screen, icon_cpu, icon_info,
    icon_play, icon_record, icon_mic, icon_restart,
    icon_calc, icon_notes, icon_alarm, icon_dot_on, icon_dot_off, icon_speaker, icon_card,
    icon_speaker_off, icon_headphones, icon_headset;

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
    bool overlay;                               /* drawn over the screen under it */
};

/* screen_t is part of the app API (os/app.h): add members only at the end. */

void ui_push(screen_t *s);
void ui_pop(void);
/* A screen in a mode of its own while it is on top (games; API 2.5),
 * UI_MODE_* or-ed, 0 for the usual one; the UI thread only. It lasts until
 * the screen is popped (set it again before pushing it anew).
 *   UI_MODE_FULL      draw() paints the whole 128 x 160 LCD: no status,
 *                     title or soft-key bars, nothing cleared before it
 *   UI_MODE_RAW_KEYS  key() gets every press and every release, END and
 *                     Center too, without repeats, tactile buzz or the
 *                     held-key gestures (quick panel, torch); END held
 *                     2 s still powers off
 *   UI_MODE_AWAKE     no screen timeout */
#define UI_MODE_FULL      1u
#define UI_MODE_RAW_KEYS  2u
#define UI_MODE_AWAKE     4u
void ui_screen_mode(screen_t *s, unsigned flags);
/* From a full-screen draw(): a whole frame sent where it lies, no copy: 32-byte
 * aligned, and not written until another is sent. The one sent already, or
 * NULL: the panel stays as it is. False if the screen is not on top and
 * full-screen (nothing done: draw as usual). */
bool ui_present(const uint16_t *px);
void ui_home(void);                 /* back to the bottom screen */
void ui_redraw(void);
void ui_refresh(void);
void ui_collect_apps(void);          /* unload apps that are no longer used */               /* ui_redraw from another thread: at once */
int ui_depth(void);
void ui_run(screen_t *home) __attribute__((noreturn));   /* the UI thread */
void ui_power_off(void) __attribute__((noreturn));
void ui_reboot(void) __attribute__((noreturn));
/* Start another image from the card in place of this OS, without a reset
 * (the clock keeps running). Returns only on an error. */
const char *ui_chain(const char *path, const char *what);
#define OS_IMAGE_PATH  "/payloads/b310e-os.bin"
/* From another thread: run fn(arg) in the UI thread and wait for it.
 * false when the UI did not take it within timeout_ms (then it never
 * runs). arg must stay valid until it returns. fn runs as the caller's
 * app (app_current), so what it pushes and allocates is that app's. */
bool ui_call(void (*fn)(void *arg), void *arg, uint32_t timeout_ms);

/* settings, kept in /b310e-os/settings.txt */
typedef struct {
    int brightness;                 /* percent */
    int screen_timeout_s;           /* 0: never */
    int cpu_max;                    /* 1: always 208 MHz, 0: governor */
    int deep_idle;                  /* 1: tickless idle (default) */
    int alarm_on;                   /* the 2026-11-07 single alarm: read once, */
    int alarm_min;                  /* moved into the alarm service's list */
    int vol[5];                     /* VOL_*: 0 mute .. 10 (tactile 0 .. 3) */
    int keylight;                   /* 1: the keypad light while the screen is on */
    int usb_default;                /* on the plug: 0 charge only, 1 storage, 2 network */
    int tz_min;                     /* the time zone: minutes from UTC */
    int net_time;                   /* 1: the clock from the network (SNTP) */
    int dev_mode;                   /* 1: show developer tools (DSP boot, ...) */
} ui_settings_t;

extern ui_settings_t ui_settings;
void ui_settings_save(void);
void ui_apply_brightness(void);
void ui_wake(void);                 /* light the screen, as a key would */
void ui_buzz(int level, uint32_t ms);   /* vibrate at level 1..7 for ms (5 s at most) */

/* ---- widgets (ui/widgets.c) --------------------------------------------- */
typedef struct {
    int count, sel, top;
    /* text of item i; detail (right-aligned, dim) may stay empty */
    void (*item)(void *ctx, int i, char *text, int tn, char *detail, int dn);
    void *ctx;
    /* optional: the row's icon, or NULL; a switch instead of the detail
     * text when the row's toggle returns 0 or 1 (-1: no switch) */
    const icon_t *(*icon)(void *ctx, int i);
    int (*toggle)(void *ctx, int i);
} list_t;

void list_draw(list_t *l);
bool list_key(list_t *l, const key_event_t *ev);   /* Up/Down; true if used */

/* A confirm dialog: LSoft runs yes() and closes; RSoft just closes. */
void ui_confirm(const char *title, const char *message, void (*yes)(void));
/* A message box until any key. */
void ui_message(const char *title, const char *message);
/* A scrolling text view of a copy of text (lines split at '\n' and wrapped). */
void ui_text_view(const char *title, const char *text);
/* Options to pick from (the current one marked); picked(i, ctx) once one
 * is chosen. title and options must outlive the picker. */
void ui_choose(const char *title, const char *const *options, int n, int current,
               void (*picked)(int i, void *ctx), void *ctx);
/* label ... value in one line of w pixels; neither runs into the other */
void ui_kv(int x, int y, int w, const char *label, const char *value, uint16_t c);
/* A multi-tap text editor (ui/textedit.c) with a copy of initial, up to
 * max characters; Center calls done() with the text, RSoft just leaves.
 * multiline: # starts a new line. title must outlive the screen. */
void ui_text_edit(const char *title, const char *initial, int max, bool multiline,
                  void (*done)(const char *text, void *ctx), void *ctx);

/* ---- apps (os/apps/) ---------------------------------------------------- */
screen_t *app_home(void);
screen_t *app_menu(void);
screen_t *app_files(const char *path);
screen_t *app_system(void);
screen_t *app_settings(void);
screen_t *app_power(void);
screen_t *app_tools(void);
screen_t *app_sim(void);            /* apps/sim.c */
screen_t *app_clock(void);          /* apps/clock.c: Date & time */

#endif
