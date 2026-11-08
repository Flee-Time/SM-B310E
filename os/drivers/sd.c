/* B310E OS — SD card block device.
 *
 * Every transfer goes through the 16 KB IRAM bounce buffer at 0x40004000
 * (SDIO DMA into PSRAM hangs the phone), as up to 32 blocks per command.
 * It stays below 0x4000c000: IRAM above that only holds data while
 * 0x8b0001a0 bits 21:19 are set, and the DSP runs with them clear.
 * The keypad scan is paused around each SDIO DMA (a held key during it
 * hangs the phone). A failed chunk waits for the card (CMD13, stopping an
 * aborted transfer) and is retried up to four times; rewriting the same
 * blocks is harmless. One mutex covers the controller and the buffer.
 * Ported from the Rockbox port's sd-sc6530c.c, verified on the phone.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/arch.h"
#include "os/drivers.h"
#include "os/kernel.h"
#include "os/lib.h"
#include "drivers/sd_internal.h"

#define BOUNCE        ((uint8_t *)0x40004000u)
#define MAX_BLOCKS    32
#define TRIES         4
#define READY_MS      1000

static mutex_t lock;
static bool present;
static int init_code = -1;
static sd_stats_t stats;

int sd_init(void)
{
    uint32_t s;

    mutex_init(&lock);
    s = arch_irq_save();                /* the probe must not see the tick */
    sdio_init();
    init_code = sdcard_init();
    arch_irq_restore(s);
    present = init_code == 0;
    return init_code;
}

bool sd_present(void)
{
    return present;
}

uint32_t sd_sectors(void)
{
    return present ? sdio_num_blocks : 0;
}

void sd_get_stats(sd_stats_t *out)
{
    *out = stats;
}

/* Wait until the card is back in transfer state and ready (CMD13). */
static void wait_ready(void)
{
    bool sched = kernel_running() && !arch_in_irq();
    uint32_t end = kernel_ticks() + READY_MS;

    for (unsigned polls = 20000; polls; polls--) {
        uint32_t st;

        if (sdio_card_status(&st) == 0) {
            unsigned state = (st >> 9) & 15;

            if (state == 4 && (st & (1u << 8)))     /* tran, ready */
                return;
            if (state == 5 || state == 6)           /* data, receive */
                sdio_stop();
        }
        if (sched) {
            if ((int32_t)(kernel_ticks() - end) >= 0)
                return;
            thread_sleep(1);
        }
    }
}

static int chunk(uint32_t lba, uint8_t *buf, unsigned n, bool write)
{
    int rc;

    keypad_pause();
    if (write) {
        memcpy(BOUNCE, buf, n * 512);
        rc = sdio_transfer(lba, BOUNCE, n, true);
    } else {
        rc = sdio_transfer(lba, BOUNCE, n, false);
        if (rc == 0)
            memcpy(buf, BOUNCE, n * 512);
    }
    keypad_resume();
    if (n > 1)
        wait_ready();
    return rc;
}

static int transfer(uint32_t lba, uint8_t *buf, unsigned count, bool write)
{
    int rc = 0;

    if (!present)
        return -1;
    mutex_lock(&lock, WAIT_FOREVER);
    cpufreq_hold();                     /* the SD clock comes from the AHB */
    while (count) {
        unsigned n = count > MAX_BLOCKS ? MAX_BLOCKS : count;
        int tries = 0;

        while (chunk(lba, buf, n, write)) {
            if (++tries >= TRIES) {
                stats.failures++;
                rc = -1;
                break;
            }
            stats.retries++;
            wait_ready();
        }
        if (rc)
            break;
        if (write)
            stats.blocks_written += n;
        else
            stats.blocks_read += n;
        lba += n;
        buf += n * 512;
        count -= n;
    }
    cpufreq_release();
    mutex_unlock(&lock);
    return rc;
}

int sd_read(uint32_t lba, void *buf, unsigned count)
{
    return transfer(lba, buf, count, false);
}

int sd_write(uint32_t lba, const void *buf, unsigned count)
{
    return transfer(lba, (uint8_t *)buf, count, true);
}
