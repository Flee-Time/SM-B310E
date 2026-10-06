/*
 * B310E-OS — drivers/sdio.c
 *
 * SD/MMC driver on the SC6530 SDIO0 controller @ 0x20700000. Ported from
 * fpdoom sdio.c (Unlicense, SC6530-only branch — the B310E is SC6530C,
 * _chip == 3). Polled command engine + DMA data path; no interrupts.
 *
 * SAFETY: touches only SDIO0 (0x20700000), AHB power/reset
 * (0x20500060/0x20500020/0x205000b0), the 48 MHz freq gate (0x8b000040),
 * the ADI SD power LDOs (0x82001184/0x820011a4) and the SDIO pinmux
 * registers 0x8c000250-0x264. NOTE: those pinmux regs are a DIFFERENT
 * set from the forbidden 0x8c0002a4 (UART-TX pinmux hangs the B310E) —
 * fpdoom's sdboot runs on this exact phone with them, but they are live
 * pin configuration, so they must be validated on hardware before the
 * 4-bit/high-speed path is trusted. All waits are bounded (poll +
 * timeout via the 1 ms sys timer), so a missing card can never hang the
 * cooperative scheduler.
 *
 * DMA coherency: clean+invalidate the whole D-cache before each DMA
 * transfer (clean_invalidate_dcache — the ARM926 has no set/way ops; the
 * whole-cache op is correct for a 512-byte transfer and matches the
 * LCD's clean_dcache discipline).
 */

#include "os.h"
#include "../arch/chip.h"
#include "sdio.h"

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
};

unsigned sdio_shl;

/* Bounded controller-wait budget: a wedged SDIO controller must not hang
 * the cooperative scheduler. All ctrl2/int_st poll loops below use it. */
#define SDIO_WAIT_BUDGET 1000000u

/* ---- clock/control helpers (fpdoom sdio.c:82-170) ---------------------- */

static void sdio_sw_reset(sdio_base_t *sdio)
{
    uint32_t n = SDIO_WAIT_BUDGET;

    sdio->ctrl2 |= 1 << 24;                 /* SW_RST_ALL */
    while (sdio->ctrl2 & (1 << 24))         /* bounded by budget */
        if (--n == 0) break;
}

static void sdio_intclk_on(sdio_base_t *sdio)
{
    uint32_t n = SDIO_WAIT_BUDGET;

    sdio->ctrl2 |= 1;                       /* INT_CLK_EN */
    while (!(sdio->ctrl2 & 2))              /* INT_CLK_STABLE */
        if (--n == 0) break;
}

static void sdio_sdclk_on(sdio_base_t *sdio)
{
    uint32_t n = SDIO_WAIT_BUDGET;

    sdio->ctrl2 |= 4;                       /* SDCLK_EN */
    while (!(sdio->ctrl2 & 2))              /* INT_CLK_STABLE */
        if (--n == 0) break;
}

static inline void sdio_sdclk_off(sdio_base_t *sdio)
{
    sdio->ctrl2 &= ~4u;                     /* SDCLK_EN */
}

static inline void sdio_data_timeout(sdio_base_t *sdio, int cnt)
{
    uint32_t tmp = sdio->int_en;

    sdio->int_en = tmp & ~(1u << 20);       /* disable DATA_TIMEOUT int */
    sdio->ctrl2 = (sdio->ctrl2 & ~(15u << 16)) | (uint32_t)cnt << 16;
    sdio->int_en = tmp;
}

static inline void sdio_blksize(sdio_base_t *sdio, int size, int count,
                                int dma_size)
{
    /* 4K << dma_size */
    uint32_t a = (uint32_t)count << 16 | (uint32_t)(size & 0x1000) << 3 |
                 (uint32_t)dma_size << 12 | (uint32_t)(size & 0xfff);

    sdio->blk_size = a;
}

/* SDIO pinmux (SC6530: clk0 0x250, clk1 0x254, cmd 0x258, d0 0x25c,
 * d1 0x260, d2 0x264, d3 0x268 — 6 written). fpdoom's PROVEN RMW form
 * (sdio.c:115-152, verified on the B310E via fptest): clear the function
 * bits 6-7 and set bit 7 (0x80) for the SDIO function, preserving the
 * rest of whatever FDL1/the boot left in the register. The dump's pinmap
 * value (0x100) is the BOOT-TIME state, not the SDIO function select —
 * writing it verbatim was a wrong guess that broke card init. */
