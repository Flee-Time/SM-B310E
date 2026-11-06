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
#include <stdint.h>
#include "ff.h"

void disk_set_readahead(LBA_t first, LBA_t count);     /* fs/diskio.c */

#define OS_DIR      "/b310e-os"
#define OS_LOG      OS_DIR "/log.txt"
#define OS_LOG_OLD  OS_DIR "/log-old.txt"

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
void fs_sync(void);             /* flush the log; before power-off */
void fs_suspend(void);          /* flush, stop the log, unmount (for USB) */
int fs_resume(void);            /* mount again; 0 when mounted */

/* The card log: lines with uptime, buffered in RAM, written once a
 * second by a low-priority thread (OS_LOG; rotated past 256 KB). */
void klog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif
