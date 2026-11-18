/* Host tests: the stand-ins (see host.h).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#undef WAIT_TIMEOUT                     /* os/kernel.h's is -1, Windows' 258 */
#else
#include <signal.h>
#include <unistd.h>
#endif

#include "host.h"
#include "ff.h"
#include "os/fs.h"
#include "os/kernel.h"

int host_failures;
bool host_long;
#ifdef HOST_PROVE
bool host_prove = true;
#else
bool host_prove = false;
#endif
static char context[512];
static const void *dump_data;
static size_t dump_size;

/* ---- reporting -------------------------------------------------------------- */

static void print_log_tail(void)
{
    size_t n = strlen(host_log);
    const char *p = n > 2000 ? host_log + n - 2000 : host_log;

    if (n)
        fprintf(stderr, "  klog (last lines):\n%s", p);
}

void host_fail(const char *file, int line, const char *what, const char *fmt, ...)
{
    va_list ap;

    if (++host_failures > 12) {
        if (host_failures == 13)
            fprintf(stderr, "  (more failures not shown)\n");
        return;
    }
    fprintf(stderr, "FAIL %s:%d: %s\n", file, line, what);
    if (context[0])
        fprintf(stderr, "  while: %s\n", context);
    fprintf(stderr, "  ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    if (host_failures == 1)
        print_log_tail();
    fflush(stderr);
}

void host_context(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(context, sizeof(context), fmt, ap);
    va_end(ap);
}

void host_dump(const void *data, size_t n)
{
    dump_data = data;
    dump_size = n;
}

static void save_dump(void)
{
    const char *path = getenv("HOST_CRASH_FILE");
    FILE *f;

    if (!dump_data || !path || !(f = fopen(path, "wb")))
        return;
    fwrite(dump_data, 1, dump_size, f);
    fclose(f);
    fprintf(stderr, "  the input (%u bytes) saved as %s\n", (unsigned)dump_size, path);
}

static void crash(const char *what, const void *addr) __attribute__((noreturn));
static void crash(const char *what, const void *addr)
{
    fprintf(stderr, "CRASH: %s at %p\n", what, addr);
    if (context[0])
        fprintf(stderr, "  while: %s\n", context);
    print_log_tail();
    save_dump();
    fflush(stderr);
    fflush(stdout);
    _Exit(70);
}

#ifdef _WIN32
static LONG CALLBACK on_exception(EXCEPTION_POINTERS *e)
{
    DWORD code = e->ExceptionRecord->ExceptionCode;
    const char *what;

    switch (code) {
    case EXCEPTION_ILLEGAL_INSTRUCTION: what = "trap (a UBSan check, or __builtin_trap)"; break;
    case EXCEPTION_ACCESS_VIOLATION:    what = "access violation"; break;
    case EXCEPTION_STACK_OVERFLOW:      what = "stack overflow"; break;
    case EXCEPTION_INT_DIVIDE_BY_ZERO:  what = "integer divide by zero"; break;
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: what = "array bounds"; break;
    case EXCEPTION_DATATYPE_MISALIGNMENT: what = "misaligned access"; break;
    default:
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (code == EXCEPTION_ACCESS_VIOLATION) {
        fprintf(stderr, "  (%s of address %p)\n",
                e->ExceptionRecord->ExceptionInformation[0] ? "write" : "read",
                (void *)e->ExceptionRecord->ExceptionInformation[1]);
    }
    crash(what, e->ExceptionRecord->ExceptionAddress);
}
#else
static void on_signal(int sig)
{
    crash(sig == SIGILL ? "trap (a UBSan check)" : sig == SIGSEGV ? "segmentation fault" :
          "signal", NULL);
}
#endif

void host_init(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--long"))
            host_long = true;
#ifdef _WIN32
    AddVectoredExceptionHandler(1, on_exception);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#else
    signal(SIGILL, on_signal);
    signal(SIGSEGV, on_signal);
    signal(SIGBUS, on_signal);
#endif
}

int host_done(void)
{
    ga_check();
    if (host_failures)
        fprintf(stderr, "%d check(s) failed\n", host_failures);
    return host_failures ? 1 : 0;
}

void host_hexdump(const char *label, const void *p, size_t n)
{
    const uint8_t *b = p;

    fprintf(stderr, "  %s (%u bytes):", label, (unsigned)n);
    for (size_t i = 0; i < n && i < 256; i++)
        fprintf(stderr, "%s%02x", i % 32 ? " " : "\n    ", b[i]);
    fprintf(stderr, "%s\n", n > 256 ? " ..." : "");
}

void *host_edge(size_t n)
{
    static uint8_t *page;

    if (n > 4096) {
        fprintf(stderr, "host_edge: %u bytes\n", (unsigned)n);
        exit(2);
    }
    if (!page) {
#ifdef _WIN32
        DWORD old;
        uint8_t *p = VirtualAlloc(NULL, 2 * 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);

        if (!p || !VirtualProtect(p + 4096, 4096, PAGE_NOACCESS, &old)) {
            fprintf(stderr, "host_edge: no guard page\n");
            exit(2);
        }
        page = p;
#else
        page = malloc(4096);            /* no guard page here */
#endif
    }
    return page + 4096 - n;
}

static uint32_t rng = 0x9e3779b9u;

uint32_t host_rand(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

void host_srand(uint32_t seed)
{
    rng = seed ? seed : 0x9e3779b9u;
}

/* ---- guard allocator --------------------------------------------------------- */

#define GUARD      32
#define G_FRONT    0xa5
#define G_BACK     0x5a
#define FILL_NEW   0xcd
#define FILL_FREE  0xdd
#define M_LIVE     0x4c495645u
#define M_FREED    0x46524545u
#define QUARANTINE (4u << 20)

typedef struct blk {
    uint32_t magic;
    unsigned seq;
    size_t size;
    struct blk *next, *prev;
    const void *caller;
    uint8_t front[GUARD];
} blk_t;

static blk_t live_head = { 0, 0, 0, &live_head, &live_head, NULL, { 0 } };
static blk_t *quar_first, *quar_last;  /* freed blocks, oldest first (next) */
static size_t quar_bytes, live_bytes;
static unsigned live_n, alloc_n, fail_target;
static bool fail_hit;

static uint8_t *user(blk_t *b)
{
    return (uint8_t *)(b + 1);
}

static void heap_error(const char *what, blk_t *b, const void *caller) __attribute__((noreturn));
static void heap_error(const char *what, blk_t *b, const void *caller)
{
    char msg[256];

    if (b)
        snprintf(msg, sizeof(msg), "heap: %s (block #%u, %u bytes, allocated from %p; this call from)",
                 what, b->seq, (unsigned)b->size, b->caller);
    else
        snprintf(msg, sizeof(msg), "heap: %s (this call from)", what);
    crash(msg, caller);
}

static void check_guards(blk_t *b, const void *caller)
{
    const uint8_t *u = user(b);

    for (int i = 0; i < GUARD; i++)
        if (b->front[i] != G_FRONT)
            heap_error("written before the start", b, caller);
    for (int i = 0; i < GUARD; i++)
        if (u[b->size + i] != G_BACK)
            heap_error("written past the end", b, caller);
}

static void quar_drop_oldest(void)
{
    blk_t *b = quar_first;
    const uint8_t *u = user(b);

    for (size_t i = 0; i < b->size; i++)
        if (u[i] != FILL_FREE)
            heap_error("written after it was freed", b, NULL);
    quar_first = b->next;
    if (!quar_first)
        quar_last = NULL;
    quar_bytes -= b->size;
    free(b);
}

static void *do_alloc(size_t n, const void *caller)
{
    blk_t *b;

    alloc_n++;
    if (fail_target && alloc_n == fail_target) {
        fail_hit = true;
        return NULL;
    }
    if (n > (64u << 20))
        return NULL;                    /* the phone has 4 MB */
    b = malloc(sizeof(blk_t) + n + GUARD);
    if (!b) {
        fprintf(stderr, "host malloc failed\n");
        exit(2);
    }
    b->magic = M_LIVE;
    b->seq = alloc_n;
    b->size = n;
    b->caller = caller;
    memset(b->front, G_FRONT, GUARD);
    memset(user(b), FILL_NEW, n);
    memset(user(b) + n, G_BACK, GUARD);
    b->next = live_head.next;
    b->prev = &live_head;
    live_head.next->prev = b;
    live_head.next = b;
    live_n++;
    live_bytes += n;
    return user(b);
}

void *kmalloc(size_t size)
{
    return do_alloc(size, __builtin_return_address(0));
}

void *kzalloc(size_t size)
{
    void *p = do_alloc(size, __builtin_return_address(0));

    if (p)
        memset(p, 0, size);
    return p;
}

void kfree(void *p)
{
    blk_t *b;
    const void *caller = __builtin_return_address(0);

    if (!p)
        return;
    b = (blk_t *)p - 1;
    if (b->magic == M_FREED)
        heap_error("freed twice", b, caller);
    if (b->magic != M_LIVE)
        heap_error("free of a pointer kmalloc did not return", NULL, caller);
    check_guards(b, caller);
    b->prev->next = b->next;
    b->next->prev = b->prev;
    live_n--;
    live_bytes -= b->size;
    b->magic = M_FREED;
    memset(user(b), FILL_FREE, b->size);
    b->next = NULL;
    if (quar_last)
        quar_last->next = b;
    else
        quar_first = b;
    quar_last = b;
    quar_bytes += b->size;
    while (quar_bytes > QUARANTINE && quar_first)
        quar_drop_oldest();
}

size_t heap_free_bytes(void)
{
    return 3u << 20;
}

void heap_stats(size_t *free_b, size_t *largest, unsigned *blocks)
{
    if (free_b)
        *free_b = 3u << 20;
    if (largest)
        *largest = 3u << 20;
    if (blocks)
        *blocks = live_n;
}

void ga_check(void)
{
    for (blk_t *b = live_head.next; b != &live_head; b = b->next)
        check_guards(b, NULL);
    for (blk_t *b = quar_first; b; b = b->next) {
        const uint8_t *u = user(b);

        for (size_t i = 0; i < b->size; i++)
            if (u[i] != FILL_FREE)
                heap_error("written after it was freed", b, NULL);
    }
}

void ga_free_all(void)
{
    ga_check();
    while (live_head.next != &live_head) {
        blk_t *b = live_head.next;

        live_head.next = b->next;
        free(b);
    }
    live_head.prev = &live_head;
    while (quar_first)
        quar_drop_oldest();
    live_n = 0;
    live_bytes = 0;
}

void ga_reset(void)
{
    ga_free_all();
    alloc_n = 0;
    fail_target = 0;
    fail_hit = false;
}

void ga_fail_at(unsigned n)
{
    fail_target = n ? alloc_n + n : 0;
    if (n)
        fail_hit = false;               /* ga_fail_at(0) keeps what happened */
}

unsigned ga_count(void)
{
    return alloc_n;
}

unsigned ga_live(void)
{
    return live_n;
}

size_t ga_live_bytes(void)
{
    return live_bytes;
}

bool ga_failed(void)
{
    return fail_hit;
}

/* ---- memfs ---------------------------------------------------------------------- */

#define FILES 32
#define NAME  160

struct memfile {
    bool used;
    char name[NAME];
    uint8_t *data;
    uint32_t size, cap;
    int opens;
};

static struct memfile files[FILES];
static bool powered = true, junk;
static uint32_t budget = MEMFS_UNLIMITED, used_units, junk_seed = 1;

static const char *norm(const char *path)
{
    if (!strncmp(path, "0:", 2))
        path += 2;
    return path;
}

static struct memfile *find(const char *path)
{
    path = norm(path);
    for (int i = 0; i < FILES; i++)
        if (files[i].used && !strcmp(files[i].name, path))
            return &files[i];
    return NULL;
}

static struct memfile *create(const char *path)
{
    path = norm(path);
    for (int i = 0; i < FILES; i++)
        if (!files[i].used && !files[i].opens) {
            struct memfile *f = &files[i];

            free(f->data);
            memset(f, 0, sizeof(*f));
            f->used = true;
            snprintf(f->name, NAME, "%s", path);
            return f;
        }
    fprintf(stderr, "memfs: too many files\n");
    exit(2);
}

static void grow(struct memfile *f, uint32_t size)
{
    if (size > f->cap) {
        uint32_t cap = size + 4096;

        f->data = realloc(f->data, cap);
        if (!f->data)
            exit(2);
        f->cap = cap;
    }
}

/* one unit of the budget, or the power goes */
static bool spend(uint32_t n)
{
    if (!powered)
        return false;
    if (budget != MEMFS_UNLIMITED) {
        if (budget < n) {
            powered = false;
            return false;
        }
        budget -= n;
    }
    used_units += n;
    return true;
}

void memfs_reset(void)
{
    for (int i = 0; i < FILES; i++) {
        free(files[i].data);
        memset(&files[i], 0, sizeof(files[i]));
    }
    powered = true;
    budget = MEMFS_UNLIMITED;
    used_units = 0;
    junk = false;
}

void memfs_budget(uint32_t units)
{
    budget = units;
}

uint32_t memfs_used(void)
{
    return used_units;
}

bool memfs_powered(void)
{
    return powered;
}

void memfs_set_junk(bool on, uint32_t seed)
{
    junk = on;
    junk_seed = seed | 1;
}

void memfs_power_on(void)
{
    for (int i = 0; i < FILES; i++)
        files[i].opens = 0;             /* RAM is gone: no handle survives */
    powered = true;
    budget = MEMFS_UNLIMITED;
}

bool memfs_exists(const char *path)
{
    return find(path) != NULL;
}

const uint8_t *memfs_data(const char *path, uint32_t *size)
{
    struct memfile *f = find(path);

    if (size)
        *size = f ? f->size : 0;
    return f ? (f->data ? f->data : (const uint8_t *)"") : NULL;
}

void memfs_put(const char *path, const void *data, uint32_t size)
{
    struct memfile *f = find(path);

    if (!f)
        f = create(path);
    grow(f, size);
    if (size)
        memcpy(f->data, data, size);
    f->size = size;
}

void memfs_remove(const char *path)
{
    struct memfile *f = find(path);

    if (f)
        f->used = false;
}

int memfs_open_count(void)
{
    int n = 0;

    for (int i = 0; i < FILES; i++)
        n += files[i].opens;
    return n;
}

struct memfs_image {
    int n;
    char name[FILES][NAME];
    uint8_t *data[FILES];
    uint32_t size[FILES];
};

memfs_image_t *memfs_save(void)
{
    memfs_image_t *m = calloc(1, sizeof(*m));

    for (int i = 0; i < FILES; i++)
        if (files[i].used) {
            int k = m->n++;

            memcpy(m->name[k], files[i].name, NAME);
            m->size[k] = files[i].size;
            m->data[k] = malloc(files[i].size + 1);
            memcpy(m->data[k], files[i].data ? files[i].data : (uint8_t *)"", files[i].size);
        }
    return m;
}

void memfs_restore(const memfs_image_t *m)
{
    memfs_reset();
    for (int k = 0; k < m->n; k++)
        memfs_put(m->name[k], m->data[k], m->size[k]);
}

void memfs_image_free(memfs_image_t *m)
{
    if (!m)
        return;
    for (int k = 0; k < m->n; k++)
        free(m->data[k]);
    free(m);
}

bool memfs_same(const memfs_image_t *m)
{
    int n = 0;

    for (int i = 0; i < FILES; i++)
        n += files[i].used;
    if (n != m->n)
        return false;
    for (int k = 0; k < m->n; k++) {
        struct memfile *f = find(m->name[k]);

        if (!f || f->size != m->size[k] || (f->size && memcmp(f->data, m->data[k], f->size)))
            return false;
    }
    return true;
}

FSIZE_t memfs_fsize(const FIL *fp)
{
    return fp->node ? fp->node->size : 0;
}

FRESULT f_open(FIL *fp, const TCHAR *path, BYTE mode)
{
    struct memfile *f;

    fp->node = NULL;
    fp->fptr = 0;
    fp->flag = 0;
    if (!powered)
        return FR_DISK_ERR;
    f = find(path);
    if (mode & FA_CREATE_NEW) {
        if (f)
            return FR_EXIST;
        if (!spend(1))
            return FR_DISK_ERR;
        f = create(path);
    } else if (mode & FA_CREATE_ALWAYS) {
        if (!spend(1))
            return FR_DISK_ERR;
        if (!f)
            f = create(path);
        f->size = 0;
    } else if (mode & FA_OPEN_ALWAYS) {
        if (!f) {
            if (!spend(1))
                return FR_DISK_ERR;
            f = create(path);
        }
    } else if (!f) {
        return FR_NO_FILE;
    }
    f->opens++;
    fp->node = f;
    fp->flag = mode & (FA_READ | FA_WRITE);
    if ((mode & FA_OPEN_APPEND) == FA_OPEN_APPEND)
        fp->fptr = f->size;
    return FR_OK;
}

FRESULT f_close(FIL *fp)
{
    if (!fp->node)
        return FR_INVALID_OBJECT;
    if (fp->node->opens > 0)
        fp->node->opens--;
    fp->node = NULL;
    return powered ? FR_OK : FR_DISK_ERR;
}

FRESULT f_read(FIL *fp, void *buff, UINT btr, UINT *br)
{
    struct memfile *f = fp->node;
    uint32_t n;

    *br = 0;
    if (!f)
        return FR_INVALID_OBJECT;
    if (!powered)
        return FR_DISK_ERR;
    if (!(fp->flag & FA_READ))
        return FR_DENIED;
    n = fp->fptr < f->size ? f->size - fp->fptr : 0;
    if (n > btr)
        n = btr;
    if (n)
        memcpy(buff, f->data + fp->fptr, n);
    fp->fptr += n;
    *br = n;
    return FR_OK;
}

FRESULT f_write(FIL *fp, const void *buff, UINT btw, UINT *bw)
{
    struct memfile *f = fp->node;
    uint32_t n = btw, end;

    *bw = 0;
    if (!f)
        return FR_INVALID_OBJECT;
    if (!powered)
        return FR_DISK_ERR;
    if (!(fp->flag & FA_WRITE))
        return FR_DENIED;
    if (!btw)
        return FR_OK;
    if (budget != MEMFS_UNLIMITED && budget < btw) {
        n = budget;                     /* the power goes in this write */
        used_units += n;
        budget = 0;
        powered = false;
    } else {
        spend(btw);
    }
    end = fp->fptr + (powered || !junk ? n : btw);
    grow(f, end);
    if (n)
        memcpy(f->data + fp->fptr, buff, n);
    if (!powered && junk) {
        uint32_t s = junk_seed;

        for (uint32_t i = fp->fptr + n; i < end; i++) {
            s ^= s << 13;
            s ^= s >> 17;
            s ^= s << 5;
            f->data[i] = (uint8_t)s;
        }
    }
    if (end > f->size)
        f->size = end;
    fp->fptr += n;
    *bw = n;
    return powered ? FR_OK : FR_DISK_ERR;
}

FRESULT f_lseek(FIL *fp, FSIZE_t ofs)
{
    struct memfile *f = fp->node;

    if (!f)
        return FR_INVALID_OBJECT;
    if (!powered)
        return FR_DISK_ERR;
    if (ofs > f->size) {
        if (fp->flag & FA_WRITE) {      /* FatFs extends a file opened for writing */
            grow(f, ofs);
            memset(f->data + f->size, 0, ofs - f->size);
            f->size = ofs;
        } else {
            ofs = f->size;
        }
    }
    fp->fptr = ofs;
    return FR_OK;
}

FRESULT f_truncate(FIL *fp)
{
    if (!fp->node)
        return FR_INVALID_OBJECT;
    if (!(fp->flag & FA_WRITE))
        return FR_DENIED;
    if (!spend(1))
        return FR_DISK_ERR;
    if (fp->fptr < fp->node->size)
        fp->node->size = fp->fptr;
    return FR_OK;
}

FRESULT f_sync(FIL *fp)
{
    if (!fp->node)
        return FR_INVALID_OBJECT;
    return powered ? FR_OK : FR_DISK_ERR;
}

FRESULT f_stat(const TCHAR *path, FILINFO *fno)
{
    struct memfile *f;

    if (!powered)
        return FR_DISK_ERR;
    if (!(f = find(path)))
        return FR_NO_FILE;
    if (fno) {
        const char *base = strrchr(f->name, '/');

        memset(fno, 0, sizeof(*fno));
        fno->fsize = f->size;
        fno->fattrib = AM_ARC;
        snprintf(fno->fname, sizeof(fno->fname), "%s", base ? base + 1 : f->name);
    }
    return FR_OK;
}

FRESULT f_unlink(const TCHAR *path)
{
    struct memfile *f;

    if (!powered)
        return FR_DISK_ERR;
    if (!(f = find(path)))
        return FR_NO_FILE;
    if (!spend(1))
        return FR_DISK_ERR;
    f->used = false;
    return FR_OK;
}

FRESULT f_rename(const TCHAR *path_old, const TCHAR *path_new)
{
    struct memfile *f;

    if (!powered)
        return FR_DISK_ERR;
    if (!(f = find(path_old)))
        return FR_NO_FILE;
    if (find(path_new))
        return FR_EXIST;
    if (!spend(1))
        return FR_DISK_ERR;
    snprintf(f->name, NAME, "%s", norm(path_new));
    return FR_OK;
}

FRESULT f_mkdir(const TCHAR *path)
{
    (void)path;
    return powered ? FR_OK : FR_DISK_ERR;
}

FRESULT f_opendir(DIR *dp, const TCHAR *path)
{
    if (!powered)
        return FR_DISK_ERR;
    dp->index = 0;
    dp->dir = norm(path);
    return FR_OK;
}

FRESULT f_readdir(DIR *dp, FILINFO *fno)
{
    size_t dl = strlen(dp->dir);

    memset(fno, 0, sizeof(*fno));
    if (!powered)
        return FR_DISK_ERR;
    for (; dp->index < FILES; dp->index++) {
        struct memfile *f = &files[dp->index];

        if (f->used && !strncmp(f->name, dp->dir, dl) && f->name[dl] == '/' &&
            !strchr(f->name + dl + 1, '/')) {
            snprintf(fno->fname, sizeof(fno->fname), "%s", f->name + dl + 1);
            fno->fsize = f->size;
            fno->fattrib = AM_ARC;
            dp->index++;
            return FR_OK;
        }
    }
    return FR_OK;                       /* fname "": the end */
}

FRESULT f_closedir(DIR *dp)
{
    (void)dp;
    return FR_OK;
}

/* ---- the kernel, the rest of the OS ------------------------------------------------ */

uint32_t host_ms;
unsigned host_kicks;
#define LOG_MAX (16 * 1024)
char host_log[LOG_MAX];
unsigned host_log_lines;

uint32_t kernel_ticks(void)
{
    return host_ms;
}

uint32_t kernel_cycles(void)
{
    return host_ms * 26000u;
}

void mutex_init(mutex_t *m)
{
    memset(m, 0, sizeof(*m));
}

int mutex_lock(mutex_t *m, uint32_t timeout_ms)
{
    (void)timeout_ms;
    m->depth++;
    m->owner = (thread_t *)(uintptr_t)1;
    return WAIT_OK;
}

void mutex_unlock(mutex_t *m)
{
    if (!m->depth) {
        host_fail(__FILE__, __LINE__, "mutex_unlock", "unlock of a mutex not held");
        return;
    }
    if (!--m->depth)
        m->owner = NULL;
}

void sem_init(sem_t *s, int count)
{
    memset(s, 0, sizeof(*s));
    s->count = count;
}

int sem_wait(sem_t *s, uint32_t timeout_ms)
{
    if (s->count > 0) {
        s->count--;
        return WAIT_OK;
    }
    if (timeout_ms != WAIT_FOREVER)
        host_ms += timeout_ms;
    return WAIT_TIMEOUT;
}

void sem_post(sem_t *s)
{
    s->count++;
}

thread_t *thread_create(const char *name, int prio, size_t stack_size, thread_fn fn, void *arg)
{
    (void)name, (void)prio, (void)stack_size, (void)fn, (void)arg;
    return NULL;                        /* one thread: nothing else runs */
}

void thread_sleep(uint32_t ms)
{
    host_ms += ms;
}

void thread_yield(void)
{
}

void thread_exit(void)
{
    fprintf(stderr, "thread_exit called in a host test\n");
    exit(3);
}

thread_t *thread_self(void)
{
    return NULL;
}

void cpufreq_hold(void)
{
}

void cpufreq_release(void)
{
}

void klog(const char *fmt, ...)
{
    char line[KLOG_LINE + 2];
    size_t have = strlen(host_log), n;
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(line, KLOG_LINE, fmt, ap);
    va_end(ap);
    strcat(line, "\n");
    n = strlen(line);
    if (have + n + 1 > LOG_MAX) {       /* keep the newer half */
        memmove(host_log, host_log + LOG_MAX / 2, have - LOG_MAX / 2 + 1);
        have = strlen(host_log);
    }
    memcpy(host_log + have, line, n + 1);
    host_log_lines++;
    if (getenv("HOST_KLOG"))
        fprintf(stderr, "klog: %s", line);
}

void host_log_clear(void)
{
    host_log[0] = 0;
    host_log_lines = 0;
}

bool host_log_has(const char *text)
{
    return strstr(host_log, text) != NULL;
}

void fs_kick(void)
{
    host_kicks++;
}

static fs_info_t info = { .mounted = true, .fs_type = 3 };

const fs_info_t *fs_info(void)
{
    return &info;
}

void keypad_inject(uint8_t code, bool down)
{
    (void)code, (void)down;
}

void ui_refresh(void)
{
}
