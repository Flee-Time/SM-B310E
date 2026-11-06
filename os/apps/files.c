/* B310E OS — file browser: folders first, then files with their sizes;
 * Center opens a folder, shows a text file's start, or a file's details.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/fs.h"
#include "os/kernel.h"
#include "os/lib.h"
#include "os/ui.h"

#define MAX_ENTRIES  256
#define VIEW_BYTES   2048

typedef struct {
    char *name;
    uint32_t size;
    bool dir;
} entry_t;

typedef struct {
    screen_t s;
    list_t list;
    entry_t *entries;
    char *names;                /* one block for every name */
    FRESULT error;
    char path[];                /* the folder; also the title */
} browser_t;

static void size_text(uint32_t size, char *out, int n)
{
    if (size < 1024)
        snprintf(out, (size_t)n, "%luB", (unsigned long)size);
    else if (size < 1024u * 1024)
        snprintf(out, (size_t)n, "%luK", (unsigned long)(size / 1024));
    else
        snprintf(out, (size_t)n, "%luM", (unsigned long)(size / (1024u * 1024)));
}

static void item(void *ctx, int i, char *text, int tn, char *detail, int dn)
{
    browser_t *b = ctx;
    entry_t *e = &b->entries[i];

    snprintf(text, (size_t)tn, "%s%s", e->name, e->dir ? "/" : "");
    if (!e->dir)
        size_text(e->size, detail, dn);
}

/* two passes: folders, then files, in directory order */
static void load(browser_t *b)
{
    DIR dir;
    FILINFO fi;
    size_t used = 0, cap = 4096;
    int n = 0;

    b->entries = kzalloc(MAX_ENTRIES * sizeof(entry_t));
    b->names = kmalloc(cap);
    if (!b->entries || !b->names) {
        b->error = FR_NOT_ENOUGH_CORE;
        return;
    }
    for (int pass = 0; pass < 2; pass++) {
        b->error = f_opendir(&dir, b->path);
        if (b->error != FR_OK)
            return;
        while (n < MAX_ENTRIES && f_readdir(&dir, &fi) == FR_OK && fi.fname[0]) {
            bool is_dir = fi.fattrib & AM_DIR;
            size_t len = strlen(fi.fname) + 1;

            if (is_dir != (pass == 0) || (fi.fattrib & (AM_HID | AM_SYS)))
                continue;
            if (used + len > cap)
                break;
            memcpy(b->names + used, fi.fname, len);
            b->entries[n].name = b->names + used;
            b->entries[n].size = (uint32_t)fi.fsize;
            b->entries[n].dir = is_dir;
            used += len;
            n++;
        }
        f_closedir(&dir);
    }
    b->list.count = n;
}

static void enter(screen_t *s)
{
    browser_t *b = (browser_t *)s;

    if (!fs_info()->mounted) {
        b->error = FR_NOT_READY;
        return;
    }
    load(b);
}

static void leave(screen_t *s)
{
    browser_t *b = (browser_t *)s;

    kfree(b->entries);
    kfree(b->names);
}

static void draw(screen_t *s)
{
    browser_t *b = (browser_t *)s;
    char buf[32];

    if (b->error != FR_OK) {
        snprintf(buf, sizeof(buf), b->error == FR_NOT_READY ? "no card" :
                 "error %d", (int)b->error);
        gfx_text_centered(UI_TOP + 50, buf, C_BAD, FONT_SMALL, 1);
        return;
    }
    list_draw(&b->list);
}

static bool is_text(const char *name)
{
    static const char *const ext[] = {
        ".txt", ".log", ".cfg", ".ini", ".md", ".json", ".m3u", ".m3u8",
        ".csv", ".xml",
    };
    size_t n = strlen(name);

    for (unsigned i = 0; i < ARRAY_SIZE(ext); i++) {
        size_t e = strlen(ext[i]);
        if (n > e) {
            const char *p = name + n - e, *q = ext[i];
            bool same = true;
            for (; *q; p++, q++)
                if ((*p | 0x20) != *q && *p != *q)
                    same = false;
            if (same)
                return true;
        }
    }
    return false;
}

static void open_entry(browser_t *b, entry_t *e)
{
    size_t plen = strlen(b->path), nlen = strlen(e->name);
    char *full = kmalloc(plen + nlen + 2);

    if (!full)
        return;
    memcpy(full, b->path, plen);
    if (plen && full[plen - 1] != '/')
        full[plen++] = '/';
    memcpy(full + plen, e->name, nlen + 1);

    if (e->dir) {
        ui_push(app_files(full));
    } else if (is_text(e->name)) {
        char *buf = kmalloc(VIEW_BYTES + 1);
        FIL f;
        UINT n = 0;

        if (buf && f_open(&f, full, FA_READ) == FR_OK) {
            f_read(&f, buf, VIEW_BYTES, &n);
            f_close(&f);
            buf[n] = 0;
            for (UINT i = 0; i < n; i++)    /* tabs, CR, odd bytes */
                if (buf[i] == '\r' || buf[i] == '\t' ||
                    ((unsigned char)buf[i] < 0x20 && buf[i] != '\n'))
                    buf[i] = ' ';
            ui_text_view(e->name, buf);
        }
        kfree(buf);
    } else {
        char msg[96], size[16];

        size_text(e->size, size, sizeof(size));
        snprintf(msg, sizeof(msg), "%s\n\n%s (%lu bytes)", e->name, size,
                 (unsigned long)e->size);
        ui_message("File", msg);
    }
    kfree(full);
}

static void key(screen_t *s, const key_event_t *ev)
{
    browser_t *b = (browser_t *)s;

    if (list_key(&b->list, ev))
        return;
    if ((ev->code == KEY_CENTER || ev->code == KEY_LSOFT) && b->list.count)
        open_entry(b, &b->entries[b->list.sel]);
    else if (ev->code == KEY_RSOFT)
        ui_pop();
}

screen_t *app_files(const char *path)
{
    size_t n = strlen(path) + 1;
    browser_t *b = kzalloc(sizeof(*b) + n);

    if (!b)
        return NULL;
    memcpy(b->path, path, n);
    b->s.title = b->path;
    b->s.left = "Open";
    b->s.center = "OK";
    b->s.right = "Back";
    b->s.enter = enter;
    b->s.leave = leave;
    b->s.draw = draw;
    b->s.key = key;
    b->s.data = b;
    b->s.free_on_pop = true;
    b->list.item = item;
    b->list.ctx = b;
    return &b->s;
}