static void sdio_pin_init(int state)
{
    uint32_t val = state ? 0x40u : 0x80u, mask = 0xc0u;
    uint32_t n = 6, addr = 0x8c000250u;

    for (;;) {
        addr += 4;
        MEM4(addr) = (MEM4(addr) & ~mask) | val;
        if (!--n) break;
    }
    DELAY(100);
}

/* Iteration-based busy delay. The SD path must NOT depend on the
 * free-running counter (0x8100300c): lcd_sys_timer_ms()'s stability loop
 * (`do a=b,b=MEM4() while (a != b)`) spins FOREVER if the counter reads
 * glitch mid-SDIO — the exact stage-5 hardware hang (counter was healthy
 * at the "5 clk" marker, then the wait never returned). A pure volatile
 * spin always terminates. ~208 MHz, ~5 cycles/iter -> 1M iters ~= 25 ms. */
static void sdio_busy_ms(uint32_t ms)
{
    volatile uint32_t d = ms * 40000u;

    while (d--)
        ;
}

static inline void sdio_sdfreq_val(sdio_base_t *sdio, unsigned val)
{
    /* SDCLK = 48 MHz >> val: 0x40 -> 750 kHz, 1 -> 24 MHz, 0 -> 48 MHz */
    sdio->ctrl2 = (sdio->ctrl2 & ~(0xffu << 8)) | (uint32_t)val << 8;
}

static void sdio_reset(void)
{
    /* SDIO0 soft reset via AHB (SC6530: 0x20500020 set / 0x20500030 clr) */
    MEM4(0x20500020) = 0x100;
    DELAY(1000);
    MEM4(0x20500030) = 0x100;
    DELAY(1000);
}

static inline void sdio_slot(unsigned slot)
{
    uint32_t mask = 3u << 2;                /* SC6530: slot at bits 2-3 */

    MEM4(0x205000b0) = (MEM4(0x205000b0) & ~mask) | (uint32_t)slot << 2;
}

/* ---- controller init (fpdoom sdio_init, SC6530 branch) ------------------ */

