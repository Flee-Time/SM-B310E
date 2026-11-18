/* B310E OS — boot self-test: kernel objects (sleep, semaphore, priority
 * inheritance, queue, heap) and a card write and read, reported in
 * os_status and the card log. It runs in its own thread behind the UI
 * (selftest_start), so Home comes up at once: until 2026-11-07 it ran
 * on the boot console first and the UI waited for it and 0.8 s more.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/arch.h"
#include "os/drivers.h"
#include "os/fs.h"
#include "os/kernel.h"
#include "os/lib.h"
#include "os/status.h"
#include "build_info.h"

/* self-test result bits */
#define ST_SLEEP   (1u << 0)
#define ST_SEM     (1u << 1)
#define ST_MUTEX   (1u << 2)
#define ST_QUEUE   (1u << 3)
#define ST_HEAP    (1u << 4)
#define ST_FS      (1u << 5)

/* ---- kernel self-test ----------------------------------------------------- */

static void report(uint32_t bit, bool ok, const char *what)
{
    os_status.selftest_done |= bit;
    if (ok)
        os_status.selftest_pass |= bit;
    else
        klog("self-test FAIL: %s", what);   /* the screen is the UI's now */
}

static sem_t ping, pong;
static volatile int pong_rounds;

static void pong_main(void *arg)
{
    for (int i = 0; i < 2000; i++) {
        sem_wait(&ping, WAIT_FOREVER);
        pong_rounds++;
        sem_post(&pong);
    }
}

/* Priority inheritance. Low takes the mutex and sleeps; high blocks on it,
 * which raises low to high's priority; then medium spins above the self-test
 * and the UI. With inheritance low (now above medium) wakes, works PI_WORK_MS
 * and lets go: high waits about PI_HOLD_MS + PI_WORK_MS. Without it medium
 * starves low until its own cap, PI_MEDIUM_MS, and high waits longer than
 * PI_PASS_MS: the check fails. Until 2026-11-18 medium was created before
 * high: it ran first and spun its whole 300 ms before high even existed, so
 * nothing was inherited (it passed only because low did its 50 ms after),
 * and boot began with ~87 M instructions of spinning above the UI. */
#define PI_HOLD_MS    5             /* low's sleep holding the mutex */
#define PI_WORK_MS    10            /* low's work after it */
#define PI_PASS_MS    40            /* high's wait, at most, with inheritance */
#define PI_MEDIUM_MS  100           /* medium's cap: well past PI_PASS_MS */

static mutex_t pi_mutex;
static volatile uint32_t pi_wait;
static volatile bool pi_done, pi_medium_stop, pi_held;

static void busy_ms(uint32_t ms)
{
    uint32_t end = kernel_ticks() + ms;

    while ((int32_t)(kernel_ticks() - end) < 0)
        ;
}

static void pi_low(void *arg)       /* holds the mutex through a sleep and work */
{
    mutex_lock(&pi_mutex, WAIT_FOREVER);
    pi_held = true;
    thread_sleep(PI_HOLD_MS);       /* high blocks, medium starts meanwhile */
    busy_ms(PI_WORK_MS);
    mutex_unlock(&pi_mutex);
}

static void pi_medium(void *arg)    /* would starve low without inheritance */
{
    uint32_t t0 = kernel_ticks();

    while (!pi_medium_stop && kernel_ticks() - t0 < PI_MEDIUM_MS)
        ;
}

static void pi_high(void *arg)
{
    uint32_t t0 = kernel_ticks();

    mutex_lock(&pi_mutex, WAIT_FOREVER);
    pi_wait = kernel_ticks() - t0;
    mutex_unlock(&pi_mutex);
    pi_medium_stop = true;
    pi_done = true;
}

void selftest_kernel(void)
{
    uint32_t t0, dt;

    /* sleep: 100 ms within 2 ms */
    t0 = kernel_ticks();
    thread_sleep(100);
    dt = kernel_ticks() - t0;
    report(ST_SLEEP, dt >= 100 && dt <= 102, "sleep 100 ms");

    /* semaphore ping-pong with a second thread */
    sem_init(&ping, 0);
    sem_init(&pong, 0);
    pong_rounds = 0;
    thread_create("pong", PRIO_NORMAL + 1, 1024, pong_main, NULL);
    t0 = kernel_ticks();
    for (int i = 0; i < 2000; i++) {
        sem_post(&ping);
        if (sem_wait(&pong, 100) != WAIT_OK)
            break;
    }
    dt = kernel_ticks() - t0;
    os_status.sem_rounds_per_s = dt ? 2000u * 1000u / dt : 0;
    report(ST_SEM, pong_rounds == 2000, "semaphore x2000");

    /* priority inheritance: low holds, high waits, medium spins (above) */
    mutex_init(&pi_mutex);
    pi_done = pi_medium_stop = pi_held = false;
    thread_create("pi-low", PRIO_LOW, 1024, pi_low, NULL);
    /* low runs below the UI: wait until it has the mutex (a busy UI could
     * keep it from it past a fixed 2 ms, and high would then take the mutex
     * free, proving nothing) */
    t0 = kernel_ticks();
    while (!pi_held && kernel_ticks() - t0 < 500)
        thread_sleep(1);
    /* high first: each preempts this thread at once, so high is blocked on
     * the mutex (low raised) before medium spins */
    thread_create("pi-high", PRIO_NORMAL + 4, 1024, pi_high, NULL);
    thread_create("pi-med", PRIO_NORMAL + 2, 1024, pi_medium, NULL);
    t0 = kernel_ticks();
    while (!pi_done && kernel_ticks() - t0 < 500)
        thread_sleep(5);
    pi_medium_stop = true;
    os_status.pi_wait_ms = pi_wait;
    report(ST_MUTEX, pi_held && pi_done && pi_wait < PI_PASS_MS,
           pi_held ? "mutex inherit" : "mutex inherit: low never ran");

    /* queue: 100 messages in order through a 4-slot queue */
    {
        queue_t q;
        bool ok = queue_init(&q, sizeof(uint32_t), 4) == 0;

        for (uint32_t i = 0; ok && i < 100; i++) {
            uint32_t v;
            ok = queue_send(&q, &i, 10) == WAIT_OK &&
                 queue_recv(&q, &v, 10) == WAIT_OK && v == i;
        }
        kfree(q.buf);
        report(ST_QUEUE, ok, "queue order");
    }

    /* heap: free bytes come back after a mixed alloc/free pattern */
    {
        size_t before = heap_free_bytes();
        void *p[16];

        for (int i = 0; i < 16; i++)
            p[i] = kmalloc(32u << (i % 6));
        for (int i = 0; i < 16; i += 2)
            kfree(p[i]);
        for (int i = 1; i < 16; i += 2)
            kfree(p[i]);
        report(ST_HEAP, heap_free_bytes() == before, "heap reuse");
    }
}

