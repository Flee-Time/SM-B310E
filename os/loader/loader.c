/* B310E OS — app loader: read an .app into the heap, relocate, start it,
 * and free it once its last screen has left the stack (os/app.h).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/app.h"
#include "os/arch.h"
#include "os/fs.h"
#include "os/kernel.h"
#include "os/lib.h"
#include "os/notify.h"
#include "os/status.h"

#define RELOC_CHUNK 64

struct app {
    app_t *next;
    const app_info_t *info;
    uint8_t *image;
    uint32_t size;                      /* image + bss */
    void **allocs;                      /* api->malloc blocks, freed with it */
    int n_allocs, cap_allocs;
    char name[20];
    char *arg;                          /* what it was opened with, or NULL */
    char dir[96];                       /* the folder its .app is in (2.6) */
    char path[96];                      /* its .app */
    bool reload;                        /* load it again once it has gone (2.7) */
    int vol_cat;                        /* its volume tag (2.10); 0: none yet */
    char *reload_arg;
};

/* the app to load again once app_collect has unloaded it (app_reload) */
static char reload_path[96], *reload_arg;
static bool reload_pending;
static uint32_t reload_asked;           /* when app_reload was called; 0: not */
#define RELOAD_PATIENCE_MS 10000        /* an app that does not go: not for ever */

static app_t *apps;
app_t *app_current;
static mutex_t allocs_lock;             /* every app's allocs list (threads) */

/* The app a call is made for: the one whose thread is running, else the
 * one whose callback the UI is running. An app's own thread must not go
 * by app_current: that is the UI's, and whatever it last set (often no app
 * at all, or another app's while that one's callback waits on the card). */
app_t *app_self(void)
{
    thread_t *t = thread_self();

    return t && t->owner ? t->owner : app_current;
}

/* ---- CRC-32 (IEEE), four bits at a time ---------------------------------- */
static uint32_t crc32_update(uint32_t crc, const void *data, size_t n)
{
    static const uint32_t t[16] = {
        0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac, 0x76dc4190, 0x6b6b51f4,
        0x4db26158, 0x5005713c, 0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c,
        0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c,
    };
    const uint8_t *p = data;

    while (n--) {
        crc ^= *p++;
        crc = (crc >> 4) ^ t[crc & 15];
        crc = (crc >> 4) ^ t[crc & 15];
    }
    return crc;
}

/* ---- memory owned by an app ---------------------------------------------- */
/* (allocs_lock held) */
static void track(app_t *a, void *p)
{
    if (a->n_allocs == a->cap_allocs) {
        int cap = a->cap_allocs ? a->cap_allocs * 2 : 16;
        void **n = kmalloc((size_t)cap * sizeof(void *));

        if (!n)
            return;                     /* untracked: freed only by the app */
        if (a->allocs) {
            memcpy(n, a->allocs, (size_t)a->n_allocs * sizeof(void *));
            kfree(a->allocs);
        }
        a->allocs = n;
        a->cap_allocs = cap;
    }
    a->allocs[a->n_allocs++] = p;
}

/* (allocs_lock held) */
static bool forget(app_t *a, void *p)
{
    for (int i = 0; a && i < a->n_allocs; i++)
        if (a->allocs[i] == p) {
            a->allocs[i] = a->allocs[--a->n_allocs];
            return true;
        }
    return false;
}

void app_forget(app_t *a, void *p)
{
    mutex_lock(&allocs_lock, WAIT_FOREVER);
    forget(a, p);
    mutex_unlock(&allocs_lock);
}

void *app_malloc(size_t size)
{
    void *p = kmalloc(size);

    app_t *a = app_self();

    if (p && a) {
        mutex_lock(&allocs_lock, WAIT_FOREVER);
        track(a, p);
        mutex_unlock(&allocs_lock);
    }
    return p;
}

void *app_zalloc(size_t size)
{
    void *p = app_malloc(size);

    if (p)
        memset(p, 0, size);
    return p;
}

/* Off the list it is on: the caller's, or another app's (a block one
 * thread allocated and another freed); left on one, unload would free it
 * a second time. */
void app_free(void *p)
{
    app_t *self;

    if (!p)
        return;
    mutex_lock(&allocs_lock, WAIT_FOREVER);
    self = app_self();
    if (!forget(self, p))
        for (app_t *a = apps; a; a = a->next)
            if (a != self && forget(a, p))
                break;
    mutex_unlock(&allocs_lock);
    kfree(p);
}

