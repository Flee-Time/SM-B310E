/* The game kit: newlib's system calls over the OS (kit_os.h), so a game
 * gets the whole C library: files on the card (relative paths in the
 * game's folder), stdout and stderr to the OS log a line at a time, malloc
 * on the block the kit took for the game (_sbrk), exit() back to
 * kit_run(), the time from the RTC.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/times.h>
#include <unistd.h>
#include <dirent.h>                     /* the kit's (include/dirent.h) */

#include "kit.h"
#include "kit_os.h"

#define MAX_FILES 16
#define PATH_MAX_ 160

/* ---- memory: kit_malloc.c (the game's block as a heap of its own) ---------------- */
void *_sbrk(ptrdiff_t incr)
{
    (void)incr;
    errno = ENOMEM;                     /* nothing in newlib grows a heap here */
    return (void *)-1;
}

/* ---- the compiler's own: word at a time ------------------------------------------ */
void *memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;

    if (!(((uintptr_t)d ^ (uintptr_t)s) & 3)) {
        while (n && ((uintptr_t)d & 3)) {
            *d++ = *s++;
            n--;
        }
        for (; n >= 16; n -= 16, d += 16, s += 16) {
            uint32_t a = ((const uint32_t *)s)[0], b = ((const uint32_t *)s)[1];
            uint32_t c = ((const uint32_t *)s)[2], e = ((const uint32_t *)s)[3];

            ((uint32_t *)d)[0] = a;
            ((uint32_t *)d)[1] = b;
            ((uint32_t *)d)[2] = c;
            ((uint32_t *)d)[3] = e;
        }
        for (; n >= 4; n -= 4, d += 4, s += 4)
            *(uint32_t *)d = *(const uint32_t *)s;
    }
    while (n--)
        *d++ = *s++;
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    uint8_t *d = dst;
    uint32_t w = (uint8_t)c * 0x01010101u;

    while (n && ((uintptr_t)d & 3)) {
        *d++ = (uint8_t)c;
        n--;
    }
    for (; n >= 16; n -= 16, d += 16) {
        ((uint32_t *)d)[0] = w;
        ((uint32_t *)d)[1] = w;
        ((uint32_t *)d)[2] = w;
        ((uint32_t *)d)[3] = w;
    }
    for (; n >= 4; n -= 4, d += 4)
        *(uint32_t *)d = w;
    while (n--)
        *d++ = (uint8_t)c;
    return dst;
}

/* ---- files ------------------------------------------------------------------------ */
/* A file opened only for reading (a WAD, a ROM) reads through a cache of
 * CACHE bytes: a small read takes the CACHE bytes from its sector on in
 * one card command (the SD driver's largest), and the next small reads
 * are there already (a WAD's sprites, its patches, lie one after
 * another). A read of CACHE or more goes to the card as it is. Every card
 * command costs about a millisecond on the phone, whatever its size, and
 * then 16 KB about 5 ms, in the middle of a frame: a small read after a seek
 * (a tile, from anywhere in the file) takes what it needs, or SEEK_READ. */
#define CACHE (16 * 1024)
#define SEEK_READ (4 * 1024)

static struct {
    void *fil;                          /* the OS's open file, or NULL */
    int append;
    int cached;                         /* read only: through the cache */
    unsigned long pos;                  /* where the next read is (cached) */
    uint8_t *cache;                     /* CACHE bytes, or NULL until used */
    unsigned long cache_at;             /* the file offset of cache[0] */
    unsigned cache_n;                   /* bytes in it */
    unsigned long read_end;             /* where the last read ended */
} files[MAX_FILES];

/* the log: stdout and stderr, a line at a time */
static char line[2][120];
static unsigned line_n[2];
static char last_error[120];            /* stderr's last line: why it stopped */

static void log_flush(int i)
{
    if (line_n[i]) {
        line[i][line_n[i]] = 0;
        kit_log(line[i]);
        if (i == 1)
            memcpy(last_error, line[i], line_n[i] + 1);
        line_n[i] = 0;
    }
}

static void log_put(int i, const char *s, int n)
{
    for (int k = 0; k < n; k++) {
        if (s[k] == '\n' || line_n[i] >= sizeof(line[i]) - 1)
            log_flush(i);
        if ((unsigned char)s[k] >= ' ')     /* no backspaces, no CRs */
            line[i][line_n[i]++] = s[k];
    }
}

/* a path in the game's folder unless it starts at the root */
static const char *full_path(const char *path, char *buf)
{
    if (path[0] == '/')
        return path;
    while (path[0] == '.' && path[1] == '/')
        path += 2;
    snprintf(buf, PATH_MAX_, "%s/%s", kit_dir(), path);
    return buf;
}

