/* B310E OS — SC6530 SDIO0 controller and SD card protocol.
 *
 * Ported from the Rockbox port's sdio-sc6530c.c (GPLv2-or-later, verified
 * on the phone), itself a port of fpdoom's sdio.c (Unlicense). The
 * controller at 0x20700000 has a polled command engine and a DMA data
 * path; no interrupts are used. Rules carried over from the hardware:
 *  - the SDIO pinmux (0x8c000254..0x268, read-modify-write) is the only
 *    0x8c write allowed; 0x8c0002a4 hangs the phone;
 *  - every wait is bounded by iterations, never by the free-running
 *    counter;
 *  - DMA targets must be IRAM (sd.c bounces through 0x4000b000);
 *  - the probe runs with interrupts masked (sd.c).
 * Long data waits (the card programming a block, up to 250-500 ms by the
 * SD spec) sleep 1 ms at a time once the scheduler runs.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/arch.h"
#include "os/drivers.h"
#include "os/kernel.h"
#include "drivers/sd_internal.h"

#define DELAY(n) do { volatile uint32_t _d = (n); while (_d--) ; } while (0)

typedef volatile struct {
    uint32_t dma_addr, blk_size, arg, tr_mode;
    uint32_t resp[4];
    uint32_t buf_port, state, ctrl1, ctrl2;
    uint32_t int_st, int_en, int_sig;
} sdio_t;

#define SDIO ((sdio_t *)0x20700000u)

enum {
    CMD_DATA = 1 << 21,
    CMD_IND_CHK = 1 << 20,
    CMD_CRC_CHK = 1 << 19,
    CMD_RESP_0 = 0 << 16,
    CMD_RESP_136 = 1 << 16,
    CMD_RESP_48 = 2 << 16,
    CMD_RESP_48_BUSY = 3 << 16,
    CMD_DATA_READ = 1 << 4,
    CMD_DMA = 1 << 0,
    TR_BLKCNT = 1 << 1,
    TR_MULTI = 1 << 5,

    INT_TARGET_RESP = 1 << 28,
    INT_DATA_END = 1 << 22,
    INT_DATA_CRC = 1 << 21,
    INT_DATA_TIMEOUT = 1 << 20,
    INT_CMD_INDEX = 1 << 19,
    INT_CMD_END = 1 << 18,
    INT_CMD_CRC = 1 << 17,
    INT_CMD_TIMEOUT = 1 << 16,
    INT_ERR = 1 << 15,
    INT_DMA = 1 << 3,
    INT_TR_COMPLETE = 1 << 1,
    INT_CMD_COMPLETE = 1 << 0,
};

#define MAKE_CMD(cmd, resp) ((uint32_t)(cmd) << 24 | CMD_RESP_##resp)
#define CHK (CMD_IND_CHK | CMD_CRC_CHK)

#define CMD0   MAKE_CMD(0, 0)
#define CMD2   (MAKE_CMD(2, 136) | CMD_CRC_CHK)
#define CMD3   (MAKE_CMD(3, 48) | CHK)
#define CMD7   (MAKE_CMD(7, 48_BUSY) | CHK)
#define CMD8   (MAKE_CMD(8, 48) | CHK)
#define CMD9   (MAKE_CMD(9, 136) | CMD_CRC_CHK)
#define CMD12  (MAKE_CMD(12, 48_BUSY) | CHK)
#define CMD13  (MAKE_CMD(13, 48) | CHK)
#define CMD16  (MAKE_CMD(16, 48) | CHK)
#define CMD17  (MAKE_CMD(17, 48) | CHK | CMD_DMA | CMD_DATA_READ | CMD_DATA)
#define CMD18  ((CMD17 & ~(0x3fu << 24)) | (18u << 24) | TR_BLKCNT | TR_MULTI)
#define CMD24  (MAKE_CMD(24, 48) | CHK | CMD_DMA | CMD_DATA)
#define CMD25  ((CMD24 & ~(0x3fu << 24)) | (25u << 24) | TR_BLKCNT | TR_MULTI)
#define CMD55  (MAKE_CMD(55, 48) | CHK)
#define ACMD6  (MAKE_CMD(6, 48) | CHK)
#define ACMD41 MAKE_CMD(41, 48)

#define INT_COMMON (INT_CMD_COMPLETE | INT_TARGET_RESP | INT_CMD_INDEX | \
                    INT_CMD_END | INT_CMD_CRC | INT_CMD_TIMEOUT)
#define INT_R2     (INT_CMD_COMPLETE | INT_TARGET_RESP | INT_CMD_END | \
                    INT_CMD_CRC | INT_CMD_TIMEOUT)
#define INT_READ   (INT_R2 | INT_DATA_END | INT_DATA_CRC | INT_DATA_TIMEOUT | \
                    INT_TR_COMPLETE | INT_DMA)
#define INT_WRITE  (INT_READ & ~INT_DATA_END)
#define INT_ACMD41 (INT_CMD_COMPLETE | INT_TARGET_RESP | INT_CMD_END | \
                    INT_CMD_TIMEOUT)

#define WAIT_BUDGET       1000000u
#define DATA_SPIN_BUDGET  2000u      /* ~40 us: the data clocks out */
#define DATA_WAIT_BUDGET  20000000u  /* without a scheduler */
#define DATA_WAIT_MS      1500

