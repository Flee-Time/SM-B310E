/* B310E OS — the Apps menu: /b310e-os/apps as a list, by name.
 *
 *   x.app                 an app
 *   Doom/doom.app         a folder with an .app of its own name: an app, with
 *                         its data beside it (its case does not matter)
 *   Emulators/            any other folder: a category, opening on a list
 *                         like this one, as deep as the paths reach
 *
 * A folder may say more in a text file of its own, .info, each line optional:
 *
 *   name Retro games      what the list calls it
 *   app infones.app       this folder is that app (whatever the file's name)
 *   icon 00ffff00ffff     12 of these, top to bottom, 12 pixels each, 0 to f:
 *   ...                   its icon (sdk/mkicon.py makes them from a picture)
 *
 * Categories first, then the apps, each by name. A folder or file whose name
 * starts with a dot is not listed.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/app.h"
#include "os/fs.h"
#include "os/kernel.h"
#include "os/lib.h"
#include "os/ui.h"

#define MAX_APPS 32
#define PATH_MAX_ 96
#define INFO_FILE ".info"

typedef struct {
    char path[PATH_MAX_];               /* the .app, or the category's folder */
    char name[24];
    uint32_t detail;                    /* an app: its KB; a category: what it holds */
    bool category, has_icon;
    icon_t icon;
} item_t;

typedef struct {
    screen_t s;                         /* (first: the screen is the level) */
    list_t list;
    item_t *items;
    char dir[PATH_MAX_], title[24];
} level_t;

static screen_t *level_new(const char *dir, const char *title);

static void item(void *ctx, int i, char *text, int tn, char *detail, int dn)
{
    const item_t *it = &((level_t *)ctx)->items[i];

    snprintf(text, (size_t)tn, "%s", it->name);
    if (it->category)
        snprintf(detail, (size_t)dn, "%lu", (unsigned long)it->detail);
    else
        snprintf(detail, (size_t)dn, "%lu KB", (unsigned long)it->detail);
}

static const icon_t *item_icon(void *ctx, int i)
{
    const item_t *it = &((level_t *)ctx)->items[i];

    return it->has_icon ? &it->icon : it->category ? &icon_folder : &icon_apps;
}

static bool is_app(const char *name)
{
    size_t n = strlen(name);

    return n > 4 && !strcmp(name + n - 4, ".app");
}

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static int by_name(const item_t *a, const item_t *b)
{
    const char *p = a->name, *q = b->name;

    if (a->category != b->category)
        return a->category ? -1 : 1;
    while (*p && lower(*p) == lower(*q))
        p++, q++;
    return lower(*p) - lower(*q);
}

/* Adds it to the level, in its place by name; false if it does not fit. */
static item_t *new_item(level_t *l)
{
    return l->list.count < MAX_APPS ? &l->items[l->list.count] : NULL;
}

static void keep(level_t *l)
{
    item_t it = l->items[l->list.count];
    int i = l->list.count++;

    for (; i > 0 && by_name(&l->items[i - 1], &it) > 0; i--)
        l->items[i] = l->items[i - 1];
    l->items[i] = it;
}

/* A folder's .info, if it has one */
typedef struct {
    char name[24], app[40];
    bool has_icon;
    icon_t icon;
} info_t;