static void *file_of(int fd)
{
    if (fd < 3 || fd >= MAX_FILES || !files[fd].fil) {
        errno = EBADF;
        return NULL;
    }
    return files[fd].fil;
}

int _open(const char *path, int flags, int mode)
{
    char buf[PATH_MAX_];
    int m = 0, fd;
    void *f;

    (void)mode;
    for (fd = 3; fd < MAX_FILES && files[fd].fil; fd++)
        ;
    if (fd == MAX_FILES) {
        errno = EMFILE;
        return -1;
    }
    switch (flags & O_ACCMODE) {
    case O_RDONLY: m = KOS_READ; break;
    case O_WRONLY: m = KOS_WRITE; break;
    default:       m = KOS_READ | KOS_WRITE; break;
    }
    if (flags & O_CREAT)
        m |= flags & O_EXCL ? KOS_NEW : flags & O_TRUNC ? KOS_CREATE : KOS_ALWAYS;
    else if (flags & O_TRUNC)
        m |= KOS_CREATE;
    f = malloc((size_t)kos_file_size());
    if (!f) {
        errno = ENOMEM;
        return -1;
    }
    if (kos_open(f, full_path(path, buf), m)) {
        free(f);
        errno = ENOENT;
        return -1;
    }
    files[fd].fil = f;
    files[fd].append = !!(flags & O_APPEND);
    files[fd].cached = (flags & O_ACCMODE) == O_RDONLY;
    files[fd].pos = 0;
    files[fd].cache = NULL;
    files[fd].cache_n = 0;
    return fd;
}

int _close(int fd)
{
    void *f;
    int r;

    if (fd < 3) {
        if (fd == 1 || fd == 2)
            log_flush(fd - 1);
        return 0;
    }
    if (!(f = file_of(fd)))
        return -1;
    r = kos_close(f);
    free(f);
    free(files[fd].cache);
    files[fd].cache = NULL;
    files[fd].fil = NULL;
    return r ? -1 : 0;
}

/* the card reads a game made: how many, how much (kit_io_report) */
static unsigned io_reads, io_kb, io_asked, io_ms;

unsigned kit_io_kb(void) { return io_kb; }
unsigned kit_io_ms(void) { return io_ms; }

static int card_read(void *f, unsigned long pos, void *buf, unsigned n, unsigned *got)
{
    uint32_t start = kit_ms();

    io_reads++;
    if (kos_seek(f, pos) || kos_read(f, buf, n, got))
        return -1;
    io_kb += (*got + 512) / 1024;
    io_ms += kit_ms() - start;
    return 0;
}

void kit_io_report(const char *when)
{
    char text[96];

    snprintf(text, sizeof(text), "%s: %u card reads, %u KB (%u reads asked)",
             when, io_reads, io_kb, io_asked);
    kit_log(text);
}

static int read_cached(int fd, void *f, char *buf, unsigned n)
{
    unsigned done = 0, got, want;

    while (done < n) {
        unsigned long pos = files[fd].pos;

        if (files[fd].cache_n && pos >= files[fd].cache_at &&
            pos < files[fd].cache_at + files[fd].cache_n) {
            unsigned have = (unsigned)(files[fd].cache_at + files[fd].cache_n - pos);
            unsigned k = n - done < have ? n - done : have;

            memcpy(buf + done, files[fd].cache + (pos - files[fd].cache_at), k);
            done += k;
            files[fd].pos += k;
            files[fd].read_end = files[fd].pos;
            continue;
        }
        if (n - done >= CACHE || (!files[fd].cache && !(files[fd].cache = malloc(CACHE)))) {
            got = 0;                    /* large (or no memory): as it is */
            if (card_read(f, pos, buf + done, n - done, &got))
                return done ? (int)done : -1;
            files[fd].pos += got;
            files[fd].read_end = files[fd].pos;
            return (int)(done + got);
        }
        files[fd].cache_at = pos & ~511ul;     /* from its sector on */
        files[fd].cache_n = 0;
        got = 0;
        want = CACHE;
        if (pos != files[fd].read_end) {
            want = (unsigned)(pos - files[fd].cache_at) + (n - done);
            want = want < SEEK_READ ? SEEK_READ : (want + 511) & ~511u;
            if (want > CACHE)
                want = CACHE;
        }
        if (card_read(f, files[fd].cache_at, files[fd].cache, want, &got))
            return done ? (int)done : -1;
        files[fd].cache_n = got;
        if (pos >= files[fd].cache_at + got)
            break;                      /* the end of the file */
    }
    return (int)done;
}

