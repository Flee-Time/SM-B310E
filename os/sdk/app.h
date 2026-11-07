/* B310E OS app SDK. An app includes this one header and declares itself:
 *
 *     #include "app.h"
 *
 *     static screen_t *start(void) { ...; return &my_screen; }
 *     APP("Stopwatch", start, NULL);
 *
 * and then uses the OS's names as usual (gfx_text, ui_push, f_open,
 * snprintf ...): each is a macro for the pointer in the API table the OS
 * passed to the app (os/app.h). Build with `make` in os/ (every folder in
 * os/modules/ becomes build/os/modules/<folder>.app).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef OS_SDK_APP_H
#define OS_SDK_APP_H

#include "os/app.h"

extern const os_api_t *os;

#define APP(name_, start_, stop_)                                           \
    static screen_t *app_start_(const os_api_t *api)                        \
    {                                                                       \
        os = api;                                                           \
        return start_();                                                    \
    }                                                                       \
    const app_info_t app_info                                               \
        __attribute__((section(".app_info"), used)) = {                     \
        OS_API_MAJOR, OS_API_MINOR, name_, app_start_, stop_ }

/* memcpy, memset, memmove: in the SDK (sdk/start.c) */
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);

#define memcmp            (os->memcmp)
#define strlen            (os->strlen)
#define strcmp            (os->strcmp)
#define strncpy           (os->strncpy)
#define strchr            (os->strchr)
#define snprintf          (os->snprintf)
#define vsnprintf         (os->vsnprintf)
#define malloc            (os->malloc)
#define zalloc            (os->zalloc)
#define free              (os->free)
#define ticks             (os->ticks)
#define thread_sleep      (os->sleep)
#define klog              (os->klog)
#define rtc_time          (os->rtc_time)
#define battery_mv        (os->battery_mv)
#define battery_percent   (os->battery_percent)
#define charger_present   (os->charger_present)
#define gfx_fill          (os->gfx_fill)
#define gfx_hline         (os->gfx_hline)
#define gfx_rect          (os->gfx_rect)
#define gfx_text          (os->gfx_text)
#define gfx_text_width    (os->gfx_text_width)
#define gfx_text_centered (os->gfx_text_centered)
#define ui_push           (os->ui_push)
#define ui_pop            (os->ui_pop)
#define ui_home           (os->ui_home)
#define ui_redraw         (os->ui_redraw)
#define list_draw         (os->list_draw)
#define list_key          (os->list_key)
#define ui_confirm        (os->ui_confirm)
#define ui_message        (os->ui_message)
#define ui_text_view      (os->ui_text_view)
#define ui_settings       (*os->settings)
#define f_open            (os->f_open)
#define f_close           (os->f_close)
#define f_read            (os->f_read)
#define f_write           (os->f_write)
#define f_lseek           (os->f_lseek)
#define f_sync            (os->f_sync)
#define f_opendir         (os->f_opendir)
#define f_closedir        (os->f_closedir)
#define f_readdir         (os->f_readdir)
#define f_stat            (os->f_stat)
#define f_mkdir           (os->f_mkdir)
#define f_unlink          (os->f_unlink)
#define f_rename          (os->f_rename)
#define gfx_text_height   (os->gfx_text_height)
#define gfx_round         (os->gfx_round)
#define gfx_round_alpha   (os->gfx_round_alpha)
#define gfx_shade         (os->gfx_shade)
#define gfx_icon          (os->gfx_icon)
#define gfx_bar           (os->gfx_bar)
#define gfx_switch        (os->gfx_switch)

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

#endif
