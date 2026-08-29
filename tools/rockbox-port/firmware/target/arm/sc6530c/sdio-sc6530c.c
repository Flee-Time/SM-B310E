/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
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
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "logf.h"

/*
 * B310E-OS Rockbox port — b310e/target/arm/sc6530c/sdio-sc6530c.c
 * (GPLv2; a port of the B310E-OS drivers/sdio.c — fpdoom sdio.c, SC6530
 * branch, Unlicense — adapted to the Rockbox target environment:
 * MEM4 -> REG32, DELAY -> local busy loop, kprintf -> logf,
 * irq_disable/enable -> Rockbox's, clean_invalidate_dcache ->
 * commit_discard_dcache).
 *
 * The SC6530 SDIO0 controller @ 0x20700000, polled command engine + DMA
 * data path, no interrupts. HW-VALIDATED on the B310E via fpdoom fptest
 * and the B310E-OS boot menu (sdcard_init stage codes printed by
 * diag-sd). SAFETY rules carried over:
 *  - the SDIO pinmux writes (0x8c000250-264, fpdoom RMW) are the ONLY
 *    0x8c writes in the tree — they are a DIFFERENT register set from the
 *    forbidden 0x8c0002a4 (UART-TX pinmux hangs the phone); do not add
 *    any other 0x8c write.
 *  - ALL waits are bounded (iteration caps only — the free-running
 *    0x8100300c counter is NEVER used here: its stability loop can hang
 *    mid-SDIO, the stage-5 hardware hang).
 *  - the DMA target MUST be an IRAM buffer (SDIO DMA into PSRAM hangs
 *    this phone) — sd-sc6530c.c bounces through 0x4000b000.
 *  - the 1 ms tick IRQ must be masked around the probe (a live tick
 *    during sdio_init kills the phone) — sd-sc6530c.c wraps sd_init.
 */

#define REG32(a) (*(volatile uint32_t *)(a))

/* DELAY: the B310E-OS chip.h macro is a small busy loop; a volatile
 * iteration spin terminates always (the SD path must not touch the
 * free-running counter). */
#define DELAY(n) do { volatile uint32_t _d = (n); while (_d--) ; } while (0)

#define SDIO0_BASE ((sdio_base_t *)0x20700000u)

typedef volatile struct {
    uint32_t dma_addr, blk_size, arg, tr_mode;
    uint32_t resp[4];
    uint32_t buf_port, state, ctrl1, ctrl2;
    uint32_t int_st, int_en, int_sig;
} sdio_base_t;

enum {
    SDIO_CMD_DATA = 1 << 21,
    SDIO_CMD_IND_CHK = 1 << 20,
    SDIO_CMD_CRC_CHK = 1 << 19,

    SDIO_CMD_RESP_0 = 0 << 16,
    SDIO_CMD_RESP_136 = 1 << 16,
    SDIO_CMD_RESP_48 = 2 << 16,
    SDIO_CMD_RESP_48_BUSY = 3 << 16,

    SDIO_CMD_DATA_READ = 1 << 4,
    SDIO_CMD_DMA = 1 << 0,

    SDIO_INT_TARGET_RESP = 1 << 28,
    SDIO_INT_CMD_INDEX = 1 << 19,
    SDIO_INT_CMD_END = 1 << 18,
    SDIO_INT_CMD_CRC = 1 << 17,
    SDIO_INT_CMD_TIMEOUT = 1 << 16,
    SDIO_INT_ERR = 1 << 15,
    SDIO_INT_DATA_END = 1 << 22,
    SDIO_INT_DATA_CRC = 1 << 21,
    SDIO_INT_DATA_TIMEOUT = 1 << 20,
    SDIO_INT_TR_COMPLETE = 1 << 1,
    SDIO_INT_CMD_COMPLETE = 1 << 0,
    SDIO_INT_DMA = 1 << 3,
};

unsigned sdio_shl;          /* 0 SDHC / 9 SDSC (block vs byte addressing)  */
unsigned long sdio_num_blocks;  /* card size in 512-byte blocks (CSD)     */

#define SDIO_WAIT_BUDGET 1000000u

/* ---- clock/control helpers (fpdoom sdio.c:82-170) ---------------------- */

static void sdio_sw_reset(sdio_base_t *sdio)
{
    uint32_t n = SDIO_WAIT_BUDGET;

    sdio->ctrl2 |= 1 << 24;
    while (sdio->ctrl2 & (1 << 24))
        if (--n == 0) break;
}

