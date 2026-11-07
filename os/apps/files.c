/* B310E OS — file browser: folders first, then files with their sizes;
 * Center opens a folder, shows a text file's start, or a file's details;
 * LSoft: Options, to rename (the multi-tap editor), delete, or copy a
 * file and paste it into another folder (a progress bar; RSoft cancels).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/app.h"
#include "os/fs.h"
#include "os/kernel.h"
#include "os/lib.h"
#include "os/media.h"
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
    bool reload;                /* changed by Options: read it again */
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

    snprintf(text, (size_t)tn, "%s", e->name);
    if (!e->dir)
        size_text(e->size, detail, dn);
}

static const icon_t *entry_icon(void *ctx, int i)
{
    browser_t *b = ctx;

    return b->entries[i].dir ? &icon_folder : &icon_file;
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

static void reload(browser_t *b)
{
    kfree(b->entries);
    kfree(b->names);
    b->entries = NULL;
    b->names = NULL;
    b->reload = false;
    load(b);
    if (b->list.sel >= b->list.count)
        b->list.sel = b->list.count ? b->list.count - 1 : 0;
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

    if (b->reload)
        reload(b);
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

static bool has_ext(const char *name, const char *ext)
{
    size_t n = strlen(name), e = strlen(ext);

    if (n <= e)
        return false;
    for (size_t i = 0; i < e; i++)
        if ((name[n - e + i] | 0x20) != ext[i] && name[n - e + i] != ext[i])
            return false;
    return true;
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
    } else if (media_supported(e->name) || has_ext(e->name, ".m3u") ||
               has_ext(e->name, ".m3u8")) {
        /* the Music app plays it (and its folder); without the app the
         * play queue still does */
        if (app_launch("Music", full)) {
            media_queue_clear();
            if (has_ext(e->name, ".m3u") || has_ext(e->name, ".m3u8")) {
                media_queue_add_m3u(full);
                media_queue_play(0);
            } else {
                *strrchr(full, '/') = 0;
                media_queue_add_folder(full[0] ? full : "/", 0);
                full[strlen(full)] = '/';
                media_queue_play(media_queue_find(full));
            }
            ui_message("Music", "Playing. Put music.app in /b310e-os/apps for the player.");
        }
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

/* ---- Options: rename, delete, copy, paste --------------------------------- */

enum { OP_RENAME, OP_DELETE, OP_COPY, OP_PASTE };

static struct {
    browser_t *b;
    char path[256];             /* the entry; empty when the folder is empty */
    char name[128];
    bool dir;
    int rows[4], count;         /* OP_* in the list's order */
} op;

static char clip[256];          /* the file marked by Copy, "" none */
static const char *clip_name(void)
{
    const char *s = strrchr(clip, '/');

    return s ? s + 1 : clip;
}

static list_t op_list;

static void op_item(void *ctx, int i, char *text, int tn, char *detail, int dn)
{
    static const char *const names[] = { "Rename", "Delete", "Copy", "Paste here" };

    snprintf(text, (size_t)tn, "%s", names[op.rows[i]]);
    if (op.rows[i] == OP_PASTE)
        snprintf(detail, (size_t)dn, "%s", clip_name());
}

static void renamed(const char *name, void *ctx)
{
    char to[256];
    size_t dirlen = strlen(op.path) - strlen(op.name);
    FRESULT r;

    if (!name[0] || !strcmp(name, op.name) || strchr(name, '/'))
        return;
    if (dirlen + strlen(name) + 1 > sizeof(to))
        return;
    memcpy(to, op.path, dirlen);
    memcpy(to + dirlen, name, strlen(name) + 1);
    r = f_rename(op.path, to);
    if (r == FR_OK) {
        klog("files: renamed %s to %s", op.path, name);
        if (!strcmp(clip, op.path))     /* the marked file moved with it */
            memcpy(clip, to, strlen(to) + 1);
        op.b->reload = true;
    } else {
        ui_message("Rename", r == FR_EXIST ? "That name is taken." : "Could not rename it.");
    }
}

static void delete_yes(void)
{
    FRESULT r = f_unlink(op.path);

    if (r == FR_OK) {
        klog("files: deleted %s", op.path);
        if (!strcmp(clip, op.path))
            clip[0] = 0;
        op.b->reload = true;
    } else {
        ui_message("Delete", r == FR_DENIED && op.dir ? "The folder is not empty." :
                   "Could not delete it.");
    }
}

/* ---- the copy: 16 KB a UI tick, with a bar; RSoft cancels ------------------ */

#define COPY_CHUNK (16u * 1024)

static struct {
    FIL src, dst;
    uint8_t *buf;
    uint32_t done, size;
    char to[256];
    bool open;
    browser_t *b;
} cp;

static void copy_close(bool keep)
{
    if (cp.open) {
        f_close(&cp.src);
        f_close(&cp.dst);
        if (!keep)
            f_unlink(cp.to);            /* no half a file left behind */
        cp.open = false;
    }
    kfree(cp.buf);
    cp.buf = NULL;
}

static void copy_tick(screen_t *s)
{
    UINT n = 0, w = 0;
    FRESULT r;

    if (!cp.open)
        return;
    if ((r = f_read(&cp.src, cp.buf, COPY_CHUNK, &n)) != FR_OK ||
        (n && ((r = f_write(&cp.dst, cp.buf, n, &w)) != FR_OK || w != n))) {
        klog("files: copy to %s failed after %lu bytes (FatFs %d)", cp.to,
             (unsigned long)cp.done, (int)r);
        copy_close(false);
        ui_pop();
        ui_message("Paste", w != n ? "The card is full." : "Could not copy it.");
        return;
    }
    cp.done += n;
    if (!n || cp.done >= cp.size) {
        copy_close(true);
        klog("files: copied %s to %s, %lu bytes", clip, cp.to, (unsigned long)cp.done);
        cp.b->reload = true;
        ui_pop();
    }
}

static void copy_draw(screen_t *s)
{
    char buf[32];

    gfx_text_centered(UI_TOP + 24, "Copying", C_TEXT, FONT_LARGE, 1);
    gfx_text_centered(UI_TOP + 42, clip_name(), C_DIM, FONT_SMALL, 1);
    gfx_bar(UI_PAD + 8, UI_TOP + 60, LCD_WIDTH - 2 * UI_PAD - 16, 6,
            (int)(cp.done / 1024), (int)(cp.size / 1024) + 1, C_ACCENT);
    snprintf(buf, sizeof(buf), "%lu of %lu KB", (unsigned long)(cp.done / 1024),
             (unsigned long)(cp.size / 1024));
    gfx_text_centered(UI_TOP + 74, buf, C_DIM, FONT_SMALL, 1);
}

static void copy_key(screen_t *s, const key_event_t *ev)
{
    if (ev->code == KEY_RSOFT) {
        copy_close(false);
        klog("files: copy to %s cancelled", cp.to);
        ui_pop();
    }
}

static void copy_leave(screen_t *s)
{
    copy_close(false);                  /* END home in the middle */
}

static screen_t copy_screen = {
    .title = "Paste", .right = "Cancel",
    .draw = copy_draw, .key = copy_key, .tick = copy_tick, .leave = copy_leave,
    .tick_ms = 10,
};

/* the target: the folder's path plus the name, or "Copy of" the name when
 * that is taken (pasting where it came from) */
static void paste(browser_t *b)
{
    const char *name = clip_name();
    size_t plen = strlen(b->path);
    FRESULT r = FR_EXIST;

    for (int attempt = 0; attempt < 2 && r == FR_EXIST; attempt++) {
        FILINFO fi;
        int n = snprintf(cp.to, sizeof(cp.to), "%s%s%s%s", b->path,
                         plen && b->path[plen - 1] != '/' ? "/" : "",
                         attempt ? "Copy of " : "", name);

        if (n >= (int)sizeof(cp.to))
            return;
        /* a name taken: the next. Asked first: opening a file that is open
         * (the music playing it) to write fails as locked, not as there */
        if (f_stat(cp.to, &fi) == FR_OK)
            r = FR_EXIST;
        else
            r = f_open(&cp.dst, cp.to, FA_WRITE | FA_CREATE_NEW);
    }
    if (r != FR_OK) {
        klog("files: paste to %s: FatFs %d", cp.to, (int)r);
        ui_message("Paste", r == FR_EXIST ? "A file of that name is here." : "Could not copy it.");
        return;
    }
    if ((r = f_open(&cp.src, clip, FA_READ)) != FR_OK || !(cp.buf = kmalloc(COPY_CHUNK))) {
        klog("files: paste from %s: %s %d", clip, r ? "FatFs" : "no memory", (int)r);
        f_close(&cp.dst);
        f_unlink(cp.to);
        ui_message("Paste", "Could not copy it.");
        return;
    }
    cp.open = true;
    cp.done = 0;
    cp.size = (uint32_t)f_size(&cp.src);
    cp.b = b;
    ui_push(&copy_screen);
}

static void op_key(screen_t *s, const key_event_t *ev)
{
    if (list_key(&op_list, ev))
        return;
    if (ev->code == KEY_CENTER || ev->code == KEY_LSOFT) {
        int row = op.rows[op_list.sel];

        ui_pop();                       /* the options; then the action */
        if (row == OP_RENAME) {
            ui_text_edit("Rename", op.name, 64, false, renamed, NULL);
        } else if (row == OP_DELETE) {
            ui_confirm(op.dir ? "Delete folder" : "Delete file", op.name, delete_yes);
        } else if (row == OP_COPY) {
            memcpy(clip, op.path, strlen(op.path) + 1);
            klog("files: marked %s to copy", clip);
        } else if (row == OP_PASTE) {
            paste(op.b);
        }
    } else if (ev->code == KEY_RSOFT) {
        ui_pop();
    }
}

static void op_draw(screen_t *s)
{
    list_draw(&op_list);
    if (op.path[0])
        gfx_text_centered(UI_BOTTOM - 12, op.name, C_DIM, FONT_SMALL, 1);
}

static screen_t op_screen = {
    .title = "Options", .left = "Select", .center = "OK", .right = "Back",
    .draw = op_draw, .key = op_key,
};

/* e: the selected entry, or NULL in an empty folder (only Paste) */
static void options(browser_t *b, entry_t *e)
{
    op.b = b;
    op.count = 0;
    op.path[0] = op.name[0] = 0;
    if (e) {
        size_t plen = strlen(b->path), nlen = strlen(e->name);

        if (plen + nlen + 2 > sizeof(op.path) || nlen + 1 > sizeof(op.name))
            return;
        op.dir = e->dir;
        memcpy(op.path, b->path, plen);
        if (plen && op.path[plen - 1] != '/')
            op.path[plen++] = '/';
        memcpy(op.path + plen, e->name, nlen + 1);
        memcpy(op.name, e->name, nlen + 1);
        op.rows[op.count++] = OP_RENAME;
        op.rows[op.count++] = OP_DELETE;
        if (!e->dir)                    /* files only: no folder copies */
            op.rows[op.count++] = OP_COPY;
    }
    if (clip[0])
        op.rows[op.count++] = OP_PASTE;
    if (!op.count)
        return;
    op_list.count = op.count;
    op_list.sel = 0;
    op_list.item = op_item;
    ui_push(&op_screen);
}

static void key(screen_t *s, const key_event_t *ev)
{
    browser_t *b = (browser_t *)s;

    if (list_key(&b->list, ev))
        return;
    if (ev->code == KEY_CENTER && b->list.count)
        open_entry(b, &b->entries[b->list.sel]);
    else if (ev->code == KEY_LSOFT)
        options(b, b->list.count ? &b->entries[b->list.sel] : NULL);
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
    b->s.left = "Options";
    b->s.center = "OK";
    b->s.right = "Back";
    b->s.enter = enter;
    b->s.leave = leave;
    b->s.draw = draw;
    b->s.key = key;
    b->s.data = b;
    b->s.free_on_pop = true;
    b->list.item = item;
    b->list.icon = entry_icon;
    b->list.ctx = b;
    return &b->s;
}