unsigned sdio_shl;                   /* 0 block-addressed, 9 byte-addressed */
uint32_t sdio_num_blocks;
static uint32_t rca;

/* ---- controller --------------------------------------------------------- */

static void sw_reset(void)
{
    uint32_t n = WAIT_BUDGET;

    SDIO->ctrl2 |= 1 << 24;
    while ((SDIO->ctrl2 & (1 << 24)) && --n)
        ;
}

static void clk_wait(uint32_t bit)
{
    uint32_t n = WAIT_BUDGET;

    SDIO->ctrl2 |= bit;
    while (!(SDIO->ctrl2 & 2) && --n)
        ;
}

static void ahb_reset(void)
{
    REG32(0x20500020) = 0x100;
    DELAY(1000);
    REG32(0x20500030) = 0x100;
    DELAY(1000);
}

static void busy_ms(uint32_t ms)    /* interrupts may be off: no timer */
{
    DELAY(ms * 40000u);
}

void sdio_init(void)
{
    uint32_t addr = 0x8c000250u, tmp;

    REG32(0x20500060) = 0x400;          /* SDIO0 clock */
    ahb_reset();
    REG32(0x205000b0) &= ~(3u << 2);    /* slot 0 */
    REG32(0x8b000040) = (REG32(0x8b000040) & ~(3u << 28)) | 1u << 28;

    sw_reset();
    clk_wait(1);
    sw_reset();
    ahb_reset();
    SDIO->ctrl1 &= ~0x26u;              /* 1-bit, no high speed */

    for (int i = 0; i < 6; i++) {       /* pinmux: SDIO function */
        addr += 4;
        REG32(addr) = (REG32(addr) & ~0xc0u) | 0x80u;
    }
    DELAY(100);

    adi_write(0x82001184, adi_read(0x82001184) & ~1u);   /* SD LDO on */
    adi_write(0x820011a4, adi_read(0x820011a4) | 1u);

    SDIO->ctrl2 = (SDIO->ctrl2 & ~(0xffu << 8)) | 0x40u << 8;   /* 750 kHz */
    clk_wait(1);
    clk_wait(4);
    busy_ms(350);                       /* card power-up */

    tmp = SDIO->int_en;                 /* data timeout count 14 */
    SDIO->int_en = tmp & ~(1u << 20);
    SDIO->ctrl2 = (SDIO->ctrl2 & ~(15u << 16)) | 14u << 16;
    SDIO->int_en = tmp;
}

/* One command; returns int_st. resp is filled when no error occurred. */
static uint32_t command(uint32_t tr, uint32_t arg, uint32_t ints, void *data,
                        unsigned count, uint32_t *resp)
{
    sdio_t *s = SDIO;
    uint32_t st, done, n;

    clk_wait(4);
    s->int_en &= ~0x11ff01ffu;
    s->int_st = 0x11ff01ffu;
    s->int_en |= ints;
    if (data) {
        dcache_clean_range(data, 0);    /* drain the write buffer */
        s->dma_addr = (uint32_t)data;
        s->blk_size = count << 16 | 7u << 12 | 512u;
    }
    s->arg = arg;
    s->tr_mode = (s->tr_mode & 0xc004ff80u) | tr;

    done = (data ? INT_TR_COMPLETE : INT_CMD_COMPLETE) | INT_ERR;
    n = data ? DATA_SPIN_BUDGET : WAIT_BUDGET;
    do {
        st = s->int_st;
    } while (!(st & done) && --n);

    if (data && !(st & done)) {
        bool sched = kernel_running() && !arch_in_irq();
        uint32_t end = kernel_ticks() + DATA_WAIT_MS;

        if (sched && count == 1)
            keypad_resume();            /* only the card's programming left */
        n = DATA_WAIT_BUDGET;
        do {
            if (sched) {
                if ((int32_t)(kernel_ticks() - end) >= 0)
                    break;
                thread_sleep(1);
            }
            st = s->int_st;
        } while (!(st & done) && --n);
        if (sched && count == 1)
            keypad_pause();
    }

    s->ctrl2 |= 3u << 25;               /* reset the data and command lines */
    n = WAIT_BUDGET;
    while ((s->ctrl2 & (3u << 25)) && --n)
        ;
    if (!(st & INT_ERR) && resp) {
        resp[0] = s->resp[0];
        resp[1] = s->resp[1];
        resp[2] = s->resp[2];
        resp[3] = s->resp[3];
    }
    s->ctrl2 &= ~4u;                    /* card clock off */
    return st;
}

