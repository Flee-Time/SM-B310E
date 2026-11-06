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
 *  - SDIO DMA into PSRAM HANGS this phone — EVERY transfer bounces
 *    through the IRAM buffer at 0x4000b000 (the DMA-safe window the boot
 *    menu stages its 512-byte blocks through). It is 16 KB now: requests
 *    go out as multi-block commands of up to 32 blocks (CMD18/CMD25 +
 *    CMD12). Nothing else in Rockbox uses IRAM; the boot stub that ran
 *    from 0x40004000 is gone once Rockbox starts.
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
#define SD_BOUNCE_BUF      0x4000b000u   /* IRAM, 512-byte aligned */
#define SD_MULTI_BLOCKS    32            /* bounce buffer: 16 KB */

/* ---- sdio-sc6530c.c (ported fpdoom SC6530 SDIO driver) ------------------ */
extern void sdio_init(void);
extern int  sdcard_init(void);
extern int  sdio_read_block(uint32_t idx, uint8_t *buf);
extern int  sdio_write_block(uint32_t idx, uint8_t *buf);
extern int  sdio_read_blocks(uint32_t idx, uint8_t *buf, unsigned count);
extern int  sdio_write_blocks(uint32_t idx, uint8_t *buf, unsigned count);
extern int  sdio_card_status(uint32_t *status);
extern void sdio_stop_transmission(void);
extern unsigned sdio_shl;
extern unsigned long sdio_num_blocks;
extern void sc6530_keypad_capture(void);
/* usb-drv-sc6530c.c: USB write DMA running / start one held back for us */
extern volatile bool sc6530_usb_out_dma;
extern void sc6530_usb_sd_idle(void);

static bool sd_enabled_ = false;   /* card present after sd_init */
/* a transfer is running: sampled by the USB driver's throughput log */
volatile bool sd_busy_now;
static long  sd_activity;
static struct mutex sd_mutex;
/* Block transfers that failed and were retried / that failed for good
 * (shown in the I/O dump). */
static unsigned sd_retries, sd_failures;

void sc6530_sd_errors(unsigned *retries, unsigned *failures)
{
    *retries = sd_retries;
    *failures = sd_failures;
}

/* Write throughput since boot: blocks written, ticks spent in write
 * requests (sums of tick deltas, so short requests average out), and the
 * slowest single request. */
static unsigned long sd_wr_blocks, sd_wr_ticks, sd_wr_max_ticks;

void sc6530_sd_write_stats(unsigned long *blocks, unsigned long *ticks,
                           unsigned long *max_ticks)
{
    *blocks = sd_wr_blocks;
    *ticks = sd_wr_ticks;
    *max_ticks = sd_wr_max_ticks;
}