int _read(int fd, char *buf, int n)
{
    unsigned got = 0;
    void *f;
    int r;

    if (fd < 3)
        return 0;
    if (!(f = file_of(fd)))
        return -1;
    if (files[fd].cached) {
        io_asked++;
        if ((r = read_cached(fd, f, buf, (unsigned)n)) < 0)
            errno = EIO;
        return r;
    }
    if (kos_read(f, buf, (unsigned)n, &got)) {
        errno = EIO;
        return -1;
    }
    return (int)got;
}

int _write(int fd, const char *buf, int n)
{
    unsigned put = 0;
    void *f;

    if (fd == 1 || fd == 2) {
        log_put(fd - 1, buf, n);
        return n;
    }
    if (!(f = file_of(fd)))
        return -1;
    if (files[fd].append)
        kos_seek(f, kos_size(f));
    if (kos_write(f, buf, (unsigned)n, &put)) {
        errno = EIO;
        return -1;
    }
    return (int)put;
}

int _lseek(int fd, int off, int whence)
{
    void *f = file_of(fd);
    long pos;

    if (!f)
        return -1;
    if (files[fd].cached) {             /* the next read seeks */
        pos = whence == SEEK_CUR ? (long)files[fd].pos + off
            : whence == SEEK_END ? (long)kos_size(f) + off : off;
        if (pos < 0) {
            errno = EINVAL;
            return -1;
        }
        files[fd].pos = (unsigned long)pos;
        return (int)pos;
    }
    pos = whence == SEEK_CUR ? (long)kos_tell(f) + off
        : whence == SEEK_END ? (long)kos_size(f) + off : off;
    if (pos < 0 || kos_seek(f, (unsigned long)pos)) {
        errno = EINVAL;
        return -1;
    }
    return (int)pos;
}

int _fstat(int fd, struct stat *st)
{
    void *f;

    memset(st, 0, sizeof(*st));
    if (fd < 3) {
        st->st_mode = S_IFCHR;
        return 0;
    }
    if (!(f = file_of(fd)))
        return -1;
    st->st_mode = S_IFREG | 0666;
    st->st_size = (off_t)kos_size(f);
    st->st_blksize = 512;
    return 0;
}

int _stat(const char *path, struct stat *st)
{
    char buf[PATH_MAX_];
    unsigned long size;
    int dir;
    uint32_t mtime;

    if (kos_stat(full_path(path, buf), &size, &dir, &mtime)) {
        errno = ENOENT;
        return -1;
    }
    memset(st, 0, sizeof(*st));
    st->st_size = (off_t)size;
    st->st_mode = dir ? S_IFDIR | 0777 : S_IFREG | 0666;
    st->st_mtime = (time_t)mtime;
    return 0;
}

int stat(const char *path, struct stat *st) { return _stat(path, st); }

/* newlib's buffer (the link wraps fopen) cuts a read into 1 KB ones, each
 * a card command (a WAD's 64 KB lump: 64 of them): a file opened only for
 * reading has none (it has the cache above). A file written to gets one
 * of 4 KB: config files and saves are written a few bytes a call
 * (fprintf, a byte at a time), which would be a FatFs call each, and a
 * card write whenever a cluster fills. */
FILE *__real_fopen(const char *path, const char *mode);
size_t __real_fread(void *buf, size_t size, size_t count, FILE *f);

/* newlib-nano's fread on an unbuffered file reads a byte a call (no
 * read straight into the caller's buffer): 8 MB of a WAD would be 8
 * million calls. The link wraps it: a file read through the cache takes
 * the whole read in one, unless newlib holds a byte of it (ungetc). */
size_t __wrap_fread(void *buf, size_t size, size_t count, FILE *f)
{
    int fd = fileno(f), n;
    size_t want = size * count;

    if (fd < 3 || fd >= MAX_FILES || !files[fd].cached || f->_r > 0 || f->_ub._base ||
        !want)
        return __real_fread(buf, size, count, f);
    n = _read(fd, buf, (int)want);
    f->_flags &= ~__SOFF;               /* newlib's offset is stale: ftell asks _lseek */
    if (n < 0) {
        f->_flags |= __SERR;
        return 0;
    }
    if ((size_t)n < want)
        f->_flags |= __SEOF;
    return (size_t)n / size;
}