static int hex(int c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 :
           c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

static bool starts(const char *s, const char *with)
{
    while (*with)
        if (*s++ != *with++)
            return false;
    return true;
}

static void read_info(const char *folder, info_t *in)
{
    char path[PATH_MAX_ + 8], text[480], *line, *next;
    FIL f;
    UINT n = 0;
    int rows = 0;

    memset(in, 0, sizeof(*in));
    snprintf(path, sizeof(path), "%s/" INFO_FILE, folder);
    if (f_open(&f, path, FA_READ) != FR_OK)
        return;
    f_read(&f, text, sizeof(text) - 1, &n);
    f_close(&f);
    text[n] = 0;
    for (line = text; *line; line = next) {
        char *nl = strchr(line, '\n');
        size_t len;

        next = nl ? nl + 1 : line + strlen(line);
        if (nl)
            *nl = 0;
        len = strlen(line);
        if (len && line[len - 1] == '\r')
            line[--len] = 0;
        if (starts(line, "name ")) {
            snprintf(in->name, sizeof(in->name), "%s", line + 5);
        } else if (starts(line, "app ")) {
            snprintf(in->app, sizeof(in->app), "%s", line + 4);
        } else if (starts(line, "icon ") && len == 17 && rows < 12) {
            bool ok = true;

            for (int x = 0; x < 12; x++) {
                int v = hex(line[5 + x]);

                if (v < 0) {
                    ok = false;
                    break;
                }
                if (x & 1)
                    in->icon.a[rows * 6 + x / 2] |= (uint8_t)v;
                else
                    in->icon.a[rows * 6 + x / 2] = (uint8_t)(v << 4);
            }
            if (ok)
                rows++;
        }
    }
    in->has_icon = rows == 12;
}

static void add_app(level_t *l, const char *path, const char *fallback, const info_t *in)
{
    item_t *it = new_item(l);
    app_file_t h;

    if (!it || app_peek(path, &h))
        return;
    snprintf(it->path, sizeof(it->path), "%s", path);
    snprintf(it->name, sizeof(it->name), "%s",
             in && in->name[0] ? in->name : h.name[0] ? h.name : fallback);
    it->category = false;
    it->detail = (h.image_size + h.bss_size + 1023) / 1024;
    it->has_icon = in && in->has_icon;
    if (it->has_icon)
        it->icon = in->icon;
    else
        it->has_icon = app_peek_icon(path, &h, &it->icon);
    keep(l);
}

/* What a category shows: its apps and categories, one level, by their names */
static uint32_t count_in(const char *dir)
{
    DIR d;
    FILINFO fi;
    uint32_t n = 0;

    if (f_opendir(&d, dir) != FR_OK)
        return 0;
    while (f_readdir(&d, &fi) == FR_OK && fi.fname[0])
        if (fi.fname[0] != '.' && (fi.fattrib & AM_DIR || is_app(fi.fname)))
            n++;
    f_closedir(&d);
    return n;
}

static void add_category(level_t *l, const char *path, const char *name, const info_t *in)
{
    item_t *it = new_item(l);

    if (!it)
        return;
    snprintf(it->path, sizeof(it->path), "%s", path);
    snprintf(it->name, sizeof(it->name), "%s", in->name[0] ? in->name : name);
    it->category = true;
    it->detail = count_in(path);
    it->has_icon = in->has_icon;
    if (it->has_icon)
        it->icon = in->icon;
    keep(l);
}

static void scan(level_t *l)
{
    DIR d;
    FILINFO fi;
    char path[PATH_MAX_], own[PATH_MAX_];
    info_t in;

    l->list.count = l->list.sel = l->list.top = 0;
    if (f_opendir(&d, l->dir) != FR_OK)
        return;
    while (f_readdir(&d, &fi) == FR_OK && fi.fname[0]) {
        FILINFO st;

        if (fi.fname[0] == '.' ||
            snprintf(path, sizeof(path), "%s/%s", l->dir, fi.fname) >= (int)sizeof(path))
            continue;
        if (!(fi.fattrib & AM_DIR)) {
            if (is_app(fi.fname))
                add_app(l, path, fi.fname, NULL);
            continue;
        }
        read_info(path, &in);
        if (in.app[0])
            snprintf(own, sizeof(own), "%s/%s", path, in.app);
        else
            snprintf(own, sizeof(own), "%s/%s.app", path, fi.fname);
        if (f_stat(own, &st) == FR_OK && !(st.fattrib & AM_DIR))
            add_app(l, own, fi.fname, &in);     /* its own folder */
        else
            add_category(l, path, fi.fname, &in);
    }
    f_closedir(&d);
}

static void enter(screen_t *s)
{
    level_t *l = (level_t *)s;

    if (!l->items)
        l->items = kmalloc(MAX_APPS * sizeof(item_t));
    if (l->items)
        scan(l);
}

static void leave(screen_t *s)
{
    level_t *l = (level_t *)s;

    kfree(l->items);
    l->items = NULL;
    l->list.count = 0;
}

static void draw(screen_t *s)
{
    level_t *l = (level_t *)s;

    if (l->list.count)
        list_draw(&l->list);
    else
        gfx_text_centered(UI_TOP + 40, "no apps here", C_DIM, FONT_SMALL, 1);
}

static void key(screen_t *s, const key_event_t *ev)
{
    level_t *l = (level_t *)s;
    const char *err;

    if (list_key(&l->list, ev))
        return;
    if ((ev->code == KEY_CENTER || ev->code == KEY_LSOFT) && l->list.count) {
        const item_t *it = &l->items[l->list.sel];

        if (it->category)
            ui_push(level_new(it->path, it->name));
        else if ((err = app_open(it->path)))
            ui_message(it->name, err);
    } else if (ev->code == KEY_RSOFT) {
        ui_pop();
    }
}

static screen_t *level_new(const char *dir, const char *title)
{
    level_t *l = kzalloc(sizeof(*l));

    if (!l)
        return NULL;
    snprintf(l->dir, sizeof(l->dir), "%s", dir);
    snprintf(l->title, sizeof(l->title), "%s", title);
    l->s.title = l->title;
    l->s.left = "Open";
    l->s.center = "OK";
    l->s.right = "Back";
    l->s.enter = enter;
    l->s.leave = leave;
    l->s.draw = draw;
    l->s.key = key;
    l->s.free_on_pop = true;
    l->list.item = item;
    l->list.icon = item_icon;
    l->list.ctx = l;
    return &l->s;
}

screen_t *app_apps(void)
{
    return level_new(APP_DIR, "Apps");
}