/* ---- loading -------------------------------------------------------------- */
static const char *check_header(const app_file_t *h, uint32_t file_size)
{
    if (h->magic != APP_MAGIC || h->header_size != sizeof(*h))
        return "not an app";
    if (h->api_major != OS_API_MAJOR || h->api_minor > OS_API_MINOR)
        return "built for another OS API";
    if (h->image_size < sizeof(app_info_t) || h->image_size > APP_MAX_SIZE ||
        h->bss_size > APP_MAX_SIZE - h->image_size)
        return "bad size";
    if (file_size != sizeof(*h) + h->image_size + h->reloc_count * 4u)
        return "file truncated";
    return NULL;
}

int app_peek(const char *path, app_file_t *out)
{
    FIL f;
    UINT n = 0;
    FRESULT r = f_open(&f, path, FA_READ);

    if (r != FR_OK)
        return -1;
    r = f_read(&f, out, sizeof(*out), &n);
    if (r == FR_OK && n == sizeof(*out) && !check_header(out, (uint32_t)f_size(&f))) {
        out->name[sizeof(out->name) - 1] = 0;
        f_close(&f);
        return 0;
    }
    f_close(&f);
    return -1;
}

/* The icon an app built for 2.3 or later declared: app_info_t is at the
 * start of the image, linked at 0, so its icon pointer in the file is the
 * icon's offset in the image (the loader relocates it only when loading). */
bool app_peek_icon(const char *path, const app_file_t *h, icon_t *out)
{
    FIL f;
    UINT n = 0;
    uint32_t off = 0;
    bool ok = false;

    if (h->api_minor < 3 || f_open(&f, path, FA_READ) != FR_OK)
        return false;
    if (f_lseek(&f, h->header_size + APP_INFO_ICON) == FR_OK &&
        f_read(&f, &off, 4, &n) == FR_OK && n == 4 && off >= sizeof(app_info_t) &&
        off <= h->image_size - sizeof(*out) &&
        f_lseek(&f, h->header_size + off) == FR_OK &&
        f_read(&f, out, sizeof(*out), &n) == FR_OK && n == sizeof(*out))
        ok = true;
    f_close(&f);
    return ok;
}

const char *app_name(const app_t *a)
{
    return a->name;
}

/* ---- per-app volume (2.10) ----------------------------------------------- */
static const icon_t *own_icon(const app_t *a)
{
    return a->info->api_minor >= 3 ? a->info->icon : NULL;
}

int app_volume_cat(void)
{
    app_t *a = app_self();

    if (!a)
        return VOL_APP;
    if (!a->vol_cat)
        a->vol_cat = volume_tag(a->name, own_icon(a));
    return a->vol_cat;
}

int app_volume_register(const char *name)
{
    app_t *a = app_self();
    int cat = volume_tag(name, a ? own_icon(a) : NULL);

    if (a && !a->vol_cat)
        a->vol_cat = cat;
    return cat;
}

void app_volume_choose(int cat)
{
    app_t *a = app_self();

    if (a && cat >= VOL_COUNT)
        a->vol_cat = cat;
}

static void unload(app_t *a)
{
    app_t **pp;

    notify_drop_owner(a);
    if (a->info->stop) {
        app_t *saved = app_current;

        app_current = a;
        a->info->stop();
        app_current = saved;
    }
    for (pp = &apps; *pp; pp = &(*pp)->next)
        if (*pp == a) {
            *pp = a->next;
            break;
        }
    mutex_lock(&allocs_lock, WAIT_FOREVER);
    for (int i = 0; i < a->n_allocs; i++)
        kfree(a->allocs[i]);
    mutex_unlock(&allocs_lock);
    os_status.apps_loaded = (uint32_t)app_count();
    klog("app: %s closed (%d blocks freed)", a->name, a->n_allocs);
    if (a->reload && !reload_pending) {
        memcpy(reload_path, a->path, sizeof(reload_path));
        reload_arg = a->reload_arg;     /* app_collect loads it again */
        a->reload_arg = NULL;
        reload_pending = true;
    }
    kfree(a->reload_arg);
    kfree(a->arg);
    kfree(a->allocs);
    kfree(a->image);
    kfree(a);
}

