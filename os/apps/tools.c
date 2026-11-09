/* B310E OS — Tools: torch, SIM cards, IMEI, USB (storage or network), DSP boot,
 * radio status. DSP boot is a developer check: it shows only when
 * Settings -> Developer -> Developer mode is on.
 * (Calculator, Notes, Alarm and the rest are apps since 2026-11-07; the
 * vibrator is Settings -> Sound & touch's tactile feedback.)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/drivers.h"
#include "os/dsp.h"
#include "os/kernel.h"
#include "os/lib.h"
#include "os/ui.h"
#include "os/usb.h"

/* DSP boot (T_DSP) and Register (T_REGISTER) show only in developer mode: they
 * are two contiguous slots, so tool_at() skips both when dev mode is off and the
 * other entries keep their order. Register transmits, so it stays developer-gated. */
enum { T_TORCH, T_SIM, T_IMEI, T_USB, T_DSP, T_REGISTER, T_RADIO, T_SLEEP, T_COUNT };
#define T_DEV_ONLY 2                /* T_DSP, T_REGISTER */

static int tool_at(int visible)
{
    return !ui_settings.dev_mode && visible >= T_DSP ? visible + T_DEV_ONLY : visible;
}

screen_t *app_dspboot(void);        /* apps/dspboot.c */
screen_t *app_register(void);       /* apps/register.c */
screen_t *app_radio_status(void);   /* apps/gsmstatus.c */
void app_sleep_test(void);          /* apps/sleeptest.c */

static list_t list;

char tools_imei_shown[16];          /* on the screen only (QEMU test: test-imei.py) */

static void show_imei(void)
{
    ui_message("IMEI", nv_imei(tools_imei_shown) ? tools_imei_shown : "No valid IMEI in NOR.");
}

static void item(void *ctx, int i, char *text, int tn, char *detail, int dn)
{
    switch (tool_at(i)) {
    case T_TORCH:
        snprintf(text, (size_t)tn, "Torch");
        break;
    case T_SIM:
        snprintf(text, (size_t)tn, "SIM cards");
        break;
    case T_IMEI:
        snprintf(text, (size_t)tn, "IMEI");
        break;
    case T_USB:
        snprintf(text, (size_t)tn, "USB");
        break;
    case T_DSP:
        snprintf(text, (size_t)tn, "DSP boot");
        break;
    case T_REGISTER:
        snprintf(text, (size_t)tn, "Register");
        break;
    case T_RADIO:
        snprintf(text, (size_t)tn, "Radio status");
        break;
    case T_SLEEP:
        snprintf(text, (size_t)tn, "Sleep test");
        break;
    }
}

static const icon_t *tool_icon(void *ctx, int i)
{
    static const icon_t *const icons[] = {
        &icon_torch, &icon_sim, &icon_sim, &icon_usb, &icon_cpu, &icon_sim,
        &icon_chart, &icon_power,
    };

    return icons[tool_at(i)];
}

static int tool_toggle(void *ctx, int i)
{
    return tool_at(i) == T_TORCH ? torch_get() : -1;
}

static void draw(screen_t *s)
{
    list_draw(&list);
    if (list.count <= 5)                /* below the rows, when they leave room */
        gfx_text_centered(UI_BOTTOM - 12, "hold 0 anywhere: torch", C_DIM, FONT_SMALL, 1);
}

static void key(screen_t *s, const key_event_t *ev)
{
    if (list_key(&list, ev))
        return;
    if (ev->code == KEY_CENTER || ev->code == KEY_LSOFT) {
        switch (tool_at(list.sel)) {
        case T_TORCH:
            torch_set(!torch_get());
            break;
        case T_SIM:
            ui_push(app_sim());
            break;
        case T_IMEI:
            show_imei();
            break;
        case T_USB:
            ui_push(app_usb_menu());
            break;
        case T_DSP:
            ui_push(app_dspboot());
            break;
        case T_REGISTER:
            ui_push(app_register());
            break;
        case T_RADIO:
            ui_push(app_radio_status());
            break;
        case T_SLEEP:
            app_sleep_test();
            break;
        }
    } else if (ev->code == KEY_RSOFT) {
        ui_pop();
    }
}

static screen_t screen = {
    .title = "Tools", .left = "Select", .center = "OK", .right = "Back",
    .draw = draw, .key = key,
};

screen_t *app_tools(void)
{
    /* opened anew: from the top (back from a screen it opened, it stays) */
    list.sel = list.top = 0;
    list.count = ui_settings.dev_mode ? T_COUNT : T_COUNT - T_DEV_ONLY;   /* DSP boot, Register: dev only */
    list.item = item;
    list.icon = tool_icon;
    list.toggle = tool_toggle;
    return &screen;
}
