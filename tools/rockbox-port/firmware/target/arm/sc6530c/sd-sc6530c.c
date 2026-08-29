/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2009 by Bob Cousins
 * Copyright (C) 2026 by B310E-OS project
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/
#include "config.h" /* for HAVE_MULTIDRIVE */
#include "sd.h"
#include "sdmmc.h"   /* tCardInfo (card_get_info_target) */
#include "storage.h"
#include "system.h"
#include "kernel.h"
#include "thread.h"
#include <string.h>
#include "gcc_extensions.h"

/*
 * B310E-OS Rockbox port — b310e/target/arm/sc6530c/sd-sc6530c.c
 * (GPLv2, Rockbox-derived; modeled on Rockbox's s3c2440/sd-s3c2440.c and
 * the B310E-OS boot menu's SD path — arch/diag_menu_main.c).
 *
 * Rockbox's storage layer over the REAL SC6530 SDIO driver (the ported
 * fpdoom driver in sdio-sc6530c.c). Hardware lessons baked in:
 *  - SDIO DMA into PSRAM HANGS this phone — EVERY block transfer bounces
 *    through the 512-byte IRAM buffer at 0x4000b000 (the proven DMA-safe
 *    window the boot menu stages through).
 *  - The SD probe must run with the 1 ms tick IRQ MASKED (a live tick
 *    during sdio_init kills the phone on hardware) — sd_init() wraps the
 *    whole probe in disable_irq_save()/restore_irq().
 *  - A HELD key + SDIO DMA contends and HANGS — the keypad matrix scan is
 *    paused (controller enable bit off, exactly the B310E-OS
 *    keypad_pause()/keypad_resume() discipline) around each transfer.
 *  - No card-detect IRQ exists: absence is detected by command timeouts.
 *    sd_present() reports the last init result (present-after-init); the
 *    storage thread's hotswap poll reads it — it never probes hardware,
 *    so it cannot hang. A re-init (card swap) is a future enhancement.
 *
 * The SDIO pinmux (0x8c000250-264) is the fpdoom RMW form INSIDE
 * sdio-sc6530c.c; nothing here writes 0x8c.
 */

#define KEYPAD_BASE_ADDR   0x87000000u
#define KEYPAD_CTRL        (*(volatile uint32_t *)(KEYPAD_BASE_ADDR + 0x00))
#define KEYPAD_INT_CLR     (*(volatile uint32_t *)(KEYPAD_BASE_ADDR + 0x10))
#define SD_BOUNCE_BUF      0x4000b000u   /* IRAM, 512-byte aligned */

/* ---- sdio-sc6530c.c (ported fpdoom SC6530 SDIO driver) ------------------ */
extern void sdio_init(void);
extern int  sdcard_init(void);
extern int  sdio_read_block(uint32_t idx, uint8_t *buf);
extern int  sdio_write_block(uint32_t idx, uint8_t *buf);
extern unsigned sdio_shl;
extern unsigned long sdio_num_blocks;

static bool sd_enabled_ = false;   /* card present after sd_init */
static long  sd_activity;

int sd_init(void)
{
    int oldlevel;

    /* The whole probe runs with the tick IRQ masked (B310E-OS hardware
     * lesson: a live 1 ms tick during sdio_init kills the phone). */
    oldlevel = disable_irq_save();
    sdio_init();
    sd_enabled_ = (sdcard_init() == 0);
    restore_irq(oldlevel);

    return 0;   /* storage_init proceeds; card absence is a soft state */
}

bool sd_removable(void)
{
    return true;
}

bool sd_present(void)
{
    /* No card-detect IRQ: report the last init result (see the file
     * header — the hotswap poll must never touch hardware). */
    return sd_enabled_;
}

/* One block read/write through the IRAM bounce buffer. The keypad matrix
 * scan is paused first (held key + SDIO DMA contends and hangs) — the
 * exact keypad_pause()/keypad_resume() discipline of the boot menu. */
static int transfer_block(uint32_t idx, void *buf, bool write)
{
    unsigned char *bounce = (unsigned char *)SD_BOUNCE_BUF;
    int rc;

    KEYPAD_CTRL &= ~1u;                     /* keypad_pause() */

    if (write) {
        memcpy(bounce, buf, SD_BLOCK_SIZE);
        rc = sdio_write_block(idx, bounce);
    } else {
        rc = sdio_read_block(idx, bounce);
        if (rc == 0)
            memcpy(buf, bounce, SD_BLOCK_SIZE);
    }

    KEYPAD_INT_CLR = 0xfffu;                /* keypad_resume() */
    KEYPAD_CTRL |= 1u;
    return rc;
}

int sd_read_sectors(IF_MD(int drive,) sector_t start, int count, void *buf)
{
    uint32_t idx = (uint32_t)start;         /* sdio_read_block shifts */
    uint8_t *p = (uint8_t *)buf;

#ifdef HAVE_MULTIDRIVE
    (void)drive;
#endif

    if (!sd_enabled_)
        return 0;

    while (count--) {
        if (transfer_block(idx++, p, false))
            return -1;
        p += SD_BLOCK_SIZE;
    }
    sd_activity = current_tick;
    return 0;
}

int sd_write_sectors(IF_MD(int drive,) sector_t start, int count,
                     const void *buf)
{
    uint32_t idx = (uint32_t)start;
    const uint8_t *p = (const uint8_t *)buf;

#ifdef HAVE_MULTIDRIVE
    (void)drive;
#endif

    if (!sd_enabled_)
        return 0;

    while (count--) {
        if (transfer_block(idx++, (void *)p, true))
            return -1;
        p += SD_BLOCK_SIZE;
    }
    sd_activity = current_tick;
    return 0;
}

#ifdef STORAGE_GET_INFO
/* sd_get_info is provided by the generic firmware/drivers/sd.c, which
 * calls this hook for the card geometry — filled from the ported driver's
 * CSD parse (sdio_num_blocks) and class shift (sdio_shl). */
static tCardInfo sd_card_info =
{
    .initialized = 1,
    .blocksize   = 512,
    .numblocks   = 0,
};

tCardInfo *card_get_info_target(int drive)
{
    (void)drive;
    sd_card_info.numblocks = sd_enabled_ ? sdio_num_blocks : 0;
    return &sd_card_info;
}
#endif /* STORAGE_GET_INFO */

void sd_enable(bool on)
{
    (void)on;
}

void sd_sleepnow(void)
{
}

bool sd_disk_is_active(void)
{
    return false;
}

int sd_soft_reset(void)
{
    return 0;
}

void sd_close(void)
{
}

int sd_spinup_time(void)
{
    return 0;
}

long sd_last_disk_activity(void)
{
    return sd_activity;
}

#ifdef CONFIG_STORAGE_MULTI
int sd_num_drives(int first_drive)
{
    (void)first_drive;
    return sd_enabled_ ? 1 : 0;
}
#endif /* CONFIG_STORAGE_MULTI */

int sd_event(long id, intptr_t data)
{
    (void)id;
    (void)data;
    return 0;
}