static void sdio_intclk_on(sdio_base_t *sdio)
{
    uint32_t n = SDIO_WAIT_BUDGET;

    sdio->ctrl2 |= 1;
    while (!(sdio->ctrl2 & 2))
        if (--n == 0) break;
}

static void sdio_sdclk_on(sdio_base_t *sdio)
{
    uint32_t n = SDIO_WAIT_BUDGET;

    sdio->ctrl2 |= 4;
    while (!(sdio->ctrl2 & 2))
        if (--n == 0) break;
}

static inline void sdio_sdclk_off(sdio_base_t *sdio)
{
    sdio->ctrl2 &= ~4u;
}

static inline void sdio_data_timeout(sdio_base_t *sdio, int cnt)
{
    uint32_t tmp = sdio->int_en;

    sdio->int_en = tmp & ~(1u << 20);
    sdio->ctrl2 = (sdio->ctrl2 & ~(15u << 16)) | (uint32_t)cnt << 16;
    sdio->int_en = tmp;
}

static inline void sdio_blksize(sdio_base_t *sdio, int size, int count,
                                int dma_size)
{
    uint32_t a = (uint32_t)count << 16 | (uint32_t)(size & 0x1000) << 3 |
                 (uint32_t)dma_size << 12 | (uint32_t)(size & 0xfff);

    sdio->blk_size = a;
}

/* SDIO pinmux (SC6530: clk0 0x250 .. d3 0x268 — 6 regs). fpdoom's PROVEN
 * RMW form: clear bits 6-7, set bit 7 (0x80) for the SDIO function. The
 * dump's 0x100 is the BOOT-TIME state, not the SDIO function select. */
static void sdio_pin_init(int state)
{
    uint32_t val = state ? 0x40u : 0x80u, mask = 0xc0u;
    uint32_t n = 6, addr = 0x8c000250u;

    for (;;) {
        addr += 4;
        REG32(addr) = (REG32(addr) & ~mask) | val;
        if (!--n) break;
    }
    DELAY(100);
}

/* Iteration-based busy delay (~208 MHz, ~5 cycles/iter, 1M ~= 25 ms). */
static void sdio_busy_ms(uint32_t ms)
{
    volatile uint32_t d = ms * 40000u;

    while (d--)
        ;
}

static inline void sdio_sdfreq_val(sdio_base_t *sdio, unsigned val)
{
    sdio->ctrl2 = (sdio->ctrl2 & ~(0xffu << 8)) | (uint32_t)val << 8;
}

static void sdio_reset(void)
{
    REG32(0x20500020) = 0x100;
    DELAY(1000);
    REG32(0x20500030) = 0x100;
    DELAY(1000);
}

static inline void sdio_slot(unsigned slot)
{
    uint32_t mask = 3u << 2;

    REG32(0x205000b0) = (REG32(0x205000b0) & ~mask) | (uint32_t)slot << 2;
}

/* ---- controller init (fpdoom sdio_init, SC6530 branch) ------------------ */

void sdio_init(void)
{
    sdio_base_t *sdio = SDIO0_BASE;

    logf("sdio: 1 ahb");
    REG32(0x20500060) = 0x400;
    sdio_reset();
    sdio_slot(0);

    REG32(0x8b000040) = (REG32(0x8b000040) & ~(3u << 28)) | 1u << 28;

    sdio_sw_reset(sdio);
    sdio_intclk_on(sdio);
    sdio_sw_reset(sdio);
    sdio_reset();

    logf("sdio: 2 ctrl");
    sdio->ctrl1 = (sdio->ctrl1 & ~0x22u) | 0;
    sdio->ctrl1 &= ~4u;

    /* The pinmux writes are atomic w.r.t. IRQs (the tick must not
     * interrupt them — HW hang). Rockbox runs with IRQs on; mask here. */
    logf("sdio: 3 pinmux");
    disable_irq();
    sdio_pin_init(0);
    enable_irq();

    logf("sdio: 4 ldo");
    /* SD power LDOs: adi_write 0x82001184 &= ~1 ; 0x820011a4 |= 1 (bounded
     * ADI mailbox, same as the B310E-OS keypad driver). */
    {
        uint32_t a, n;

        REG32(0x82000018) = 0x82001184u & 0xfffu;
        a = 0; n = SDIO_WAIT_BUDGET;
        while ((a = REG32(0x8200001cu)) >> 31)
            if (--n == 0) break;
        a &= 0xffffu;
        n = SDIO_WAIT_BUDGET;
        while (REG32(0x82000020u) & (1u << 9))
            if (--n == 0) break;
        REG32(0x82001184u) = a & ~1u;
        n = SDIO_WAIT_BUDGET;
        while (!(REG32(0x82000020u) & (1u << 8)))
            if (--n == 0) break;

        REG32(0x82000018) = 0x820011a4u & 0xfffu;
        a = 0; n = SDIO_WAIT_BUDGET;
        while ((a = REG32(0x8200001cu)) >> 31)
            if (--n == 0) break;
        a &= 0xffffu;
        n = SDIO_WAIT_BUDGET;
        while (REG32(0x82000020u) & (1u << 9))
            if (--n == 0) break;
        REG32(0x820011a4u) = a | 1u;
        n = SDIO_WAIT_BUDGET;
        while (!(REG32(0x82000020u) & (1u << 8)))
            if (--n == 0) break;
    }

    sdio_sdfreq_val(sdio, 0x40);            /* 750 kHz for card init */
    sdio_intclk_on(sdio);
    sdio_sdclk_on(sdio);
    logf("sdio: 5 clk ok");
    sdio_busy_ms(350);                      /* card power-up time */
    logf("sdio: 5b delay ok");

    sdio_data_timeout(sdio, 14);
    logf("sdio: 6 done");
}

