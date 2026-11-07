/* B310E OS — Tools: torch, SIM cards, USB (storage or network), DSP boot.
 * (Calculator, Notes, Alarm and the rest are apps since 2026-11-07; the
 * vibrator is Settings -> Sound & touch's tactile feedback.)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/drivers.h"
#include "os/kernel.h"
#include "os/lib.h"
#include "os/ui.h"
#include "os/usb.h"

enum { T_TORCH, T_SIM, T_USB, T_DSP, T_COUNT };

screen_t *app_dspboot(void);        /* apps/dspboot.c */

static list_t list;

static void item(void *ctx, int i, char *text, int tn, char *detail, int dn)
{
    switch (i) {
    case T_TORCH:
        snprintf(text, (size_t)tn, "Torch");
        break;
    case T_SIM:
        snprintf(text, (size_t)tn, "SIM cards");
        break;
    case T_USB:
        snprintf(text, (size_t)tn, "USB");
        break;
    case T_DSP:
        snprintf(text, (size_t)tn, "DSP boot");
        break;
    }
}

static const icon_t *tool_icon(void *ctx, int i)
{
    static const icon_t *const icons[] = {
        &icon_torch, &icon_sim, &icon_usb, &icon_cpu,
    };

    return icons[i];
}

static int tool_toggle(void *ctx, int i)
{
    return i == T_TORCH ? torch_get() : -1;
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
        if (list.sel == T_TORCH) {
            torch_set(!torch_get());
        } else if (list.sel == T_SIM) {
            ui_push(app_sim());
        } else if (list.sel == T_USB) {
            ui_push(app_usb_menu());
        } else if (list.sel == T_DSP) {
            ui_push(app_dspboot());
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
    list.count = T_COUNT;
    list.item = item;
    list.icon = tool_icon;
    list.toggle = tool_toggle;
    return &screen;
}