FILE *__wrap_fopen(const char *path, const char *mode)
{
    FILE *f = __real_fopen(path, mode);

    if (f) {
        if (mode[0] == 'r' && !strchr(mode, '+'))
            setvbuf(f, NULL, _IONBF, 0);
        else
            setvbuf(f, NULL, _IOFBF, 4096);
    }
    return f;
}
int _isatty(int fd) { return fd < 3; }

int _unlink(const char *path)
{
    char buf[PATH_MAX_];

    if (kos_unlink(full_path(path, buf))) {
        errno = ENOENT;
        return -1;
    }
    return 0;
}

int _link(const char *from, const char *to)
{
    (void)from;
    (void)to;
    errno = EMLINK;
    return -1;
}

/* FatFs does not replace: the target goes first */
int rename(const char *from, const char *to)
{
    char a[PATH_MAX_], b[PATH_MAX_];
    const char *pa = full_path(from, a), *pb = full_path(to, b);

    kos_unlink(pb);
    if (kos_rename(pa, pb)) {
        errno = ENOENT;
        return -1;
    }
    return 0;
}

int _rename(const char *from, const char *to) { return rename(from, to); }

int mkdir(const char *path, mode_t mode)
{
    char buf[PATH_MAX_];

    (void)mode;
    if (kos_mkdir(full_path(path, buf))) {
        errno = EEXIST;
        return -1;
    }
    return 0;
}

int access(const char *path, int mode)
{
    struct stat st;

    (void)mode;
    return _stat(path, &st);
}

int fsync(int fd)
{
    void *f = file_of(fd);

    return f && !kos_sync(f) ? 0 : -1;
}

/* ---- directories (the kit's dirent.h) --------------------------------------------- */
struct kit_dir {
    struct dirent ent;
    unsigned char fdir[];               /* FatFs' DIR */
};

DIR *opendir(const char *path)
{
    char buf[PATH_MAX_];
    DIR *d = malloc(sizeof(*d) + (size_t)kos_dir_size());

    if (!d) {
        errno = ENOMEM;
        return NULL;
    }
    if (kos_opendir(d->fdir, full_path(path, buf))) {
        free(d);
        errno = ENOENT;
        return NULL;
    }
    return d;
}

struct dirent *readdir(DIR *d)
{
    int dir;

    if (kos_readdir(d->fdir, d->ent.d_name, sizeof(d->ent.d_name), &dir))
        return NULL;
    d->ent.d_ino = 1;
    d->ent.d_type = dir ? DT_DIR : DT_REG;
    return &d->ent;
}

int closedir(DIR *d)
{
    kos_closedir(d->fdir);
    free(d);
    return 0;
}

/* ---- the process ------------------------------------------------------------------ */
int _getpid(void) { return 1; }

int _kill(int pid, int sig)
{
    (void)pid;
    (void)sig;
    errno = EINVAL;
    return -1;
}

int _gettimeofday(struct timeval *tv, void *tz)
{
    (void)tz;
    if (tv) {
        tv->tv_sec = (time_t)kos_time();
        tv->tv_usec = (suseconds_t)(kit_ms() % 1000) * 1000;
    }
    return 0;
}

clock_t _times(struct tms *t)
{
    clock_t now = (clock_t)kit_ms();

    if (t) {
        t->tms_utime = now;
        t->tms_stime = t->tms_cutime = t->tms_cstime = 0;
    }
    return now;
}

/* Only the card's side: the game may have been stopped inside its malloc */
void kit_close_files(void)
{
    for (int fd = 3; fd < MAX_FILES; fd++)
        if (files[fd].fil) {
            kos_close(files[fd].fil);
            files[fd].fil = NULL;
        }
}

static jmp_buf exit_jmp;
static int exit_code;

/* newlib's exit() ran the atexit handlers and flushed stdio */
void _exit(int code)
{
    kit_io_report("at the end");
    for (int fd = 0; fd < MAX_FILES; fd++)
        if (fd >= 3 ? files[fd].fil != NULL : fd > 0)
            _close(fd);
    if (code && last_error[0])
        kos_error(last_error);          /* shown once the game has stopped */
    exit_code = code;
    longjmp(exit_jmp, 1);
}

/* C++'s static constructors (sdk/app.ld keeps them): Wolf4SDL's DBOPL
 * fills its tables in them */
extern void (*__init_array_start[])(void), (*__init_array_end[])(void);

int kit_run(int argc, char **argv)
{
    for (void (**f)(void) = __init_array_start; f < __init_array_end; f++)
        (*f)();
    if (setjmp(exit_jmp))
        return exit_code;
    exit(kit_main(argc, argv));
}
