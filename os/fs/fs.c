/* B310E OS — mount the card, the card log.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdarg.h>
#include "os/db.h"
#include "os/drivers.h"
#include "os/fs.h"
#include "os/kernel.h"
#include "os/lib.h"
#include "os/status.h"

#define LOG_BUF      4096
#define LOG_ROTATE   (256u * 1024)
#define LOG_TAIL     8192               /* the last lines, for klog_read() */
#define LOG_DELAY_MS 1000               /* lines gathered this long: one card write */

static FATFS fat;
static fs_info_t info;
static char log_buf[LOG_BUF];
static size_t log_len;
static bool log_dropped;
static uint32_t log_first;              /* when the oldest line waiting came */
static mutex_t log_lock;
static sem_t log_kick;
static char log_tail[LOG_TAIL];
static uint32_t tail_pos;               /* bytes ever logged */
static char log_line[KLOG_LINE];        /* klog's, under log_lock */

const fs_info_t *fs_info(void)
{
    return &info;
}

/* Formatted under the lock into one static line: the line no longer sits on
 * the caller's stack (threads with 768-byte stacks log), so it can be long
 * enough for the media "done" line, which lost its tail at 160. */
void klog(const char *fmt, ...)
{
    va_list ap;
    uint32_t t = kernel_ticks();
    bool kick;
    int n;

    mutex_lock(&log_lock, WAIT_FOREVER);
    n = snprintf(log_line, KLOG_LINE, "%5lu.%03lu ",
                 (unsigned long)(t / 1000), (unsigned long)(t % 1000));
    va_start(ap, fmt);
    n += vsnprintf(log_line + n, KLOG_LINE - (size_t)n - 1, fmt, ap);
    va_end(ap);
    if (n > KLOG_LINE - 2)
        n = KLOG_LINE - 2;
    log_line[n++] = '\n';

    for (int i = 0; i < n; i++)
        log_tail[tail_pos++ % LOG_TAIL] = log_line[i];
    /* the log thread sleeps with nothing to write: the first line wakes it, and
     * one that fills half the buffer (a burst) has it write at once */
    kick = !log_len || (log_len < LOG_BUF / 2 && log_len + (size_t)n >= LOG_BUF / 2);
    if (!log_len)
        log_first = t;
    if (log_len + (size_t)n <= LOG_BUF) {
        memcpy(log_buf + log_len, log_line, (size_t)n);
        log_len += (size_t)n;
    } else {
        log_dropped = true;
    }
    mutex_unlock(&log_lock);
    if (kick)
        sem_post(&log_kick);
}

void fs_kick(void)
{
    sem_post(&log_kick);
}

/* What was logged since *pos (from the last LOG_TAIL bytes; older lines
 * are skipped), at most n bytes; *pos moves on. */
size_t klog_read(uint32_t *pos, char *buf, size_t n)
{
    size_t k;

    mutex_lock(&log_lock, WAIT_FOREVER);
    if (tail_pos - *pos > LOG_TAIL)
        *pos = tail_pos - LOG_TAIL;
    k = tail_pos - *pos;
    if (k > n)
        k = n;
    for (size_t i = 0; i < k; i++)
        buf[i] = log_tail[(*pos + i) % LOG_TAIL];
    *pos += (uint32_t)k;
    mutex_unlock(&log_lock);
    return k;
}

uint32_t klog_pos(void)
{
    return tail_pos;
}

/* One at a time, the log thread's and fs_sync's: out is shared, and a second f_open of
 * the log fails. fs_suspend holds this through the unmount, so no flush that had already
 * passed the check can write the card once USB has it. Lines leave log_buf only once
 * they are written: a failed open or write loses nothing, unless the buffer fills. */
static mutex_t flush_lock;
static uint32_t log_retry_at;