/* ---- command engine (fpdoom sdio_cmd, SC6530 path) ---------------------- */

#define SDIO_MAKE_CMD(cmd, resp) ((uint32_t)(cmd) << 24 | SDIO_CMD_RESP_##resp)

#define SDIO_CMD0_TR SDIO_MAKE_CMD(0, 0)
#define SDIO_CMD0_INT SDIO_INT_CMD_COMPLETE

#define SDIO_INT_COMMON (SDIO_INT_CMD_COMPLETE | SDIO_INT_TARGET_RESP | \
    SDIO_INT_CMD_INDEX | SDIO_INT_CMD_END | SDIO_INT_CMD_CRC | \
    SDIO_INT_CMD_TIMEOUT)

#define SDIO_CMD2_INT (SDIO_INT_CMD_COMPLETE | SDIO_INT_TARGET_RESP | \
    SDIO_INT_CMD_END | SDIO_INT_CMD_CRC | SDIO_INT_CMD_TIMEOUT)
#define SDIO_CMD9_INT SDIO_CMD2_INT

#define SDIO_CMD2_TR (SDIO_MAKE_CMD(2, 136) | SDIO_CMD_CRC_CHK)
#define SDIO_CMD3_TR (SDIO_MAKE_CMD(3, 48) | SDIO_CMD_IND_CHK | SDIO_CMD_CRC_CHK)
#define SDIO_CMD7_TR (SDIO_MAKE_CMD(7, 48_BUSY) | SDIO_CMD_IND_CHK | SDIO_CMD_CRC_CHK)
#define SDIO_CMD8_TR (SDIO_MAKE_CMD(8, 48) | SDIO_CMD_IND_CHK | SDIO_CMD_CRC_CHK)
#define SDIO_CMD9_TR (SDIO_MAKE_CMD(9, 136) | SDIO_CMD_CRC_CHK)
#define SDIO_CMD16_TR (SDIO_MAKE_CMD(16, 48) | SDIO_CMD_IND_CHK | SDIO_CMD_CRC_CHK)
#define SDIO_CMD55_TR (SDIO_MAKE_CMD(55, 48) | SDIO_CMD_IND_CHK | SDIO_CMD_CRC_CHK)

#define SDIO_CMD6_TR (SDIO_MAKE_CMD(6, 48) | SDIO_CMD_IND_CHK | \
    SDIO_CMD_CRC_CHK | SDIO_CMD_DMA | SDIO_CMD_DATA_READ | SDIO_CMD_DATA)
#define SDIO_CMD17_TR (SDIO_MAKE_CMD(17, 48) | SDIO_CMD_IND_CHK | \
    SDIO_CMD_CRC_CHK | SDIO_CMD_DMA | SDIO_CMD_DATA_READ | SDIO_CMD_DATA)
#define SDIO_CMD24_TR (SDIO_MAKE_CMD(24, 48) | SDIO_CMD_IND_CHK | \
    SDIO_CMD_CRC_CHK | SDIO_CMD_DMA | SDIO_CMD_DATA)

#define SDIO_CMD6_INT (SDIO_INT_CMD_COMPLETE | SDIO_INT_TARGET_RESP | \
    SDIO_INT_CMD_END | SDIO_INT_CMD_CRC | SDIO_INT_CMD_TIMEOUT | \
    SDIO_INT_DATA_END | SDIO_INT_DATA_CRC | SDIO_INT_DATA_TIMEOUT | \
    SDIO_INT_TR_COMPLETE | SDIO_INT_DMA)