/* ---- a CRC that does not match: where, and whether the card agrees ---------- */
#define PART (64u * 1024)               /* the log's CRCs are per 64 KB */
#define PARTS (APP_MAX_SIZE / PART)

static uint32_t crc_of(const void *p, size_t n)
{
    return ~crc32_update(0xffffffffu, p, n);
}

/* The CRC of each 64 KB of the image and of the relocations, as read
 * first (part[], relocs) and as the file reads now, a piece at a time into
 * a small block: parts that differ between the reads point at the read or
 * the memory it went to, the same CRCs both times at the file itself
 * (the file's own: python os/sdk/appcrc.py file.app). */
static void crc_report(FIL *f, const app_t *a, const app_file_t *h,
                       const uint32_t *part, uint32_t relocs)
{
    enum { PIECE = 16 * 1024 };
    uint8_t *buf = kmalloc(PIECE);
    uint32_t nparts = (h->image_size + PART - 1) / PART, again[PARTS], crc = 0xffffffffu;
    uint32_t pos = 0, left = h->reloc_count * 4u;
    UINT n;
    int differ = 0;

    klog("app: %s: CRC, image at %p, %lu + %lu bytes, %lu relocations",
         h->name, (void *)a->image, (unsigned long)h->image_size,
         (unsigned long)h->bss_size, (unsigned long)h->reloc_count);
    if (!buf || f_lseek(f, h->header_size) != FR_OK) {
        kfree(buf);
        return;
    }
    while (pos < h->image_size) {       /* the image again */
        uint32_t k = h->image_size - pos < PIECE ? h->image_size - pos : PIECE;

        if (f_read(f, buf, k, &n) != FR_OK || n != k)
            break;
        crc = crc32_update(crc, buf, k);
        pos += k;
        if (pos % PART == 0 || pos == h->image_size) {
            again[(pos - 1) / PART] = ~crc;
            crc = 0xffffffffu;
        }
    }
    for (; pos == h->image_size && left; left -= n) {     /* the relocations */
        if (f_read(f, buf, left < PIECE ? left : PIECE, &n) != FR_OK || !n)
            break;
        crc = crc32_update(crc, buf, n);
    }
    kfree(buf);
    if (pos != h->image_size || left) {
        klog("app: %s: the second read failed at %lu", h->name, (unsigned long)pos);
        return;
    }
    for (uint32_t i = 0; i < nparts; i++) {
        klog("app: %s: %3lu KB %08lx %08lx%s", h->name, (unsigned long)(i * PART / 1024),
             (unsigned long)part[i], (unsigned long)again[i], part[i] != again[i] ? " differ" : "");
        differ += part[i] != again[i];
    }
    klog("app: %s: relocs %08lx %08lx%s; %d of %lu parts read differently", h->name,
         (unsigned long)relocs, (unsigned long)~crc, relocs != ~crc ? " differ" : "",
         differ, (unsigned long)nparts);
}

static const char *load(FIL *f, app_t *a, const app_file_t *h)
{
    uint32_t reloc[RELOC_CHUNK], crc = 0xffffffffu, left = h->reloc_count;
    uint32_t part[PARTS], rcrc = 0xffffffffu;
    UINT n;

    if (f_read(f, a->image, h->image_size, &n) != FR_OK || n != h->image_size)
        return "read error";
    crc = crc32_update(crc, a->image, h->image_size);
    for (uint32_t i = 0; i * PART < h->image_size; i++)     /* for crc_report */
        part[i] = crc_of(a->image + i * PART,
                         h->image_size - i * PART < PART ? h->image_size - i * PART : PART);
    while (left) {
        uint32_t k = left > RELOC_CHUNK ? RELOC_CHUNK : left;

        if (f_read(f, reloc, k * 4, &n) != FR_OK || n != k * 4)
            return "read error";
        crc = crc32_update(crc, reloc, k * 4);
        rcrc = crc32_update(rcrc, reloc, k * 4);
        for (uint32_t i = 0; i < k; i++) {
            uint32_t off = reloc[i], v;

            if (off > h->image_size - 4)
                return "bad relocation";
            if (!(off & 3)) {
                uint32_t *w = (uint32_t *)(a->image + off);

                if (*w > a->size)       /* must point into the app */
                    return "bad relocation";
                *w += (uint32_t)a->image;
                continue;
            }
            /* 2.8: a word of a packed struct (Wolf4SDL's), a byte at a time */
            memcpy(&v, a->image + off, 4);
            if (v > a->size)
                return "bad relocation";
            v += (uint32_t)a->image;
            memcpy(a->image + off, &v, 4);
        }
        left -= k;
    }
    if (~crc != h->crc32) {
        crc_report(f, a, h, part, ~rcrc);
        return "damaged (CRC)";
    }
    memset(a->image + h->image_size, 0, h->bss_size);
    return NULL;
}