static void log_flush(void)
{
    static char out[LOG_BUF + 32];
    size_t len, taken;
    bool marker = false, ok = false;
    FIL f;
    FILINFO st;
    UINT written = 0;

    mutex_lock(&flush_lock, WAIT_FOREVER);
    mutex_lock(&log_lock, WAIT_FOREVER);
    if (!info.mounted) {                /* keep them until it is back */
        mutex_unlock(&log_lock);
        mutex_unlock(&flush_lock);
        return;
    }
    taken = len = log_len;
    memcpy(out, log_buf, len);
    if (log_dropped) {
        memcpy(out + len, "-- lines dropped --\n", 20);
        len += 20;
        log_dropped = false;
        marker = true;
    }
    mutex_unlock(&log_lock);

    if (!len) {
        mutex_unlock(&flush_lock);
        return;
    }
    if (f_stat(OS_LOG, &st) == FR_OK && st.fsize > LOG_ROTATE) {
        f_unlink(OS_LOG_OLD);
        f_rename(OS_LOG, OS_LOG_OLD);
    }
    if (f_open(&f, OS_LOG, FA_WRITE | FA_OPEN_APPEND) == FR_OK) {
        ok = f_write(&f, out, (UINT)len, &written) == FR_OK && written == len;
        ok = f_close(&f) == FR_OK && ok;
    }
    mutex_lock(&log_lock, WAIT_FOREVER);
    /* what did reach the card leaves; written can pass taken by the marker */
    if (written > taken)
        written = (UINT)taken;
    if (ok)
        written = (UINT)taken;
    memmove(log_buf, log_buf + written, log_len - written);
    log_len -= written;
    if (!ok && marker)
        log_dropped = true;
    /* a full or failing card: each try is a hold and a PLL start, so not every second */
    log_retry_at = ok ? 0 : kernel_ticks() + 60000u;
    mutex_unlock(&log_lock);
    mutex_unlock(&flush_lock);
}

/* FSINFO as the FAT32 spec lays it out. FatFs R0.15a looks for the trailing
 * signature at offset 498 (FSI_TrailSig, and writes it there), not at 508,
 * where the spec, Windows, mkfs.fat and our card-image tool put it: so it
 * takes the free count only from an FSINFO it wrote itself, and walked the
 * FAT on any card last written elsewhere (QEMU's benchmark card at every
 * boot, 4.5 M instructions; the phone's card after a PC had it). Read here
 * with the spec's signatures. Only a possible count, and not 0 (a stale 0
 * would make FatFs refuse every allocation), is taken; the next free
 * cluster too, as FatFs takes it from its own. Before anything else uses
 * the volume, or under its lock. */
static void fsinfo_spec(void)
{
    static uint32_t w[128];             /* the sector as words (little endian); not on
                                         * the stack of whoever calls fs_resume */

    if (fat.fs_type != FS_FAT32 || fat.fsi_flag != 0 ||    /* 0: FSINFO next to the VBR */
        fat.free_clst <= fat.n_fatent - 2)                  /* FatFs took it already */
        return;
    if (sd_read(fat.volbase + 1, w, 1) != 0)
        return;
    if (w[0] != 0x41615252u || w[484 / 4] != 0x61417272u || w[508 / 4] != 0xaa550000u)
        return;
    if (w[488 / 4] == 0 || w[488 / 4] > fat.n_fatent - 2)
        return;
    fat.free_clst = w[488 / 4];
    if (w[492 / 4] >= 2 && w[492 / 4] < fat.n_fatent)
        fat.last_clst = w[492 / 4];
}

static void set_free(DWORD clusters)
{
    info.free_kib = clusters * fat.csize / 2;
    info.free_known = true;
    os_status.fs_free_kib = info.free_kib;
}

/* The card's FSINFO had no free count: walk the FAT (holds the volume
 * lock; a file operation meanwhile waits). FatFs then writes the count
 * back to FSINFO at the next file close, so this happens once. */
static void count_free(void)
{
    uint32_t t = kernel_ticks();
    DWORD clusters;
    FATFS *fs;

    if (f_getfree("", &clusters, &fs) != FR_OK)
        return;
    set_free(clusters);
    info.count_ms = kernel_ticks() - t;
    os_status.fs_count_ms = info.count_ms + 1;
    klog("card: %lu MB free (counted in %lu ms)",
         (unsigned long)(info.free_kib / 1024), (unsigned long)info.count_ms);
}

/* ms until the lines waiting should go to the card: a second after the first
 * (half a buffer: now), not before a failed write's retry; WAIT_FOREVER with
 * none, or no card (fs_resume kicks). */
