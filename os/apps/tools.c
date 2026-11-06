/* B310E OS — Tools: torch, vibrator test, SIM cards, USB storage.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/drivers.h"
#include "os/kernel.h"
#include "os/lib.h"
#include "os/ui.h"
#include "os/usb.h"

enum { T_TORCH, T_VIBRATE, T_SIM, T_USB, T_COUNT };

static list_t list;
static uint32_t buzz_until;

static void item(void *ctx, int i, char *text, int tn, char *detail, int dn)
{
    switch (i) {
    case T_TORCH:
        snprintf(text, (size_t)tn, "Torch");
        snprintf(detail, (size_t)dn, "%s", torch_get() ? "on" : "off");
        break;
    case T_VIBRATE:
        snprintf(text, (size_t)tn, "Vibrate");
        snprintf(detail, (size_t)dn, "%s", buzz_until ? "on" : "");
        break;
    case T_SIM:
        snprintf(text, (size_t)tn, "SIM cards");
        break;
    case T_USB:
        snprintf(text, (size_t)tn, "USB storage");
        break;
    }
}

static void draw(screen_t *s)
{
    list_draw(&list);
    gfx_text(2, UI_BOTTOM - 10, "hold 0 anywhere: torch", C_DIM, FONT_SMALL, 1, 0);
}

static void tick(screen_t *s)
{
    if (buzz_until && (int32_t)(kernel_ticks() - buzz_until) >= 0) {
        buzz_until = 0;
        vibrator_set(0);
    }
}

static void key(screen_t *s, const key_event_t *ev)
{
    if (list_key(&list, ev))
        return;
    if (ev->code == KEY_CENTER || ev->code == KEY_LSOFT) {
        if (list.sel == T_TORCH) {
            torch_set(!torch_get());
        } else if (list.sel == T_VIBRATE) {
            vibrator_set(5);
            buzz_until = kernel_ticks() + 500;
        } else if (list.sel == T_SIM) {
            ui_push(app_sim());
        } else if (list.sel == T_USB) {
            ui_push(app_usb());
        }
    } else if (ev->code == KEY_RSOFT) {
        ui_pop();
    }
}

static void leave(screen_t *s)
{
    if (buzz_until) {
        buzz_until = 0;
        vibrator_set(0);
    }
}

static screen_t screen = {
    .title = "Tools", .left = "Select", .center = "OK", .right = "Back",
    .draw = draw, .key = key, .tick = tick, .leave = leave, .tick_ms = 100,
};

screen_t *app_tools(void)
{
    list.count = T_COUNT;
    list.item = item;
    return &screen;
}