/* Card: write a 32 KB pattern file, read it back, compare. */
#define FS_TEST_FILE OS_DIR "/selftest"  /* the OS's own: no extension */
#define FS_TEST_SIZE (32u * 1024)

void selftest_card(void)
{
    uint8_t *buf = kmalloc(FS_TEST_SIZE);
    bool ok = buf && fs_info()->mounted;
    uint32_t t0, dt;
    FIL f;
    UINT n;

    if (ok) {
        for (uint32_t i = 0; i < FS_TEST_SIZE; i++)
            buf[i] = (uint8_t)(i * 7 + (i >> 8));
        t0 = kernel_ticks();
        ok = f_open(&f, FS_TEST_FILE, FA_WRITE | FA_CREATE_ALWAYS) == FR_OK;
        ok = ok && f_write(&f, buf, FS_TEST_SIZE, &n) == FR_OK && n == FS_TEST_SIZE;
        ok = (f_close(&f) == FR_OK) && ok;
        dt = kernel_ticks() - t0;
        os_status.fs_write_kib_per_s = dt ? 32000u / dt : 0;
    }
    if (ok) {
        memset(buf, 0, FS_TEST_SIZE);
        t0 = kernel_ticks();
        ok = f_open(&f, FS_TEST_FILE, FA_READ) == FR_OK;
        ok = ok && f_read(&f, buf, FS_TEST_SIZE, &n) == FR_OK && n == FS_TEST_SIZE;
        f_close(&f);
        dt = kernel_ticks() - t0;
        os_status.fs_read_kib_per_s = dt ? 32000u / dt : 0;
        for (uint32_t i = 0; ok && i < FS_TEST_SIZE; i++)
            ok = buf[i] == (uint8_t)(i * 7 + (i >> 8));
    }
    kfree(buf);
    report(ST_FS, ok, "card write+read");
    klog("self-test card %s, %lu KB/s write, %lu KB/s read", ok ? "ok" : "FAIL",
         (unsigned long)os_status.fs_write_kib_per_s,
         (unsigned long)os_status.fs_read_kib_per_s);
}

static volatile bool running;

static void selftest_main(void *arg)
{
    selftest_kernel();
    selftest_card();
    running = false;
}

/* Above the UI's priority, so the timing checks (sleep within 2 ms, the
 * inheritance wait) hold; the tests mostly sleep. Not at every boot since
 * 2026-11-17 (main.c): it wrote the card 32 KB a boot, ran five threads and
 * left its buffer as a hole in the heap (11 small free pieces, 35.7 KB). */
bool selftest_start(void)
{
    uint32_t s = arch_irq_save();
    bool busy = running;

    running = true;
    arch_irq_restore(s);
    if (busy)
        return false;
    os_status.selftest_done = os_status.selftest_pass = 0;  /* this run's */
    if (!thread_create("selftest", PRIO_NORMAL + 1, 2048, selftest_main, NULL)) {
        running = false;
        return false;
    }
    return true;
}

bool selftest_running(void)
{
    return running;
}

void selftest_show_card(void)
{
    const fs_info_t *fi = fs_info();
    static const char *const types[] = { "?", "FAT12", "FAT16", "FAT32", "exFAT" };

    os_status.sd_code = (uint32_t)fi->sd_code;
    os_status.fs_mounted = fi->mounted;
    os_status.fs_free_kib = fi->free_kib;
    /* No console line for a mounted card: nothing sends it before Home is
     * drawn over it (the log line below has it). The failures stay. */
    if (fi->mounted) {
        if (fi->free_known)
            klog("boot: B310E OS built %s; card %lu MB %s, %lu MB free; mount %lu ms",
                 BUILD_DATE, (unsigned long)(fi->sectors / 2048),
                 types[fi->fs_type <= 4 ? fi->fs_type : 0],
                 (unsigned long)(fi->free_kib / 1024), (unsigned long)fi->mount_ms);
        else
            klog("boot: B310E OS built %s; card %lu MB %s, free space not in FSINFO; mount %lu ms",
                 BUILD_DATE, (unsigned long)(fi->sectors / 2048),
                 types[fi->fs_type <= 4 ? fi->fs_type : 0], (unsigned long)fi->mount_ms);
    } else if (fi->sd_code) {
        console_printf("no card (step %d)\n", fi->sd_code);
    } else {
        console_printf("card not FAT (%d)\n", (int)fi->mount_result);
    }
}