static const char *open_with(const char *path, const char *arg);

/* An app already loaded: its start() again, with the new argument. */
static const char *restart(app_t *a, const char *arg)
{
    screen_t *s;
    app_t *saved = app_current;

    kfree(a->arg);
    a->arg = NULL;
    if (arg && (a->arg = kmalloc(strlen(arg) + 1)))
        memcpy(a->arg, arg, strlen(arg) + 1);
    app_current = a;
    s = a->info->start(&os_api);
    if (s)
        ui_push(s);
    app_current = saved;
    klog("app: %s started again", a->name);
    return s ? NULL : "the app did not start";
}

const char *app_open(const char *path)
{
    return open_with(path, NULL);
}

/* In dir (and, deeper levels down, in its folders): the .app whose name
 * (its header's) or file name is name; its path in path. */
static bool find_in(const char *dir, const char *name, char *path, size_t size, int deeper)
{
    DIR d;
    FILINFO fi;
    bool found = false;

    if (f_opendir(&d, dir) != FR_OK)
        return false;
    while (!found && f_readdir(&d, &fi) == FR_OK && fi.fname[0]) {
        app_file_t h;
        size_t n = strlen(fi.fname);

        if (fi.fattrib & AM_DIR) {
            if (deeper && fi.fname[0] != '.') {
                char sub[96];

                if (snprintf(sub, sizeof(sub), "%s/%s", dir, fi.fname) < (int)sizeof(sub))
                    found = find_in(sub, name, path, size, deeper - 1);
            }
            continue;
        }
        if (n < 5 || strcmp(fi.fname + n - 4, ".app") ||
            snprintf(path, size, "%s/%s", dir, fi.fname) >= (int)size)
            continue;
        found = (app_peek(path, &h) == 0 && !strcmp(h.name, name)) || !strcmp(fi.fname, name);
    }
    f_closedir(&d);
    return found;
}

/* An app's file by its name ("Doom") or file name ("doom.app"): in
 * APP_DIR, in a folder of its own there (APP_DIR "/Doom/doom.app"), or in
 * a category (APP_DIR "/Games/Doom/doom.app"). */
bool app_find(const char *name, char *path, size_t size)
{
    return find_in(APP_DIR, name, path, size, 3);
}

/* An app on the card by its name ("Music") or file ("music.app"), opened
 * with an argument it reads through app_arg(). An app that is open already
 * gets the argument and starts again (start() pushes its screen anew). */
const char *app_launch(const char *name, const char *arg)
{
    char path[96];

    for (app_t *a = apps; a; a = a->next)
        if (!strcmp(a->name, name))
            return restart(a, arg);
    return app_find(name, path, sizeof(path)) ? open_with(path, arg) : "no such app";
}

/* Load the app again, fresh (its data and bss as on the card), with arg,
 * once it has gone: its screens off the stack, its threads ended. An
 * emulator's way back to its list of ROMs, or on to one, with nothing of
 * the last game left in its variables. */
bool app_reload(const char *arg)
{
    app_t *a = app_self();

    if (!a)
        return false;
    kfree(a->reload_arg);
    a->reload_arg = NULL;
    if (arg && (a->reload_arg = kmalloc(strlen(arg) + 1)))
        memcpy(a->reload_arg, arg, strlen(arg) + 1);
    a->reload = true;
    reload_asked = kernel_ticks() | 1;
    return true;
}

bool app_reloading(void)
{
    if (reload_asked && (int32_t)(kernel_ticks() - reload_asked) > RELOAD_PATIENCE_MS)
        reload_asked = 0;
    return reload_asked != 0;
}

/* The folder the app's .app is in: APP_DIR, or one of its own there
 * (where a game keeps its data, its saves and its settings). */
const char *app_dir(void)
{
    app_t *a = app_self();

    return a ? a->dir : NULL;
}

const char *app_arg(void)
{
    app_t *a = app_self();

    return a ? a->arg : NULL;
}

