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
};

static app_t *apps;
app_t *app_current;

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

void app_forget(app_t *a, void *p)
{
    for (int i = 0; a && i < a->n_allocs; i++)
        if (a->allocs[i] == p) {
            a->allocs[i] = a->allocs[--a->n_allocs];
            return;
        }
}

void *app_malloc(size_t size)
{
    void *p = kmalloc(size);

    if (p && app_current)
        track(app_current, p);
    return p;
}

void *app_zalloc(size_t size)
{
    void *p = app_malloc(size);

    if (p)
        memset(p, 0, size);
    return p;
}

void app_free(void *p)
{
    if (!p)
        return;
    app_forget(app_current, p);
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
    for (int i = 0; i < a->n_allocs; i++)
        kfree(a->allocs[i]);
    os_status.apps_loaded = (uint32_t)app_count();
    klog("app: %s closed (%d blocks freed)", a->name, a->n_allocs);
    kfree(a->arg);
    kfree(a->allocs);
    kfree(a->image);
    kfree(a);
}

static const char *load(FIL *f, app_t *a, const app_file_t *h)
{
    uint32_t reloc[RELOC_CHUNK], crc = 0xffffffffu, left = h->reloc_count;
    UINT n;

    if (f_read(f, a->image, h->image_size, &n) != FR_OK || n != h->image_size)
        return "read error";
    crc = crc32_update(crc, a->image, h->image_size);
    while (left) {
        uint32_t k = left > RELOC_CHUNK ? RELOC_CHUNK : left;

        if (f_read(f, reloc, k * 4, &n) != FR_OK || n != k * 4)
            return "read error";
        crc = crc32_update(crc, reloc, k * 4);
        for (uint32_t i = 0; i < k; i++) {
            uint32_t off = reloc[i], *w;

            if (off & 3 || off > h->image_size - 4)
                return "bad relocation";
            w = (uint32_t *)(a->image + off);
            if (*w > a->size)           /* must point into the app */
                return "bad relocation";
            *w += (uint32_t)a->image;
        }
        left -= k;
    }
    if (~crc != h->crc32)
        return "damaged (CRC)";
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

/* An app on the card by its name ("Music") or file ("music.app"), opened
 * with an argument it reads through app_arg(). An app that is open already
 * gets the argument and starts again (start() pushes its screen anew). */
const char *app_launch(const char *name, const char *arg)
{
    DIR d;
    FILINFO fi;
    char path[80];
    const char *err = "no such app";

    for (app_t *a = apps; a; a = a->next)
        if (!strcmp(a->name, name))
            return restart(a, arg);
    if (f_opendir(&d, APP_DIR) != FR_OK)
        return "no apps folder";
    while (f_readdir(&d, &fi) == FR_OK && fi.fname[0]) {
        app_file_t h;
        size_t n = strlen(fi.fname);

        if (n < 5 || strcmp(fi.fname + n - 4, ".app"))
            continue;
        snprintf(path, sizeof(path), APP_DIR "/%s", fi.fname);
        if ((app_peek(path, &h) == 0 && !strcmp(h.name, name)) || !strcmp(fi.fname, name)) {
            f_closedir(&d);
            return open_with(path, arg);
        }
    }
    f_closedir(&d);
    return err;
}

const char *app_arg(void)
{
    return app_current ? app_current->arg : NULL;
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
        t->owner = app_current;
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
}

int app_count(void)
{
    int n = 0;

    for (app_t *a = apps; a; a = a->next)
        n++;
    return n;
}