static uint32_t log_due_ms(uint32_t now)
{
    int32_t left;

    mutex_lock(&log_lock, WAIT_FOREVER);
    if (!info.mounted || !log_len) {
        mutex_unlock(&log_lock);
        return WAIT_FOREVER;
    }
    left = log_len >= LOG_BUF / 2 ? 0 : (int32_t)(log_first + LOG_DELAY_MS - now);
    if (log_retry_at && (int32_t)(log_retry_at - now) > left)
        left = (int32_t)(log_retry_at - now);
    mutex_unlock(&log_lock);
    return left > 0 ? (uint32_t)left : 0;
}

/* Until 2026-11-17 this woke every second, with or without anything to do:
 * one of the ~4 wake-ups a second in dark idle. Now it sleeps until a line or
 * the store's next write is due (a kick from klog or the store, or the time). */
static void log_main(void *arg)
{
    if (info.mounted && !info.free_known)
        count_free();
    for (;;) {
        uint32_t log = log_due_ms(kernel_ticks()), store = db_next_ms();

        sem_wait(&log_kick, log < store ? log : store);
        db_tick();                      /* the store's settings, a second after a change */
        if (!log_due_ms(kernel_ticks()))
            log_flush();
    }
}

void fs_sync(void)
{
    db_sync();
    log_flush();
}

/* Hand the card to someone else (USB mass storage): write the store and the
 * log, stop writing, unmount. Files must be closed first. fs_resume() mounts
 * again; the free count is read again from FSINFO (or counted on the next boot). */
void fs_suspend(void)
{
    if (!info.mounted)
        return;
    db_suspend();
    log_flush();
    mutex_lock(&flush_lock, WAIT_FOREVER);
    mutex_lock(&log_lock, WAIT_FOREVER);
    info.mounted = false;               /* log_flush stops writing */
    mutex_unlock(&log_lock);
    /* f_mount takes no lock: wait for a FatFs call in progress (an app's,
     * or count_free's) so it cannot write the card once USB has it */
    ff_volume_lock(0);
    disk_suspend(true);
    f_mount(NULL, "", 0);
    ff_volume_unlock(0);
    mutex_unlock(&flush_lock);
}

int fs_resume(void)
{
    if (info.mounted || info.sd_code)
        return info.mounted ? 0 : -1;
    disk_suspend(false);
    info.mount_result = f_mount(&fat, "", 1);
    info.mounted = info.mount_result == FR_OK;
    if (info.mounted) {
        disk_set_readahead(fat.fatbase, (LBA_t)fat.fsize * fat.n_fats);
        ff_volume_lock(0);              /* a FatFs call may start once mounted */
        fsinfo_spec();                  /* as a PC left it */
        ff_volume_unlock(0);
        if (fat.free_clst <= fat.n_fatent - 2)
            set_free(fat.free_clst);
    }
    db_resume();                        /* away no more (or still: no card) */
    fs_kick();                          /* the lines kept meanwhile */
    return info.mounted ? 0 : -1;
}

int fs_init(void)
{
    uint32_t t = kernel_ticks();

    mutex_init(&log_lock);
    mutex_init(&flush_lock);
    sem_init(&log_kick, 0);
    info.sd_code = sd_init();
    if (info.sd_code == 0) {
        info.sectors = sd_sectors();
        info.mount_result = f_mount(&fat, "", 1);
        info.mounted = info.mount_result == FR_OK;
    }
    if (info.mounted) {
        info.fs_type = fat.fs_type;
        disk_set_readahead(fat.fatbase, (LBA_t)fat.fsize * fat.n_fats);
        f_getlabel("", info.label, NULL);
        fsinfo_spec();                  /* nothing else on the card yet */
        if (fat.free_clst <= fat.n_fatent - 2)  /* FSINFO's count is valid */
            set_free(fat.free_clst);
        f_mkdir(OS_DIR);
    }
    info.mount_ms = kernel_ticks() - t;
    thread_create("log", PRIO_LOW + 1, 4096, log_main, NULL);   /* the store's FIL too */
    return info.mounted ? 0 : -1;
}
