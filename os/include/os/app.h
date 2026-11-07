/* B310E OS — loadable apps: the file format and the API the OS exports.
 *
 * An app is a file /b310e-os/apps/<name>.app built with os/sdk:
 *
 *   app_file_t        header, written by os/sdk/mkapp.py
 *   image             code, read-only data and data, linked at address 0;
 *                     an app_info_t at offset 0
 *   relocations       reloc_count little-endian uint32 offsets into the
 *                     image of words that hold an image address (each gets
 *                     the load address added)
 *
 * The loader reads it into the heap, applies the relocations, zeroes
 * bss_size bytes after the image, checks the CRC and calls info->start
 * with the API table. Apps call nothing in the OS directly: every service
 * is a pointer in os_api_t, so an app keeps working across OS builds as
 * long as the API's major version matches (minors only append members).
 *
 * Apps run in the UI thread: start() returns the app's first screen and
 * every callback of the app's screens runs there. Screens the app pushes
 * and memory it gets from api->malloc belong to it; when the last of its
 * screens leaves the stack, stop() runs and the app and its memory are
 * freed. Files it leaves open are not closed for it.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef OS_APP_H
#define OS_APP_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "ff.h"
#include "os/ui.h"

/* 2.0 (2026-11-07): the new UI. screen_t gained overlay, list_t icon and
 * toggle, so 1.x apps are refused; the table gained the drawing calls
 * after f_rename. */
#define OS_API_MAJOR 2
#define OS_API_MINOR 0

#define APP_MAGIC   0x50413342u         /* "B3AP" */
#define APP_DIR     "/b310e-os/apps"
#define APP_MAX_SIZE (512u * 1024)      /* image + bss */

typedef struct {
    uint32_t magic;
    uint16_t api_major, api_minor;      /* the API the app was built for */
    uint32_t header_size;               /* sizeof(app_file_t) */
    uint32_t image_size;
    uint32_t bss_size;
    uint32_t reloc_count;
    uint32_t crc32;                     /* image, then the relocations */
    char name[20];                      /* NUL-terminated */
} app_file_t;

typedef struct os_api os_api_t;

typedef struct {
    uint16_t api_major, api_minor;
    const char *name;
    screen_t *(*start)(const os_api_t *os);
    void (*stop)(void);                 /* may be NULL */
} app_info_t;

struct os_api {
    uint16_t major, minor;
    uint32_t size;                      /* sizeof(os_api_t) of this OS */

    /* C library (memcpy, memset, memmove are in the SDK: the compiler
     * calls them itself) */
    int (*memcmp)(const void *a, const void *b, size_t n);
    size_t (*strlen)(const char *s);
    int (*strcmp)(const char *a, const char *b);
    char *(*strncpy)(char *dst, const char *src, size_t n);
    char *(*strchr)(const char *s, int c);
    int (*snprintf)(char *buf, size_t size, const char *fmt, ...);
    int (*vsnprintf)(char *buf, size_t size, const char *fmt, va_list ap);

    /* memory, freed with the app */
    void *(*malloc)(size_t size);
    void *(*zalloc)(size_t size);
    void (*free)(void *p);

    /* time, log, device */
    uint32_t (*ticks)(void);            /* ms since boot */
    void (*sleep)(uint32_t ms);         /* blocks the UI: keep it short */
    void (*klog)(const char *fmt, ...);
    void (*rtc_time)(int *h, int *m, int *s);
    int (*battery_mv)(void);
    int (*battery_percent)(int mv);
    bool (*charger_present)(void);

    /* drawing (ui/gfx.c) */
    void (*gfx_fill)(int x, int y, int w, int h, uint16_t c);
    void (*gfx_hline)(int x, int y, int w, uint16_t c);
    void (*gfx_rect)(int x, int y, int w, int h, uint16_t c);
    int (*gfx_text)(int x, int y, const char *s, uint16_t c, int font,
                    int scale, int max_w);
    int (*gfx_text_width)(const char *s, int font, int scale);
    int (*gfx_text_centered)(int y, const char *s, uint16_t c, int font,
                             int scale);

    /* screens and widgets (ui/ui.c, ui/widgets.c) */
    void (*ui_push)(screen_t *s);
    void (*ui_pop)(void);
    void (*ui_home)(void);
    void (*ui_redraw)(void);
    void (*list_draw)(list_t *l);
    bool (*list_key)(list_t *l, const key_event_t *ev);
    void (*ui_confirm)(const char *title, const char *message, void (*yes)(void));
    void (*ui_message)(const char *title, const char *message);
    void (*ui_text_view)(const char *title, const char *text);
    const ui_settings_t *settings;

    /* files: FatFs R0.15a with os/fs/ffconf.h (UTF-8 paths) */
    FRESULT (*f_open)(FIL *fp, const TCHAR *path, BYTE mode);
    FRESULT (*f_close)(FIL *fp);
    FRESULT (*f_read)(FIL *fp, void *buff, UINT btr, UINT *br);
    FRESULT (*f_write)(FIL *fp, const void *buff, UINT btw, UINT *bw);
    FRESULT (*f_lseek)(FIL *fp, FSIZE_t ofs);
    FRESULT (*f_sync)(FIL *fp);
    FRESULT (*f_opendir)(DIR *dp, const TCHAR *path);
    FRESULT (*f_closedir)(DIR *dp);
    FRESULT (*f_readdir)(DIR *dp, FILINFO *fno);
    FRESULT (*f_stat)(const TCHAR *path, FILINFO *fno);
    FRESULT (*f_mkdir)(const TCHAR *path);
    FRESULT (*f_unlink)(const TCHAR *path);
    FRESULT (*f_rename)(const TCHAR *old_name, const TCHAR *new_name);

    /* the UI's look (2.0) */
    int (*gfx_text_height)(int font, int scale);
    void (*gfx_round)(int x, int y, int w, int h, int r, uint16_t c);
    void (*gfx_round_alpha)(int x, int y, int w, int h, int r, uint16_t c, int alpha);
    void (*gfx_shade)(int x, int y, int w, int h, uint16_t c, int alpha);
    void (*gfx_icon)(int x, int y, const icon_t *icon, uint16_t c);
    void (*gfx_bar)(int x, int y, int w, int h, int value, int max, uint16_t c);
    void (*gfx_switch)(int x, int y, bool on);
};

/* ---- the OS side (os/loader) -------------------------------------------- */
typedef struct app app_t;

extern const os_api_t os_api;

/* Load and start an app; NULL on success, else why it failed. */
const char *app_open(const char *path);
/* Read only the header (for listings); 0, or -1 when it is not an app. */
int app_peek(const char *path, app_file_t *out);
/* Unload the apps that have no screen on the stack (the UI loop calls it). */
void app_collect(screen_t *const *stack, int depth);
int app_count(void);

/* The app whose code is running (the UI sets it around callbacks). */
extern app_t *app_current;
void app_forget(app_t *a, void *p);     /* p was freed by the UI */

screen_t *app_apps(void);               /* the Apps menu entry */

#endif
