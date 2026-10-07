/* File operations used by game saves. SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <string.h>
#include "fatfile.h"

static int fs_errno;
int *__errno(void) { return &fs_errno; }

int mkdir(const char *name, int mode)
{
    (void)mode;
    fatdata_t *fat = &fatdata_glob;
    fat_entry_t *p = fat_find_path(fat, name);
    if (p) return p->entry.attr & FAT_ATTR_DIR ? 0 : -1;
    if (!fat->lastname) return -1;
    unsigned result = fat_make_dir(fat, fat->lastdir, fat->lastname);
    fat_flush_buf1(fat);
    fat_flush_buf2(fat);
    return result && !(fat->flags & FAT_IO_ERROR) ? 0 : -1;
}

int remove(const char *name)
{
    fatdata_t *fat = &fatdata_glob;
    fat_entry_t *p = fat_find_path(fat, name);
    if (!p) return -1;
    if (p->entry.attr & FAT_ATTR_DIR) return -1;
    fat_delete_entry(fat, p);
    return fat->flags & FAT_IO_ERROR ? -1 : 0;
}

int rename(const char *old, const char *name)
{
    fatdata_t *fat = &fatdata_glob;
    fat_entry_t *p = fat_find_path(fat, old), saved;
    if (!p || p->entry.attr & FAT_ATTR_DIR) return -1;
    uint32_t sector = fat->buf_pos, dir = fat->lastdir;
    unsigned offset = ((uint8_t*)p - fat->buf) / 32;
    memcpy(&saved, p, 32);
    p = fat_find_path(fat, name);
    unsigned overwritten = 0;
    if (p) {
        if (fat->buf_pos == sector && (unsigned)((uint8_t*)p - fat->buf) / 32 == offset) return 0;
        if (p->entry.attr & FAT_ATTR_DIR) return -1;
        overwritten = fat_entry_clust(p);
    } else {
        if (!fat->lastname) return -1;
        p = fat_create_name(fat, fat->lastdir, fat->lastname);
    }
    if (!p) return -1;
    memcpy(p->raw + 11, saved.raw + 11, 21);
    fat->flags |= FAT_FLUSH_BUF1;
    fat_flush_buf1(fat);
    if (fat->flags & FAT_IO_ERROR) return -1;
    uint8_t *buf = fat_read_sec(fat, fat->buf, sector);
    if (!buf) return -1;
    p = (fat_entry_t*)buf + offset;
    /* Delete the old directory entry, preserving the chain now owned by the new one. */
    p->entry.clust_hi = p->entry.clust_lo = 0;
    fat->lastdir = dir;
    fat_delete_entry(fat, p);
    if (!(fat->flags & FAT_IO_ERROR)) fat_free_chain(fat, overwritten, 0);
    fat_flush_buf2(fat);
    return fat->flags & FAT_IO_ERROR ? -1 : 0;
}
