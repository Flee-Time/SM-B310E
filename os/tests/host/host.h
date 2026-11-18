/* Host tests (os/tests/test-host.py): the stand-ins the OS files run on in
 * a host program, and a few check helpers. host.c implements them:
 *
 *   guard allocator  kmalloc/kzalloc/kfree with 32 guard bytes on each side,
 *                    checked at every free and by ga_check(); a double free
 *                    or a free of something never allocated stops the
 *                    program; freed blocks wait in a quarantine (their bytes
 *                    poisoned, checked later) so a write after free is seen;
 *                    ga_fail_at(n) makes the n-th allocation from now fail.
 *   memfs            the FatFs calls (ff.h) on files in RAM, a flat name
 *                    space (directories always "exist"), with a power-cut
 *                    budget: every byte written costs 1, every rename,
 *                    unlink, truncate or file creation 1; the operation that
 *                    runs out does part of a write (or none of the others),
 *                    and from then on every call fails until memfs_power_on.
 *   kernel           one thread: mutexes count their depth only, a
 *                    semaphore is a counter, kernel_ticks() is host_ms (the
 *                    tests move it), thread_create runs nothing.
 *   klog             into a buffer (host_log), printed when a check fails.
 *
 * The UBSan checks are built in trap mode (no runtime on this host's
 * toolchain): a trap, an access violation or a guard failure prints where
 * (an address the runner passes to addr2line) and what the test was doing
 * (host_context), saves host_dump's bytes if set, and exits with 70.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HOST_H
#define HOST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- checks ------------------------------------------------------------- */
extern int host_failures;
void host_fail(const char *file, int line, const char *what, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));
#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        if (!(cond))                                                          \
            host_fail(__FILE__, __LINE__, #cond, __VA_ARGS__);                \
    } while (0)
/* What the test is doing, shown with a failure or a crash */
void host_context(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* The input to save (to the file named by env HOST_CRASH_FILE) on a crash */
void host_dump(const void *data, size_t n);
void host_init(int argc, char **argv);  /* first in main: crash handlers, options */
int host_done(void);                    /* last: the exit code */
extern bool host_long;                  /* --long */
extern bool host_prove;                 /* built with -DHOST_PROVE: break a reference */
void host_hexdump(const char *label, const void *p, size_t n);

/* n (<= 4096) bytes whose end touches a page that cannot be read: a read
 * past the end is an access violation. One buffer, reused by every call. */
void *host_edge(size_t n);

/* a small fast PRNG, the same sequence on every host */
uint32_t host_rand(void);
void host_srand(uint32_t seed);

/* ---- guard allocator ------------------------------------------------------ */
void ga_reset(void);                    /* everything freed, counters to 0 */
void ga_free_all(void);                 /* drop every live block (a reboot) */
void ga_fail_at(unsigned n);            /* the n-th allocation from now fails; 0 off */
unsigned ga_count(void);                /* allocations so far (failed ones too) */
unsigned ga_live(void);                 /* blocks allocated and not freed */
size_t ga_live_bytes(void);
void ga_check(void);                    /* every live block's guards, the quarantine */
bool ga_failed(void);                   /* the failure set by ga_fail_at(n) happened
                                         * (ga_fail_at(0) turns it off, keeps this) */

/* ---- memfs ----------------------------------------------------------------- */
#define MEMFS_UNLIMITED 0xffffffffu
void memfs_reset(void);                 /* no files, power on, unlimited budget */
void memfs_budget(uint32_t units);      /* units left before the power goes */
uint32_t memfs_used(void);              /* units spent since memfs_reset */
bool memfs_powered(void);
/* the power cut: a torn write's missing bytes are left as they were (clean),
 * or the file's size includes them and they hold junk */
void memfs_set_junk(bool junk, uint32_t seed);
void memfs_power_on(void);              /* reboot: calls work again, unlimited */
bool memfs_exists(const char *path);
/* a file's bytes (NULL when missing); valid until the next memfs call */
const uint8_t *memfs_data(const char *path, uint32_t *size);
void memfs_put(const char *path, const void *data, uint32_t size);
void memfs_remove(const char *path);
int memfs_open_count(void);             /* files open now (a leak shows) */
/* a copy of every file, and back (a snapshot of the card) */
typedef struct memfs_image memfs_image_t;
memfs_image_t *memfs_save(void);
void memfs_restore(const memfs_image_t *img);
void memfs_image_free(memfs_image_t *img);
bool memfs_same(const memfs_image_t *img);  /* the card is as img */

/* ---- kernel, log ----------------------------------------------------------- */
extern uint32_t host_ms;                /* kernel_ticks() */
extern char host_log[];                 /* klog's lines, the last 16 KB */
extern unsigned host_log_lines;
void host_log_clear(void);
bool host_log_has(const char *text);
extern unsigned host_kicks;             /* fs_kick calls */

#endif
