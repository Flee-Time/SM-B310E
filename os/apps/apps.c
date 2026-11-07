/* B310E OS — the Apps menu: every .app in /b310e-os/apps, by its name.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/app.h"
#include "os/fs.h"
#include "os/kernel.h"
#include "os/lib.h"
#include "os/ui.h"

#define MAX_APPS 32

typedef struct {
    char file[48];                      /* name in APP_DIR */
    char name[20];
    uint32_t kib;
} item_t;

static item_t *items;
static list_t list;

static void item(void *ctx, int i, char *text, int tn, char *detail, int dn)
{
    snprintf(text, (size_t)tn, "%s", items[i].name);
    snprintf(detail, (size_t)dn, "%lu KB", (unsigned long)items[i].kib);
}

static bool is_app(const char *name)
{
    size_t n = strlen(name);

    return n > 4 && !strcmp(name + n - 4, ".app");
}

static void scan(void)
{
    DIR d;
    FILINFO fi;
    char path[64];
    app_file_t h;

    list.count = list.sel = list.top = 0;
    if (f_opendir(&d, APP_DIR) != FR_OK)
        return;
    while (list.count < MAX_APPS && f_readdir(&d, &fi) == FR_OK && fi.fname[0]) {
        if (fi.fattrib & AM_DIR || !is_app(fi.fname) || strlen(fi.fname) >= sizeof(items->file))
            continue;
        snprintf(path, sizeof(path), APP_DIR "/%s", fi.fname);
        if (app_peek(path, &h))
            continue;
        item_t *it = &items[list.count++];
        snprintf(it->file, sizeof(it->file), "%s", fi.fname);
        snprintf(it->name, sizeof(it->name), "%s", h.name[0] ? h.name : fi.fname);
        it->kib = (h.image_size + h.bss_size + 1023) / 1024;
    }
    f_closedir(&d);
}

static void enter(screen_t *s)
{
    if (!items)
        items = kmalloc(MAX_APPS * sizeof(item_t));
    if (items)
        scan();
}

static void leave(screen_t *s)
{
    kfree(items);
    items = NULL;
    list.count = 0;
}

static void draw(screen_t *s)
{
    if (list.count)
        list_draw(&list);
    else
        gfx_text_centered(UI_TOP + 40, "no apps in " APP_DIR, C_DIM, FONT_SMALL, 1);
}

static void key(screen_t *s, const key_event_t *ev)
{
    char path[64];
    const char *err;

    if (list_key(&list, ev))
        return;
    if ((ev->code == KEY_CENTER || ev->code == KEY_LSOFT) && list.count) {
        snprintf(path, sizeof(path), APP_DIR "/%s", items[list.sel].file);
        if ((err = app_open(path)))
            ui_message(items[list.sel].name, err);
    } else if (ev->code == KEY_RSOFT) {
        ui_pop();
    }
}

static screen_t screen = {
    .title = "Apps", .left = "Open", .center = "OK", .right = "Back",
    .enter = enter, .leave = leave, .draw = draw, .key = key,
};

static const icon_t *app_icon(void *ctx, int i)
{
    return &icon_apps;
}

screen_t *app_apps(void)
{
    list.item = item;
    list.icon = app_icon;
    return &screen;
}
