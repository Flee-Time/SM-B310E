/* B310E OS — settings (brightness, screen timeout, CPU speed, about) and power.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/kernel.h"
#include "os/lib.h"
#include "os/ui.h"
#include "build_info.h"

/* ---- settings --------------------------------------------------------------- */

static const int brightness_steps[] = { 20, 40, 60, 80, 100 };
static const int timeout_steps[] = { 15, 30, 60, 120, 0 };

static int next_of(const int *steps, int n, int value)
{
    for (int i = 0; i < n; i++)
        if (steps[i] == value)
            return steps[(i + 1) % n];
    return steps[0];
}

static void set_item(void *ctx, int i, char *text, int tn, char *detail, int dn)
{
    switch (i) {
    case 0:
        snprintf(text, (size_t)tn, "Brightness");
        snprintf(detail, (size_t)dn, "%d%%", ui_settings.brightness);
        break;
    case 1:
        snprintf(text, (size_t)tn, "Screen off");
        if (ui_settings.screen_timeout_s)
            snprintf(detail, (size_t)dn, "%d s", ui_settings.screen_timeout_s);
        else
            snprintf(detail, (size_t)dn, "never");
        break;
    case 2:
        snprintf(text, (size_t)tn, "CPU speed");
        snprintf(detail, (size_t)dn, "%s", ui_settings.cpu_max ? "208 MHz" : "Auto");
        break;
    case 3:
        snprintf(text, (size_t)tn, "Date & time");
        break;
    case 4:
        snprintf(text, (size_t)tn, "About");
        break;
    }
}

static const icon_t *set_icon(void *ctx, int i)
{
    static const icon_t *const icons[] = {
        &icon_sun, &icon_screen, &icon_cpu, &icon_clock, &icon_info,
    };

    return icons[i];
}

static list_t set_list = { .count = 5, .item = set_item, .icon = set_icon };

static void set_draw(screen_t *s)
{
    list_draw(&set_list);
}

static void set_key(screen_t *s, const key_event_t *ev)
{
    if (list_key(&set_list, ev))
        return;
    if (ev->code == KEY_CENTER || ev->code == KEY_LSOFT) {
        switch (set_list.sel) {
        case 0:
            ui_settings.brightness = next_of(brightness_steps,
                ARRAY_SIZE(brightness_steps), ui_settings.brightness);
            ui_apply_brightness();
            ui_settings_save();
            break;
        case 1:
            ui_settings.screen_timeout_s = next_of(timeout_steps,
                ARRAY_SIZE(timeout_steps), ui_settings.screen_timeout_s);
            ui_settings_save();
            break;
        case 2:
            ui_settings.cpu_max = !ui_settings.cpu_max;
            cpufreq_set_mode(ui_settings.cpu_max ? CPUFREQ_MAX : CPUFREQ_AUTO);
            ui_settings_save();
            break;
        case 3:
            ui_push(app_clock());
            break;
        case 4:
            ui_message("About", "B310E OS 0.3\n\nbuilt " BUILD_DATE
                       "\n\nGPLv2 or later\nFatFs (C) ChaN");
            break;
        }
    } else if (ev->code == KEY_RSOFT) {
        ui_pop();
    }
}

static screen_t settings = {
    .title = "Settings", .left = "Edit", .center = "OK", .right = "Back",
    .draw = set_draw, .key = set_key,
};

screen_t *app_settings(void)
{
    return &settings;
}

/* ---- power ------------------------------------------------------------------ */

static void power_item(void *ctx, int i, char *text, int tn, char *detail, int dn)
{
    snprintf(text, (size_t)tn, "%s", i == 0 ? "Power off" : "Restart to menu");
}

static const icon_t *power_icon(void *ctx, int i)
{
    return i == 0 ? &icon_power : &icon_restart;
}

static list_t power_list = { .count = 2, .item = power_item, .icon = power_icon };

static void power_draw(screen_t *s)
{
    list_draw(&power_list);
}

static void power_key(screen_t *s, const key_event_t *ev)
{
    if (list_key(&power_list, ev))
        return;
    if (ev->code == KEY_CENTER || ev->code == KEY_LSOFT) {
        if (power_list.sel == 0)
            ui_confirm("Power off", "Power off the phone?", ui_power_off);
        else
            ui_confirm("Restart", "Restart to the boot menu?", ui_reboot);
    } else if (ev->code == KEY_RSOFT) {
        ui_pop();
    }
}

static screen_t power = {
    .title = "Power", .left = "Select", .center = "OK", .right = "Back",
    .draw = power_draw, .key = power_key,
};

screen_t *app_power(void)
{
    return &power;
}
