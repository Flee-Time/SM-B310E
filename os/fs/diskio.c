/* B310E OS — FatFs disk interface on the SD card (drive 0), plus the
 * timestamp FatFs puts on files.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "ff.h"
#include "diskio.h"
#include "os/drivers.h"
#include "os/rtc.h"
#include "os/lib.h"

/* FAT read-ahead. FatFs walks the FAT one sector at a time (free-space
 * count, cluster chains), and a single-block read costs about 1 ms on
 * the phone (the card's access time ends in a 1 ms sleep), so a 15 GB
 * card's FAT took 15 s to count. Single-sector reads inside the FAT are
 * served from a 32-sector window; writes update it. FatFs calls in here
 * under its volume lock, the only volume being the card. */
#define RA_SECTORS 32

static BYTE ra_buf[RA_SECTORS * 512] __attribute__((aligned(4)));
static LBA_t ra_lo, ra_hi;          /* the FAT region; empty before mount */
static LBA_t ra_first;
static UINT ra_count;

void disk_set_readahead(LBA_t first, LBA_t count)
{
    ra_lo = first;
    ra_hi = first + count;
    ra_count = 0;
}

static DRESULT read_ahead(BYTE *buff, LBA_t sector)
{
    if (!ra_count || sector < ra_first || sector >= ra_first + ra_count) {
        UINT n = ra_hi - sector < RA_SECTORS ? (UINT)(ra_hi - sector) : RA_SECTORS;

        ra_count = 0;
        if (sd_read(sector, ra_buf, n) != 0)
            return RES_ERROR;
        ra_first = sector;
        ra_count = n;
    }
    memcpy(buff, ra_buf + (sector - ra_first) * 512, 512);
    return RES_OK;
}

DSTATUS disk_initialize(BYTE pdrv)
{
    return pdrv == 0 && sd_present() ? 0 : STA_NOINIT;
}

DSTATUS disk_status(BYTE pdrv)
{
    return pdrv == 0 && sd_present() ? 0 : STA_NOINIT;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
    if (pdrv != 0 || !sd_present())
        return RES_NOTRDY;
    if (count == 1 && sector >= ra_lo && sector < ra_hi)
        return read_ahead(buff, sector);
    return sd_read(sector, buff, count) == 0 ? RES_OK : RES_ERROR;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
    if (pdrv != 0 || !sd_present())
        return RES_NOTRDY;
    if (ra_count && sector < ra_first + ra_count && sector + count > ra_first) {
        LBA_t lo = sector > ra_first ? sector : ra_first;
        LBA_t hi = sector + count < ra_first + ra_count ? sector + count
                                                        : ra_first + ra_count;

        memcpy(ra_buf + (lo - ra_first) * 512, buff + (lo - sector) * 512,
               (hi - lo) * 512);
    }
    if (sd_write(sector, buff, count) != 0) {
        ra_count = 0;                   /* the card may hold either */
        return RES_ERROR;
    }
    return RES_OK;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    if (pdrv != 0 || !sd_present())
        return RES_NOTRDY;
    switch (cmd) {
    case CTRL_SYNC:             /* writes are synchronous */
        return RES_OK;
    case GET_SECTOR_COUNT:
        *(LBA_t *)buff = sd_sectors();
        return RES_OK;
    case GET_SECTOR_SIZE:
        *(WORD *)buff = 512;
        return RES_OK;
    case GET_BLOCK_SIZE:
        *(DWORD *)buff = 1;
        return RES_OK;
    }
    return RES_PARERR;
}

/* FAT timestamps from the RTC's calendar (drivers/rtc.c; 2026-01-01 if
 * the clock reads past 2099). */
DWORD get_fattime(void)
{
    return rtc_fattime();
}