/* R2 is stored least significant word first without the CRC byte. */
static void parse_csd(const uint32_t *r)
{
    uint32_t c0 = r[3] << 8 | r[2] >> 24;
    uint32_t c1 = r[2] << 8 | r[1] >> 24;
    uint32_t c2 = r[1] << 8 | r[0] >> 24;

    switch (c0 >> 30) {
    case 0: {                           /* SDSC */
        unsigned rbl = (c1 >> 16) & 0xf, mult = (c2 >> 15) & 7;
        uint32_t size = (c1 & 0x3ff) << 2 | (c2 >> 30 & 3);
        uint32_t blocks = (size + 1) << (mult + 2);
        sdio_num_blocks = rbl >= 9 ? blocks << (rbl - 9) : blocks >> (9 - rbl);
        break;
    }
    case 1:                             /* SDHC/SDXC */
        sdio_num_blocks = (((c1 & 0x3f) << 16 | c2 >> 16) + 1) << 10;
        break;
    default:
        sdio_num_blocks = 0;
    }
}

/* 0 ok; 1 bad CMD8 echo; 2 no card; 3 CID; 4 RCA; 5 CSD; 6 select;
 * 7 4-bit bus; 8 block length. */
int sdcard_init(void)
{
    uint32_t r[4], st, ver = 0;
    int tries;

    command(CMD0, 0, INT_CMD_COMPLETE, NULL, 0, NULL);
    for (tries = 0; tries < 30; tries++) {
        st = command(CMD8, 0x1aa, INT_COMMON, NULL, 0, r);
        if (st & INT_CMD_TIMEOUT) {
            busy_ms(20);
        } else if ((st & INT_CMD_COMPLETE) && r[0] == 0x1aa) {
            ver = 1;
            break;
        } else {
            return 1;
        }
    }
    for (tries = 0; tries < 150; tries++) {
        st = command(CMD55, 0, INT_COMMON, NULL, 0, NULL);
        if (st & INT_CMD_COMPLETE)
            st = command(ACMD41, 0xff8000u | ver << 30, INT_ACMD41, NULL, 0, r);
        if (!(st & INT_CMD_TIMEOUT)) {
            if (!(st & INT_CMD_COMPLETE))
                break;
            if ((int32_t)r[0] < 0)
                goto ready;
        }
        busy_ms(20);
    }
    return 2;

ready:
    sdio_shl = (r[0] & (1u << 30)) ? 0 : 9;     /* OCR CCS */
    if (!(command(CMD2, 0, INT_R2, NULL, 0, r) & INT_CMD_COMPLETE))
        return 3;
    if (!(command(CMD3, 0, INT_COMMON, NULL, 0, r) & INT_CMD_COMPLETE))
        return 4;
    rca = r[0] >> 16 << 16;
    if (!(command(CMD9, rca, INT_R2, NULL, 0, r) & INT_CMD_COMPLETE))
        return 5;
    parse_csd(r);
    SDIO->ctrl2 = (SDIO->ctrl2 & ~(0xffu << 8)) | 1u << 8;     /* 24 MHz */
    if (!(command(CMD7, rca, INT_COMMON, NULL, 0, r) & INT_CMD_COMPLETE))
        return 6;
    st = command(CMD55, rca, INT_COMMON, NULL, 0, NULL);
    if (st & INT_CMD_COMPLETE)
        st = command(ACMD6, 2, INT_COMMON, NULL, 0, r);
    if (!(st & INT_CMD_COMPLETE))
        return 7;
    SDIO->ctrl1 = (SDIO->ctrl1 & ~0x22u) | 2;   /* 4-bit bus */
    if (!(command(CMD16, 512, INT_COMMON, NULL, 0, r) & INT_CMD_COMPLETE))
        return 8;
    return 0;
}

int sdio_card_status(uint32_t *status)
{
    uint32_t r[4], st = command(CMD13, rca, INT_COMMON, NULL, 0, r);

    if ((st & INT_ERR) || !(st & INT_CMD_COMPLETE))
        return -1;
    *status = r[0];
    return 0;
}

void sdio_stop(void)
{
    uint32_t r[4];

    command(CMD12, 0, INT_COMMON, NULL, 0, r);
}

/* buf: IRAM, 512-byte aligned. One block: CMD17/24; more: CMD18/25 and
 * CMD12. */
int sdio_transfer(uint32_t lba, uint8_t *buf, unsigned count, bool write)
{
    uint32_t st;

    lba <<= sdio_shl;
    if (count == 1) {
        st = command(write ? CMD24 : CMD17, lba, write ? INT_WRITE : INT_READ,
                     buf, 1, NULL);
    } else {
        st = command(write ? CMD25 : CMD18, lba, write ? INT_WRITE : INT_READ,
                     buf, count, NULL);
        sdio_stop();
    }
    return !(st & INT_ERR) && (st & INT_TR_COMPLETE) ? 0 : -1;
}