void sdio_init(void)
{
    sdio_base_t *sdio = SDIO0_BASE;

    kprintf("sdio: 1 ahb\n");
    MEM4(0x20500060) = 0x400;               /* SDIO0 AHB enable */
    sdio_reset();
    sdio_slot(0);

    /* SDIO0 48 MHz source (SC6530: 0x8b000040 bits 28-30 = 1) */
    MEM4(0x8b000040) = (MEM4(0x8b000040) & ~(3u << 28)) | 1u << 28;

    sdio_sw_reset(sdio);
    sdio_intclk_on(sdio);
    sdio_sw_reset(sdio);
    sdio_reset();

    kprintf("sdio: 2 ctrl\n");
    sdio->ctrl1 = (sdio->ctrl1 & ~0x22u) | 0;   /* 1-bit mode */
    sdio->ctrl1 &= ~4u;                         /* HI_SPD_EN off */

    /* The 0x8c SDIO pinmux writes are the ONLY 0x8c writes in the tree and
     * this phone has multiple 0x8c landmines (0x8c0002a4 + at least one in
     * the stock pinmap). On hardware the SD path hung at this stage when
     * the 1 ms tick IRQ was live (diag-sd — no IRQ — works). Mask the
     * tick around the pinmux block so the writes are atomic w.r.t. IRQs. */
    kprintf("sdio: 3 pinmux\n");
    irq_disable();
    sdio_pin_init(0);
    irq_enable();

    kprintf("sdio: 4 ldo\n");
    /* SD power LDOs (SC6530: fpdoom sdio.c:232-233) */
    /* adi_write 0x82001184 &= ~1 ; 0x820011a4 |= 1 */
    {
        /* local ADI mailbox (same as keypad.c), BOUNDED */
        uint32_t a, n;

        MEM4(0x82000018) = 0x82001184u & 0xfffu;
        a = 0; n = SDIO_WAIT_BUDGET;
        while ((a = MEM4(0x8200001cu)) >> 31)
            if (--n == 0) break;
        a &= 0xffffu;
        n = SDIO_WAIT_BUDGET;
        while (MEM4(0x82000020u) & (1u << 9))
            if (--n == 0) break;
        MEM4(0x82001184u) = a & ~1u;
        n = SDIO_WAIT_BUDGET;
        /* fpdoom adi_write: wait until FIFO_EMPTY is SET (write drained),
         * NOT while it is set (inverted form spins on the empty state). */
        while (!(MEM4(0x82000020u) & (1u << 8)))
            if (--n == 0) break;

        MEM4(0x82000018) = 0x820011a4u & 0xfffu;
        a = 0; n = SDIO_WAIT_BUDGET;
        while ((a = MEM4(0x8200001cu)) >> 31)
            if (--n == 0) break;
        a &= 0xffffu;
        n = SDIO_WAIT_BUDGET;
        while (MEM4(0x82000020u) & (1u << 9))
            if (--n == 0) break;
        MEM4(0x820011a4u) = a | 1u;
        n = SDIO_WAIT_BUDGET;
        while (!(MEM4(0x82000020u) & (1u << 8)))
            if (--n == 0) break;
    }

    sdio_sdfreq_val(sdio, 0x40);            /* 750 kHz for card init */
    sdio_intclk_on(sdio);
    sdio_sdclk_on(sdio);
    kprintf("sdio: 5 clk ok\n");
    sdio_busy_ms(350);                      /* card power-up time (bounded) */
    kprintf("sdio: 5b delay ok\n");

    sdio_data_timeout(sdio, 14);
    kprintf("sdio: 6 done\n");
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

#define SDIO_INT_DMA (1u << 3)

/* Returns the int_st status word; non-ERR + resp requested -> resp filled. */
static uint32_t sdio_cmd(uint32_t cmd_tr, uint32_t arg, uint32_t cmd_int,
                         void *data, int size, uint32_t *resp)
{
    sdio_base_t *sdio = SDIO0_BASE;
    uint32_t tmp;

    sdio_sdclk_on(sdio);

    tmp = 0x11ff01ff;
    sdio->int_en &= ~tmp;
    sdio->int_st = tmp;                     /* clear interrupt status */
    sdio->int_en |= cmd_int;

    if (data) {
        clean_invalidate_dcache();           /* whole-cache clean+inv */
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

/* ---- card init (fpdoom sdcard_init, SC6530 branch) ---------------------- */

/* Card init (fpdoom sdcard_init, SC6530 branch).
 *
 * Return codes (diag-sd reports these to pinpoint the failure):
 *   0  OK
 *   1  CMD8: response but not the SD v2 echo (0x1aa) — not an SD card / bus glitch
 *   2  CMD8 + ACMD41: no card detected (commands time out / no ready) — card absent or power/pins wrong
 *   3  CMD2 (CID) failed
 *   4  CMD3 (RCA) failed
 *   5  CMD9 (CSD) failed
 *   6  CMD7 (select) failed
 *   7  ACMD6 (4-bit bus) failed
 *   8  CMD16 (block length) failed
 */
int sdcard_init(void)
{
    uint32_t resp[4], sd_int;
    uint32_t sd_ver = 0, rca, tries;
    sdio_base_t *sdio = SDIO0_BASE;

    sdio_cmd(SDIO_CMD0_TR, 0, SDIO_CMD0_INT, NULL, 0, NULL);

    /* CMD8: SD v2 detection (echo 0x1aa) — timeout = no card. Bounded by
     * an iteration cap + iteration delays only: the free-running counter
     * is deliberately NOT used (its stability loop can hang mid-SDIO). */
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

    /* CMD55 + ACMD41 (HCS): busy-wait until the card reports ready. */
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

    sdio_sdfreq_val(sdio, 1);               /* 24 MHz */

    sd_int = sdio_cmd(SDIO_CMD7_TR, rca, SDIO_INT_COMMON, NULL, 0, resp);
    if (!(sd_int & SDIO_INT_CMD_COMPLETE)) return 6;

    /* 4-bit bus width */
    sd_int = sdio_cmd(SDIO_CMD55_TR, rca, SDIO_INT_COMMON, NULL, 0, NULL);
    if (sd_int & SDIO_INT_CMD_COMPLETE)
        sd_int = sdio_cmd(SDIO_ACMD6_TR, 2, SDIO_INT_COMMON, NULL, 0, resp);
    if (!(sd_int & SDIO_INT_CMD_COMPLETE)) return 7;
    sdio->ctrl1 = (sdio->ctrl1 & ~0x22u) | 2;   /* 4-bit mode */

    /* block length = 512 */
    sd_int = sdio_cmd(SDIO_CMD16_TR, 512, SDIO_INT_COMMON, NULL, 0, resp);
    if (!(sd_int & SDIO_INT_CMD_COMPLETE)) return 8;

    return 0;
}

/* ---- block I/O (fpdoom sdio_read_block / sdio_write_block) -------------- */

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
