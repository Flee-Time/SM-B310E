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

/* A small card: an icon, a label, a value at the right, a bar under them. */
static void card(int y, const icon_t *icon, const char *label, const char *value,
                 int level, int max, uint16_t bar)
{
    int x = UI_PAD, w = LCD_WIDTH - 2 * UI_PAD;

    gfx_round(x, y, w, 24, UI_RADIUS, C_CARD);
    gfx_icon(x + 4, y + 3, icon, C_ACCENT);
    gfx_text(x + 20, y + 5, label, C_TEXT, FONT_BODY, 1, 0);
    gfx_text(x + w - 4 - gfx_text_width(value, FONT_SMALL, 1), y + 6, value, C_DIM,
             FONT_SMALL, 1, 0);
    if (max > 0)
        gfx_bar(x + 4, y + 18, w - 8, 3, level, max, bar);
}

static void home_draw(screen_t *s)
{
    char buf[32];
    int mv = (int)os_status.battery_mv, pct = battery_percent(mv);
    const fs_info_t *fi = fs_info();
    rtc_datetime_t t;
    bool date_ok = rtc_get(&t) == 0;

    snprintf(buf, sizeof(buf), "%02d:%02d", t.hour, t.min);
    gfx_text_centered(UI_TOP + 8, buf, C_TEXT, FONT_HUGE, 1);
    if (date_ok)
        snprintf(buf, sizeof(buf), "%s %02d.%02d.%04d", rtc_weekday_name(t.wday),
                 t.day, t.month, t.year);
    else
        snprintf(buf, sizeof(buf), "date not set");
    gfx_text_centered(UI_TOP + 40, buf, C_DIM, FONT_BODY, 1);

    if (charge_state() == CHARGE_DONE)
        snprintf(buf, sizeof(buf), "charged");
    else if (charge_state() == CHARGE_CHARGING)
        snprintf(buf, sizeof(buf), "charging");
    else if (pct >= 0)
        snprintf(buf, sizeof(buf), "%d%%  %d.%02d V", pct, mv / 1000, mv % 1000 / 10);
    else
        snprintf(buf, sizeof(buf), "--");
    card(UI_TOP + 60, &icon_power, "Battery", buf, pct < 0 ? 0 : pct, 100,
         pct >= 0 && pct <= 15 ? C_BAD : C_OK);

    if (fi->mounted && fi->free_known) {
        uint32_t total = fi->sectors / 2048, free_mb = fi->free_kib / 1024;

        snprintf(buf, sizeof(buf), "%lu MB free", (unsigned long)free_mb);
        card(UI_TOP + 88, &icon_folder, "Card", buf, (int)(total - free_mb), (int)total,
             C_ACCENT);
    } else {
        card(UI_TOP + 88, &icon_folder, "Card", fi->mounted ? "counting" : "none", 0, 0, 0);
    }
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
    const icon_t *icon;
} entries[] = {
    { "Apps", app_apps, &icon_apps },
    { "Files", files_root, &icon_folder },
    { "Sound", app_sound, &icon_music },
    { "FM radio", app_radio, &icon_radio },
    { "Tools", app_tools, &icon_tools },
    { "System", app_system, &icon_chart },
    { "Settings", app_settings, &icon_gear },
    { "Power", app_power, &icon_power },
};

static void menu_item(void *ctx, int i, char *text, int tn, char *detail, int dn)
{
    snprintf(text, (size_t)tn, "%s", entries[i].name);
}

static const icon_t *menu_icon(void *ctx, int i)
{
    return entries[i].icon;
}

static list_t menu_list = {
    .count = ARRAY_SIZE(entries), .item = menu_item, .icon = menu_icon,
};

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
