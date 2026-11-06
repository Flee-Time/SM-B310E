/* B310E OS — mount the card, the card log.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdarg.h>
#include "os/drivers.h"
#include "os/fs.h"
#include "os/kernel.h"
#include "os/lib.h"
#include "os/status.h"

#define LOG_BUF      4096
#define LOG_ROTATE   (256u * 1024)

static FATFS fat;
static fs_info_t info;
static char log_buf[LOG_BUF];
static size_t log_len;
static bool log_dropped;
static mutex_t log_lock;
static sem_t log_kick;

const fs_info_t *fs_info(void)
{
    return &info;
}

void klog(const char *fmt, ...)
{
    char line[160];
    va_list ap;
    uint32_t t = kernel_ticks();
    int n = snprintf(line, sizeof(line), "%5lu.%03lu ",
                     (unsigned long)(t / 1000), (unsigned long)(t % 1000));

    va_start(ap, fmt);
    n += vsnprintf(line + n, sizeof(line) - (size_t)n - 1, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof(line) - 2)
        n = sizeof(line) - 2;
    line[n++] = '\n';

    mutex_lock(&log_lock, WAIT_FOREVER);
    if (log_len + (size_t)n <= LOG_BUF) {
        memcpy(log_buf + log_len, line, (size_t)n);
        log_len += (size_t)n;
    } else {
        log_dropped = true;
    }
    mutex_unlock(&log_lock);
}

static void log_flush(void)
{
    static char out[LOG_BUF + 32];
    size_t len;
    FIL f;
    FILINFO st;
    UINT written;

    mutex_lock(&log_lock, WAIT_FOREVER);
    if (!info.mounted) {                /* keep them until it is back */
        mutex_unlock(&log_lock);
        return;
    }
    len = log_len;
    memcpy(out, log_buf, len);
    if (log_dropped) {
        memcpy(out + len, "-- lines dropped --\n", 20);
        len += 20;
        log_dropped = false;
    }
    log_len = 0;
    mutex_unlock(&log_lock);

    if (!len || !info.mounted)
        return;
    if (f_stat(OS_LOG, &st) == FR_OK && st.fsize > LOG_ROTATE) {
        f_unlink(OS_LOG_OLD);
        f_rename(OS_LOG, OS_LOG_OLD);
    }
    if (f_open(&f, OS_LOG, FA_WRITE | FA_OPEN_APPEND) == FR_OK) {
        f_write(&f, out, (UINT)len, &written);
        f_close(&f);
    }
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

static void log_main(void *arg)
{
    if (info.mounted && !info.free_known)
        count_free();
    for (;;) {
        sem_wait(&log_kick, 1000);
        log_flush();
    }
}

void fs_sync(void)
{
    log_flush();
}

/* Hand the card to someone else (USB mass storage): flush the log, stop
 * writing, unmount. Files must be closed first. fs_resume() mounts again;
 * the free count is read again from FSINFO (or counted on the next boot). */
void fs_suspend(void)
{
    if (!info.mounted)
        return;
    log_flush();
    mutex_lock(&log_lock, WAIT_FOREVER);
    info.mounted = false;               /* log_flush stops writing */
    mutex_unlock(&log_lock);
    f_mount(NULL, "", 0);
}

int fs_resume(void)
{
    if (info.mounted || info.sd_code)
        return info.mounted ? 0 : -1;
    info.mount_result = f_mount(&fat, "", 1);
    info.mounted = info.mount_result == FR_OK;
    if (info.mounted) {
        disk_set_readahead(fat.fatbase, (LBA_t)fat.fsize * fat.n_fats);
        if (fat.free_clst <= fat.n_fatent - 2)
            set_free(fat.free_clst);
    }
    return info.mounted ? 0 : -1;
}

int fs_init(void)
{
    uint32_t t = kernel_ticks();

    mutex_init(&log_lock);
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
        if (fat.free_clst <= fat.n_fatent - 2)  /* FSINFO's count is valid */
            set_free(fat.free_clst);
        f_mkdir(OS_DIR);
    }
    info.mount_ms = kernel_ticks() - t;
    thread_create("log", PRIO_LOW + 1, 2048, log_main, NULL);
    return info.mounted ? 0 : -1;
}