#define SDIO_CMD17_INT SDIO_CMD6_INT
#define SDIO_CMD24_INT ((SDIO_CMD6_INT) & ~SDIO_INT_DATA_END)

#define SDIO_ACMD6_TR (SDIO_MAKE_CMD(6, 48) | SDIO_CMD_IND_CHK | SDIO_CMD_CRC_CHK)
#define SDIO_ACMD41_TR SDIO_MAKE_CMD(41, 48)
#define SDIO_ACMD41_INT (SDIO_INT_CMD_COMPLETE | SDIO_INT_TARGET_RESP | \
    SDIO_INT_CMD_END | SDIO_INT_CMD_TIMEOUT)

/* Returns the int_st status word; non-ERR + resp requested -> resp filled. */
static uint32_t sdio_cmd(uint32_t cmd_tr, uint32_t arg, uint32_t cmd_int,
                         void *data, int size, uint32_t *resp)
{
    sdio_base_t *sdio = SDIO0_BASE;
    uint32_t tmp;

    sdio_sdclk_on(sdio);

    tmp = 0x11ff01ff;
    sdio->int_en &= ~tmp;
    sdio->int_st = tmp;
    sdio->int_en |= cmd_int;

    if (data) {
        commit_discard_dcache();            /* whole-cache clean+inv */
        sdio->dma_addr = (uint32_t)(uintptr_t)data;
        sdio_blksize(sdio, size, 1, 7);
    }
    sdio->arg = arg;
    sdio->tr_mode = (sdio->tr_mode & 0xc004ff80u) | cmd_tr;

    tmp = data ? (SDIO_INT_TR_COMPLETE | SDIO_INT_ERR)
               : (SDIO_INT_CMD_COMPLETE | SDIO_INT_ERR);
    {
        uint32_t np = SDIO_WAIT_BUDGET;

        do {
            cmd_int = sdio->int_st;
        } while (!(cmd_int & tmp) && --np != 0);
    }

    sdio->ctrl2 |= 3u << 25;                /* SW_RST_DAT | SW_RST_CMD */
    {
        uint32_t np = SDIO_WAIT_BUDGET;

        while (sdio->ctrl2 & (3u << 25))
            if (--np == 0) break;
    }

    if (!(cmd_int & SDIO_INT_ERR) && resp) {
        resp[0] = sdio->resp[0];
        resp[1] = sdio->resp[1];
        resp[2] = sdio->resp[2];
        resp[3] = sdio->resp[3];
    }

    sdio_sdclk_off(sdio);
    return cmd_int;
}

/* ---- CSD parsing (standard SD) ------------------------------------------
 * CMD9's 128-bit CSD arrives in resp[0..3] = CSD[127:96]..[31:0]. */
static void sdio_parse_csd(uint32_t *resp)
{
    uint32_t csd0 = resp[0], csd1 = resp[1], csd2 = resp[2];
    unsigned csd_ver = (csd0 >> 30) & 3;    /* CSD[127:126] */

    if (csd_ver == 0) {
        /* v1 (SDSC): READ_BL_LEN CSD[83:80], C_SIZE CSD[73:62],
         * C_SIZE_MULT CSD[49:47] — capacity = (C_SIZE+1) << (CMULT+2)
         * blocks of 2^READ_BL_LEN bytes. */
        unsigned rbl   = (csd1 >> 16) & 0xf;
        unsigned long csize = ((unsigned long)(csd1 & 0x3ff) << 2) |
                              ((csd2 >> 30) & 3);
        unsigned cmult = (csd2 >> 15) & 7;
        unsigned long blocks = (csize + 1) << (cmult + 2);

        if (rbl >= 9)
            sdio_num_blocks = blocks << (rbl - 9);
        else
            sdio_num_blocks = blocks >> (9 - rbl);
    } else if (csd_ver == 1) {
        /* v2 (SDHC/SDXC): C_SIZE CSD[69:48] — capacity =
         * (C_SIZE+1) * 512 KiB; 512 KiB / 512 B = 1024 blocks. */
        unsigned long csize = ((unsigned long)(csd1 & 0x3f) << 16) |
                              (csd2 >> 16);

        sdio_num_blocks = (csize + 1) << 10;
    } else {
        sdio_num_blocks = 0;
    }
}

/* ---- card init (fpdoom sdcard_init, SC6530 branch) ----------------------
 * Return codes (diag-sd stage markers):
 *   0 OK; 1 CMD8 bad echo; 2 no card; 3 CID; 4 RCA; 5 CSD; 6 select;
 *   7 4-bit; 8 blocklen. */
