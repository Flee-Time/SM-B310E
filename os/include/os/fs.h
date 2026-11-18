/* B310E OS — storage: the SD card's FAT filesystem and the card log.
 *
 * Files use the FatFs API directly (f_open, f_read, f_write, f_close,
 * f_opendir, f_readdir, f_mkdir, f_unlink, f_rename, f_stat ...); paths
 * are UTF-8, the card is the only volume ("/" or "0:/").
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef OS_FS_H
#define OS_FS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "ff.h"

void disk_set_readahead(LBA_t first, LBA_t count);     /* fs/diskio.c */
void disk_suspend(bool on);                             /* fs/diskio.c */
void ff_volume_lock(int vol);                           /* fs/ffsystem.c */
void ff_volume_unlock(int vol);

/* The OS's own files have no extension: the log, its rotated copy, the data
 * store (os/db.h). What it makes for the user (recordings ...) keeps one. */
#define OS_DIR      "/b310e-os"
#define OS_LOG      OS_DIR "/log"
#define OS_LOG_OLD  OS_DIR "/log-old"

typedef struct {
    bool mounted;
    int sd_code;                /* sd_init() result */
    FRESULT mount_result;
    uint32_t sectors;           /* card size */
    uint32_t free_kib;          /* valid once free_known */
    bool free_known;            /* at mount from FSINFO, else counted later */
    uint32_t mount_ms;          /* card probe and mount */
    uint32_t count_ms;          /* free-space count, 0 when FSINFO had it */
    char label[24];
    uint8_t fs_type;            /* FS_FAT12/16/32 */
} fs_info_t;

/* Probe the card and mount it; 0 when mounted. When the card's FSINFO
 * has no valid free count, the log thread counts the FAT after boot. */
int fs_init(void);
const fs_info_t *fs_info(void);
void fs_sync(void);             /* write the store and the log; before power-off */
void fs_suspend(void);          /* write, stop the log, unmount (for USB) */
int fs_resume(void);            /* mount again; 0 when mounted */

/* The card log: lines with uptime (up to KLOG_LINE bytes each), buffered in
 * RAM, written by a low-priority thread a second after the first line waiting
 * (OS_LOG; rotated past 256 KB). The thread sleeps while there is nothing to
 * write: a line or the store (fs_kick) wakes it. */
#define KLOG_LINE   320
void klog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void fs_kick(void);             /* the log thread looks again at what is due (os/db.c) */
/* The last 8 KB of the log in RAM, card or not (the network shell's
 * "log"): bytes since *pos, which moves on; klog_pos() is the end now. */
size_t klog_read(uint32_t *pos, char *buf, size_t n);
uint32_t klog_pos(void);

#endif
