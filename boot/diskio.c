/* B310E boot loader — FatFs disk I/O on the SD card.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "boot.h"
#include "ff.h"
#include "diskio.h"

static DSTATUS state = STA_NOINIT;

DSTATUS disk_status(BYTE pdrv)
{
    return pdrv ? STA_NOINIT : state;
}

DSTATUS disk_initialize(BYTE pdrv)
{
    if (pdrv)
        return STA_NOINIT;
    state = 0;                          /* card_open() ran before f_mount() */
    return state;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
    if (pdrv || state)
        return RES_NOTRDY;
    return card_read((uint32_t)sector, buff, count) ? RES_ERROR : RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
    if (pdrv || state)
        return RES_NOTRDY;
    return card_write((uint32_t)sector, buff, count) ? RES_ERROR : RES_OK;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    (void)buff;
    if (pdrv || state)
        return RES_NOTRDY;
    return cmd == CTRL_SYNC ? RES_OK : RES_PARERR;
}