/* A thread of the app's: the app stays loaded while it runs, even with no
 * screen of its own on the stack (a background job). Below the UI. */
thread_t *app_thread(const char *name, int prio, size_t stack, void (*fn)(void *), void *arg)
{
    thread_t *t;

    if (prio >= PRIO_NORMAL)
        prio = PRIO_NORMAL - 1;
    if (prio < PRIO_LOW)
        prio = PRIO_LOW;
    t = thread_create(name, prio, stack, fn, arg);
    if (t)
        t->owner = app_self();          /* a thread's thread: the same app's */
    return t;
}

static const char *open_with(const char *path, const char *arg)
{
    app_file_t h;
    FIL f;
    UINT n = 0;
    app_t *a;
    const char *err;
    screen_t *first;
    uint32_t t = kernel_ticks();

    if (f_open(&f, path, FA_READ) != FR_OK)
        return "cannot open";
    if (f_read(&f, &h, sizeof(h), &n) != FR_OK || n != sizeof(h)) {
        f_close(&f);
        return "not an app";
    }
    if ((err = check_header(&h, (uint32_t)f_size(&f)))) {
        f_close(&f);
        return err;
    }
    h.name[sizeof(h.name) - 1] = 0;
    for (a = apps; a; a = a->next)      /* still loaded (a notification, a
                                         * thread): start that one again */
        if (!strcmp(a->name, h.name)) {
            f_close(&f);
            return restart(a, arg);
        }
    a = kzalloc(sizeof(*a));
    if (a) {
        a->size = h.image_size + h.bss_size;
        a->image = kmalloc(a->size);
    }
    if (!a || !a->image) {
        f_close(&f);
        if (a)
            kfree(a);
        return "not enough memory";
    }
    err = load(&f, a, &h);
    f_close(&f);
    if (!err) {
        a->info = (const app_info_t *)a->image;
        if (a->info->api_major != h.api_major || !a->info->start)
            err = "bad app info";
    }
    if (err) {
        kfree(a->image);
        kfree(a);
        return err;
    }
    /* The code was written through the D-cache: clean it to memory, then
     * drop stale instructions from the I-cache. */
    dcache_clean_range(a->image, a->size);
    icache_invalidate_all();

    memcpy(a->name, h.name, sizeof(a->name));
    a->name[sizeof(a->name) - 1] = 0;
    snprintf(a->path, sizeof(a->path), "%s", path);
    {
        const char *slash = strrchr(path, '/');
        size_t n = slash ? (size_t)(slash - path) : 0;

        if (n >= sizeof(a->dir))
            n = 0;
        memcpy(a->dir, path, n);
        a->dir[n] = 0;
    }
    if (arg && (a->arg = kmalloc(strlen(arg) + 1)))
        memcpy(a->arg, arg, strlen(arg) + 1);
    a->next = apps;
    apps = a;
    os_status.apps_loaded = (uint32_t)app_count();
    klog("app: %s loaded, %lu + %lu bytes, %lu relocations, %lu ms",
         a->name, (unsigned long)h.image_size, (unsigned long)h.bss_size,
         (unsigned long)h.reloc_count, (unsigned long)(kernel_ticks() - t));

    {
        app_t *saved = app_current;

        app_current = a;
        first = a->info->start(&os_api);
        if (first)
            ui_push(first);             /* tagged as the app's */
        app_current = saved;
    }
    if (!first) {
        unload(a);
        return "the app did not start";
    }
    return NULL;
}

void app_collect(screen_t *const *stack, int depth)
{
    app_t *a = apps, *next;

    for (; a; a = next) {
        bool used = false;

        next = a->next;
        for (int i = 0; i < depth && !used; i++)
            used = stack[i]->app == a;
        if (!used && thread_count_owned(a))
            used = true;                /* its threads still run: kept */
        if (!used && notify_owned(a))
            used = true;                /* a notification of its is up */
        if (!used)
            unload(a);
    }
    if (reload_pending) {               /* app_reload: the fresh one */
        char *arg = reload_arg;
        const char *err;

        reload_pending = false;
        reload_arg = NULL;
        if ((err = open_with(reload_path, arg)))
            klog("app: %s did not load again: %s", reload_path, err);
        kfree(arg);
        reload_asked = 0;
    }
}

int app_count(void)
{
    int n = 0;

    for (app_t *a = apps; a; a = a->next)
        n++;
    return n;
}
