/* B310E OS — home screen and main menu.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/app.h"
#include "os/fs.h"
#include "os/lib.h"
#include "os/rtc.h"
#include "os/status.h"
#include "os/ui.h"

/* ---- home ------------------------------------------------------------------ */

static void home_draw(screen_t *s)
{
    char buf[32];
    int mv = (int)os_status.battery_mv, pct = battery_percent(mv);
    const fs_info_t *fi = fs_info();
    rtc_datetime_t t;
    bool date_ok = rtc_get(&t) == 0;

    snprintf(buf, sizeof(buf), "%02d:%02d", t.hour, t.min);
    gfx_text_centered(UI_TOP + 8, buf, C_TEXT, FONT_LARGE, 3);
    if (date_ok)
        snprintf(buf, sizeof(buf), "%s %02d.%02d.%04d", rtc_weekday_name(t.wday),
                 t.day, t.month, t.year);
    else
        snprintf(buf, sizeof(buf), "date not set");
    gfx_text_centered(UI_TOP + 60, buf, C_DIM, FONT_SMALL, 1);

    if (charger_present())
        snprintf(buf, sizeof(buf), "charging");
    else if (pct >= 0)
        snprintf(buf, sizeof(buf), "battery %d%%  %d.%02d V", pct, mv / 1000,
                 mv % 1000 / 10);
    else
        snprintf(buf, sizeof(buf), "battery --");
    gfx_text_centered(UI_TOP + 84, buf, C_DIM, FONT_SMALL, 1);
    if (fi->mounted && fi->free_known)
        snprintf(buf, sizeof(buf), "card %lu MB free",
                 (unsigned long)(fi->free_kib / 1024));
    else if (fi->mounted)
        snprintf(buf, sizeof(buf), "card: counting free space");
    else
        snprintf(buf, sizeof(buf), "no card");
    gfx_text_centered(UI_TOP + 96, buf, C_DIM, FONT_SMALL, 1);
}

static void home_key(screen_t *s, const key_event_t *ev)
{
    if (ev->code == KEY_CENTER || ev->code == KEY_LSOFT)
        ui_push(app_menu());
}

static screen_t home = {
    .title = "B310E OS", .left = "Menu", .center = NULL, .right = NULL,
    .draw = home_draw, .key = home_key,
};

screen_t *app_home(void)
{
    return &home;
}

/* ---- menu ------------------------------------------------------------------ */

static screen_t *files_root(void)
{
    return app_files("/");
}

static const struct {
    const char *name;
    screen_t *(*open)(void);
} entries[] = {
    { "Apps", app_apps },
    { "Files", files_root },
    { "Sound", app_sound },
    { "FM radio", app_radio },
    { "Tools", app_tools },
    { "System", app_system },
    { "Settings", app_settings },
    { "Power", app_power },
};

static void menu_item(void *ctx, int i, char *text, int tn, char *detail, int dn)
{
    snprintf(text, (size_t)tn, "%s", entries[i].name);
}

static list_t menu_list = { .count = ARRAY_SIZE(entries), .item = menu_item };

static void menu_enter(screen_t *s)
{
    menu_list.sel = menu_list.top = 0;      /* from home: start at the top */
}

static void menu_draw(screen_t *s)
{
    list_draw(&menu_list);
}

static void menu_key(screen_t *s, const key_event_t *ev)
{
    if (list_key(&menu_list, ev))
        return;
    if (ev->code == KEY_CENTER || ev->code == KEY_LSOFT) {
        ui_push(entries[menu_list.sel].open());
    } else if (ev->code == KEY_RSOFT) {
        ui_pop();
    }
}

static screen_t menu = {
    .title = "Menu", .left = "Open", .center = "OK", .right = "Back",
    .enter = menu_enter, .draw = menu_draw, .key = menu_key,
};

screen_t *app_menu(void)
{
    return &menu;
}