int sdcard_init(void)
{
    uint32_t resp[4], sd_int;
    uint32_t sd_ver = 0, rca, tries;
    sdio_base_t *sdio = SDIO0_BASE;

    sdio_cmd(SDIO_CMD0_TR, 0, SDIO_CMD0_INT, NULL, 0, NULL);

    for (tries = 0; tries < 30; tries++) {
        sd_int = sdio_cmd(SDIO_CMD8_TR, 0x1aa, SDIO_INT_COMMON, NULL, 0, resp);
        if (sd_int & SDIO_INT_CMD_TIMEOUT) {
            sdio_busy_ms(20);
        } else if ((sd_int & SDIO_INT_CMD_COMPLETE) && resp[0] == 0x1aa) {
            sd_ver = 1;
            break;
        } else {
            return 1;
        }
    }

    for (tries = 0; tries < 150; tries++) {
        sd_int = sdio_cmd(SDIO_CMD55_TR, 0, SDIO_INT_COMMON, NULL, 0, NULL);
        if (sd_int & SDIO_INT_CMD_COMPLETE)
            sd_int = sdio_cmd(SDIO_ACMD41_TR, 0xff8000u | sd_ver << 30,
                              SDIO_ACMD41_INT, NULL, 0, resp);
        if (!(sd_int & SDIO_INT_CMD_TIMEOUT)) {
            if (!(sd_int & SDIO_INT_CMD_COMPLETE)) break;
            if ((int32_t)resp[0] < 0) goto acmd51_done;
        }
        sdio_busy_ms(20);
    }
    return 2;

acmd51_done:
    sdio_shl = (sd_ver == 1) ? 0 : 9;

    sd_int = sdio_cmd(SDIO_CMD2_TR, 0, SDIO_CMD2_INT, NULL, 0, resp);
    if (!(sd_int & SDIO_INT_CMD_COMPLETE)) return 3;

    sd_int = sdio_cmd(SDIO_CMD3_TR, 0, SDIO_INT_COMMON, NULL, 0, resp);
    if (!(sd_int & SDIO_INT_CMD_COMPLETE)) return 4;
    rca = resp[0] >> 16;
    rca <<= 16;

    sd_int = sdio_cmd(SDIO_CMD9_TR, rca, SDIO_CMD9_INT, NULL, 0, resp);
    if (!(sd_int & SDIO_INT_CMD_COMPLETE)) return 5;
    sdio_parse_csd(resp);                   /* -> sdio_num_blocks */

    sdio_sdfreq_val(sdio, 1);               /* 24 MHz */

    sd_int = sdio_cmd(SDIO_CMD7_TR, rca, SDIO_INT_COMMON, NULL, 0, resp);
    if (!(sd_int & SDIO_INT_CMD_COMPLETE)) return 6;

    sd_int = sdio_cmd(SDIO_CMD55_TR, rca, SDIO_INT_COMMON, NULL, 0, NULL);
    if (sd_int & SDIO_INT_CMD_COMPLETE)
        sd_int = sdio_cmd(SDIO_ACMD6_TR, 2, SDIO_INT_COMMON, NULL, 0, resp);
    if (!(sd_int & SDIO_INT_CMD_COMPLETE)) return 7;
    sdio->ctrl1 = (sdio->ctrl1 & ~0x22u) | 2;   /* 4-bit mode */

    sd_int = sdio_cmd(SDIO_CMD16_TR, 512, SDIO_INT_COMMON, NULL, 0, resp);
    if (!(sd_int & SDIO_INT_CMD_COMPLETE)) return 8;

    return 0;
}

/* ---- block I/O (fpdoom sdio_read_block / sdio_write_block) --------------
 * buf MUST be an IRAM, 512-byte-aligned buffer (SDIO DMA into PSRAM hangs
 * this phone). The logical block index is shifted by sdio_shl here. */

int sdio_read_block(uint32_t idx, uint8_t *buf)
{
    uint32_t sd_int;

    idx <<= sdio_shl;
    sd_int = sdio_cmd(SDIO_CMD17_TR, idx, SDIO_CMD17_INT, buf, 512, NULL);
    return (sd_int & SDIO_INT_CMD_COMPLETE) ? 0 : -1;
}

int sdio_write_block(uint32_t idx, uint8_t *buf)
{
    uint32_t sd_int;

    idx <<= sdio_shl;
    sd_int = sdio_cmd(SDIO_CMD24_TR, idx, SDIO_CMD24_INT, buf, 512, NULL);
    return (sd_int & SDIO_INT_CMD_COMPLETE) ? 0 : -1;
}
