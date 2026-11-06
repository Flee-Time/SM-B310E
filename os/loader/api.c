/* B310E OS — the API table apps get (os/app.h). Version 1.0: append new
 * members at the end and raise OS_API_MINOR; anything else is a new major.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/app.h"
#include "os/drivers.h"
#include "os/fs.h"
#include "os/kernel.h"
#include "os/lib.h"

void *app_malloc(size_t size);
void *app_zalloc(size_t size);
void app_free(void *p);

const os_api_t os_api = {
    .major = OS_API_MAJOR, .minor = OS_API_MINOR, .size = sizeof(os_api_t),

    .memcmp = memcmp, .strlen = strlen, .strcmp = strcmp, .strncpy = strncpy,
    .strchr = strchr, .snprintf = snprintf, .vsnprintf = vsnprintf,

    .malloc = app_malloc, .zalloc = app_zalloc, .free = app_free,

    .ticks = kernel_ticks, .sleep = thread_sleep, .klog = klog,
    .rtc_time = rtc_time, .battery_mv = battery_mv,
    .battery_percent = battery_percent, .charger_present = charger_present,

    .gfx_fill = gfx_fill, .gfx_hline = gfx_hline, .gfx_rect = gfx_rect,
    .gfx_text = gfx_text, .gfx_text_width = gfx_text_width,
    .gfx_text_centered = gfx_text_centered,

    .ui_push = ui_push, .ui_pop = ui_pop, .ui_home = ui_home,
    .ui_redraw = ui_redraw, .list_draw = list_draw, .list_key = list_key,
    .ui_confirm = ui_confirm, .ui_message = ui_message,
    .ui_text_view = ui_text_view, .settings = &ui_settings,

    .f_open = f_open, .f_close = f_close, .f_read = f_read,
    .f_write = f_write, .f_lseek = f_lseek, .f_sync = f_sync,
    .f_opendir = f_opendir, .f_closedir = f_closedir, .f_readdir = f_readdir,
    .f_stat = f_stat, .f_mkdir = f_mkdir, .f_unlink = f_unlink,
    .f_rename = f_rename,
};
