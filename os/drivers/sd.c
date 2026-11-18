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
#include "os/fs.h"
#include "os/kernel.h"
#include "os/lib.h"
#include "drivers/sd_internal.h"

#define BOUNCE        ((uint8_t *)0x40004000u)
#define MAX_BLOCKS    32
#define TRIES         4
#define READY_MS      1000
#define READY_POLLS   20000
#define READY_SPINS   8                 /* CMD13s 25 us apart before 1 ms sleeps: */
#define READY_SPIN_US 25                /* a read is in tran at once, a write soon */

/* The SDIO0 AHB clock (bit 10) is open only inside the lock while a transfer
 * or a probe runs, closed between them (power audit A3, 2026-11-18: before,
 * open from boot). Only this file reaches the controller, always under the
 * lock (sd_init before any other thread). */
#define AHB_EN_SET    0x20500060u
#define AHB_EN_CLR    0x20500070u
#define AHB_SDIO      0x400u
/* dev (poked): 0 leaves the clock open after a transfer (the pre-2026-11-18
 * behaviour), for A/B runs without an upload */
volatile uint32_t sd_gate_dev = 1;

static mutex_t lock;
static bool present;
static int init_code = -1;
static sd_stats_t stats;

/* Idle card (power audit B3, 2026-11-18; a dev switch, poked): after
 * sd_idle_after_ms without a transfer, at the UI's next pass (sd_idle_tick),
 * sd_idle_mode 1 deselects the card (CMD7 0: stand-by), 2 also switches its
 * supply off; the next transfer selects it again, or powers it and probes it
 * again (40 ms ramp slept, then the probe). 0 (the default): neither. */
volatile uint32_t sd_idle_mode;
volatile uint32_t sd_idle_after_ms = 30000;
uint32_t sd_idle_count[4];              /* deselects, powered off, reselects, re-probes */
static int idle_state;                  /* 0 selected, 1 deselected, 2 off */
static uint32_t last_io;
static void wait_ready(void);

/* Controller and card probe. Also the runtime recovery in transfer(), which
 * holds the lock: so it must not touch the mutex. The tick is masked through
 * it, so it is timed on the 1 ms counter, which runs on regardless. */
static int probe(bool recover)
{
    uint32_t s, ldo = 0, t0 = sleep_counter_ms(), ms;
    bool warm;

    s = arch_irq_save();                /* the probe must not see the tick */
    warm = sdio_init(recover, &ldo);
    init_code = sdcard_init();
    arch_irq_restore(s);
    ms = sleep_counter_ms() - t0;
    present = init_code == 0;
    klog("sd: probe %s, code %d, %lu ms (1 ms counter), ACMD41 x%lu, LDO 1184 %04lx 11a4 %04lx",
         recover ? "after failures, power wait 350 ms" :
         warm ? "supply on already, power wait 1 ms" : "supply switched on, power wait 40 ms",
         init_code, (unsigned long)ms, (unsigned long)sdio_acmd41_tries,
         (unsigned long)(ldo & 0xffffu), (unsigned long)(ldo >> 16));
    return init_code;
}

/* The probe with interrupts off (as probe()), its time given back to the
 * kernel: timer 2 reloaded every 1 ms meanwhile, and the next interrupt
 * counts one of them. */
static int probe_timed(bool recover)
{
    uint32_t t0 = sleep_counter_ms(), ms, s;
    int rc = probe(recover);

    s = arch_irq_save();
    ms = sleep_counter_ms() - t0;
    if (ms > 1)
        timer_skip((ms - 1) * (TIMER_HZ / 1000u));
    arch_irq_restore(s);
    return rc;
}

/* Under the lock, the SDIO clock open: the card back from its idle state. */
static int idle_wake(void)
{
    int rc = 0;

    if (idle_state == 1) {
        sd_idle_count[2]++;
        rc = sdio_select(true);
        if (rc)
            rc = probe_timed(false);
    } else if (idle_state == 2) {
        sd_idle_count[3]++;
        sdio_power_on();
        thread_sleep(40);               /* the spec's 35 ms ramp, slept */
        rc = probe_timed(false);        /* finds it on: the 1 ms wait */
    }
    idle_state = 0;
    return rc;
}

/* The UI loop's second (dark: each pass, about a minute apart). */
void sd_idle_tick(void)
{
    uint32_t mode = sd_idle_mode;

    if (!present || !mode || idle_state || kernel_ticks() - last_io < sd_idle_after_ms)
        return;
    if (mutex_lock(&lock, 0) != 0)
        return;                         /* in use */
    if (!idle_state && kernel_ticks() - last_io >= sd_idle_after_ms) {
        cpufreq_hold();
        REG32(AHB_EN_SET) = AHB_SDIO;
        wait_ready();                   /* a single-block write may still program */
        sdio_select(false);
        idle_state = 1;
        sd_idle_count[0]++;
        if (mode >= 2) {
            sdio_power_off();
            idle_state = 2;
            sd_idle_count[1]++;
        }
        if (sd_gate_dev)
            REG32(AHB_EN_CLR) = AHB_SDIO;
        cpufreq_release();
    }
    mutex_unlock(&lock);
}

int sd_init(void)
{
    int rc;

    mutex_init(&lock);                  /* once, at boot */
    rc = probe(false);                  /* sdio_init opens the clock */
    if (sd_gate_dev)
        REG32(AHB_EN_CLR) = AHB_SDIO;
    return rc;
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

    for (unsigned polls = READY_POLLS; polls; polls--) {
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
            if (READY_POLLS - polls < READY_SPINS)
                udelay(READY_SPIN_US);
            else
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
    bool reinit = false;                /* at most one full re-init per transfer */

    if (!present)
        return -1;
    mutex_lock(&lock, WAIT_FOREVER);
    cpufreq_hold();                     /* the SD clock comes from the AHB */
    REG32(AHB_EN_SET) = AHB_SDIO;
    if (idle_state && idle_wake())
        count = 0, rc = -1;             /* the card did not come back */
    while (count) {
        unsigned n = count > MAX_BLOCKS ? MAX_BLOCKS : count;
        int tries = 0;

        while (chunk(lba, buf, n, write)) {
            if (++tries >= TRIES) {
                /* CMD13/CMD12 could not clear it: the controller or card is
                 * wedged (an RF TX burst does this). Re-initialise once, as a
                 * reset does, and retry before giving up — so a transmit no
                 * longer leaves the card unreadable until a reboot. */
                if (!reinit && probe(true) == 0) {
                    reinit = true;      /* recovered; stats.reinits counts it */
                    stats.reinits++;
                    tries = 0;
                    continue;
                }
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
    last_io = kernel_ticks();
    if (sd_gate_dev)
        REG32(AHB_EN_CLR) = AHB_SDIO;
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

/* No transfer runs between these: for code that drops the clock below a
 * transfer's hold (the Sleep test's PLL measure, which wrote garbled bytes
 * into the log file when it preempted the log thread's write, 2026-11-18).
 * Thread context; no card access in between. */
void sd_hold(void)
{
    mutex_lock(&lock, WAIT_FOREVER);
}

void sd_release(void)
{
    mutex_unlock(&lock);
}