int sd_init(void)
{
    int oldlevel;

    mutex_init(&sd_mutex);

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

/* sdio_cmd: the card is programming a block and the driver is about to
 * yield (begin) or has finished waiting (end). Let the keypad scan run
 * meanwhile; pause it again before the next SDIO DMA. */
void sd_card_busy_wait(bool begin)
{
    if (begin) {
        sc6530_keypad_capture();
        KEYPAD_CTRL |= 1u;
    } else {
        KEYPAD_CTRL &= ~1u;
    }
}

/* After a failed block: wait until the card is back in the transfer state
 * and ready for data (CMD13), stopping an aborted transfer on the way.
 * A card still programming answers with the programming state. */
#define SD_READY_TIMEOUT  HZ
#define SD_BLOCK_TRIES    4

static void wait_card_ready(void)
{
    long end = current_tick + SD_READY_TIMEOUT;
    unsigned polls = 20000;     /* bound even with the tick stopped */

    while (polls--) {
        uint32_t st;

        if (sdio_card_status(&st) == 0) {
            unsigned state = (st >> 9) & 15;

            if (state == 4 && (st & (1u << 8)))     /* tran, ready */
                return;
            if (state == 5 || state == 6)           /* data, rcv */
                sdio_stop_transmission();
        }
        if (TIME_AFTER(current_tick, end))
            return;
        yield();
    }
}

/* Up to SD_MULTI_BLOCKS blocks through the IRAM bounce buffer, as one
 * command (CMD17/24 for one block, CMD18/25 + CMD12 for more). The keypad
 * matrix scan is paused around the SDIO DMA (held key + SDIO DMA contends
 * and hangs) — the keypad_pause()/keypad_resume() discipline of the boot
 * menu. After a multi-block command the card may still be programming:
 * wait for it (CMD13, yielding) with the keypad running again. */
static int transfer_chunk(uint32_t idx, void *buf, unsigned n, bool write)
{
    unsigned char *bounce = (unsigned char *)SD_BOUNCE_BUF;
    int rc;

    KEYPAD_CTRL &= ~1u;                     /* keypad_pause() */

    if (write) {
        memcpy(bounce, buf, n * SD_BLOCK_SIZE);
        rc = sdio_write_blocks(idx, bounce, n);
    } else {
        rc = sdio_read_blocks(idx, bounce, n);
        if (rc == 0)
            memcpy(buf, bounce, n * SD_BLOCK_SIZE);
    }

    sc6530_keypad_capture();                /* capture + ACK before resume */
    KEYPAD_CTRL |= 1u;
    if (n > 1)
        wait_card_ready();
    return rc;
}

/* File metadata and audio buffering run in different threads. Protect the
 * controller AND its shared IRAM bounce buffer for the entire request. */
static int transfer_sectors(sector_t start, int count, void *buf, bool write)
{
    uint32_t idx = (uint32_t)start;         /* sdio_read_block shifts */
    uint8_t *p = (uint8_t *)buf;
    int rc = 0;

    if (!sd_enabled_)
        return -1;

    mutex_lock(&sd_mutex);
    /* Wait while a USB write DMA runs (it and the SDIO DMA together froze
     * the phone); USB then holds a new one back until this ends. */
    {
        long end = current_tick + 2 * HZ;

        for (;;) {
            int old = disable_irq_save();
            if (!sc6530_usb_out_dma || TIME_AFTER(current_tick, end)) {
                sd_busy_now = true;
                restore_irq(old);
                break;
            }
            restore_irq(old);
            yield();
        }
    }
    long t0 = current_tick;
    int blocks = count;
    while (count > 0) {
        unsigned n = count > SD_MULTI_BLOCKS ? SD_MULTI_BLOCKS : count;
        int tries = 0;

        /* A failed chunk used to fail the whole request, and a failed
         * write panics the FAT writeback. Recover and retry instead;
         * rewriting the same blocks is harmless. */
        while (transfer_chunk(idx, p, n, write)) {
            if (++tries >= SD_BLOCK_TRIES) {
                sd_failures++;
                rc = -1;
                break;
            }
            sd_retries++;
            wait_card_ready();
        }
        if (rc)
            break;
        idx += n;
        p += n * SD_BLOCK_SIZE;
        count -= n;
    }
    sd_activity = current_tick;
    if (write && rc == 0) {
        unsigned long dt = (unsigned long)(current_tick - t0);

        sd_wr_blocks += blocks;
        sd_wr_ticks += dt;
        if (dt > sd_wr_max_ticks)
            sd_wr_max_ticks = dt;
    }
    sd_busy_now = false;
    mutex_unlock(&sd_mutex);
    sc6530_usb_sd_idle();
    return rc;
}

int sd_read_sectors(IF_MD(int drive,) sector_t start, int count, void *buf)
{
#ifdef HAVE_MULTIDRIVE
    (void)drive;
#endif
    return transfer_sectors(start, count, buf, false);
}

int sd_write_sectors(IF_MD(int drive,) sector_t start, int count,
                     const void *buf)
{
#ifdef HAVE_MULTIDRIVE
    (void)drive;
#endif
    return transfer_sectors(start, count, (void *)buf, true);
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
