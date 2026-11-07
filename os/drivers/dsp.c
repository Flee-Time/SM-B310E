/* B310E OS — the SC6530C's DSP and a receive-only layer 1 (os/dsp.h).
 *
 * Boot: the sequence verified on the phone on 2026-08-27 with
 * firmware/arch/diag_dsp_main.c (git e2c0e83), itself register for
 * register the stock downloader (FUN_0003aa76 and FUN_0003a810 in the
 * e52q7a image):
 *   - reset held through APB_EB0 bit 16 (0x8b000060 set / 0x64 clear);
 *   - the DSP strap USER_RST_MODE in 0x8b0001c0 bits 7:3, the boot vector
 *     0xa800 in 0x8b0001a0 bits 15:0 and ARM_BOOT_EN (bit 16);
 *   - after the release the boot ROM answers DSP_READY in share memory
 *     (0x10000000: arm_ctl +0, dsp_ctl +2, dl_offset +4, dl_block_size +6,
 *     data +8) and takes the program in 1024-halfword blocks, each with
 *     DATA_READY / READY_TO_COPY / START_COPY / COPY_DONE and a DSP
 *     interrupt (0x8b000140 bit 2, cleared through 0x8b000160);
 *   - BOOT_DONE, 13 ms, then strap EXT_PROG_MODE, BOOT_EN off and a second
 *     reset pulse start the program.
 * The data area takes 16-bit stores only (the stock uses sci_mem16cpy);
 * byte stores corrupted the program on the phone.
 *
 * The program comes from this phone's own NOR: the LZMA image at 0xcc874
 * (65304 bytes, the stock layer1_dsp_download.c fallback table at
 * 0xdc78c). It is used only when its CRC matches the e52q7a image; the
 * first segment of its five-entry table (match 1: 66861 halfwords) is
 * decoded into the heap and downloaded, and the segment's CRC is checked
 * too. Nothing of it is carried in the OS.
 *
 * Bus safety: on the phone, reads of DSP-side addresses above the 4 KB
 * share RAM (0x1000fb9d-0x1000fffc and others) stalled the AHB for good.
 * This driver only touches 0x10000000-0x10000fff, and every wait is
 * bounded.
 *
 * Layer 1 (receive only): the share-memory rings of the stock
 * sharedmem_proc.c, the power-measurement frame instruction of
 * frameinstrutil.c (type 11, six arfcn indexes) and its POWERMONQ results
 * (rxlevmon.c). See docs/gsm-plan.md for what is read [C] and what is
 * guessed. Only frame types 0 (idle) and 11 are ever posted; the TX
 * buffers (+0x240) are never written.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/arch.h"
#include "os/dsp.h"
#include "os/fs.h"
#include "os/kernel.h"
#include "os/lib.h"
#include "os/sprd_lzma.h"

/* ---- registers ----------------------------------------------------------- */
#define APB_EB0_SET     0x8b000060u     /* bit 16: DSP soft reset held */
#define APB_EB0_CLR     0x8b000064u
#define APB_INT_STS0    0x8b000140u     /* bit 2 DSP_IRQ, bit 3 DSP_FRQ */
#define APB_INT_SET_CLR 0x8b000160u     /* write 1 MCU_IRQ_SET, 4 DSP_IRQ_CLR,
                                           8 DSP_FRQ_CLR */
#define APB_MCU_CTL0    0x8b0001a0u     /* boot address 15:0, ARM_BOOT_EN 16 */
#define APB_DSP_CTL0    0x8b0001c0u     /* strap 7:3 */
#define DSP_RESET       0x10000u
#define BOOT_VECTOR     0xa800u
#define USER_RST_MODE   4u
#define EXT_PROG_MODE   0u

/* ---- share memory --------------------------------------------------------- */
#define SHARE           0x10000000u
#define SHARE_SIZE      0x1000u         /* the only window ever touched */
#define SHARE_CLEAR     0xfc0u          /* stock layer-1 start clears this */
#define SHARE_START     0xffeu          /* stock writes 0x8181 here */
#define START_MARK      0x8181u

#define ARM_READY       0x0001u
#define ARM_DATA_READY  0x0002u
#define ARM_START_COPY  0x0004u
#define ARM_BOOT_DONE   0x0008u
#define DSP_READY       0x0001u
#define DSP_READY_TO_COPY 0x0002u
#define DSP_COPY_DONE   0x0004u
#define BLOCK_HW        1024u

/* layer-1 rings (offsets in share memory) */
#define FI_BASE         0x000u          /* 26 x 22 bytes */
#define FI_SLOTS        26u
#define FI_SIZE         22u
#define FI_IN           0x23cu
#define FI_OUT          0x23eu
#define FI_EXT          0xeeau          /* 26 x 6 bytes */
#define CMD_BASE        0x33cu          /* 8 x 10 bytes */
#define CMD_SLOTS       8u
#define CMD_IN          0x38cu
#define CMD_OUT         0x38eu
#define PM_BASE         0x5c0u          /* POWERMONQ: 30 x 8 bytes */
#define PM_SLOTS        30u
#define PM_IN           0x6b0u
#define PM_OUT          0x6b2u
#define REC_DATA        0x6f6u          /* parameter record body */
#define REC_MAX_HW      0x280u          /* up to 0xbf6, the next area */
#define FN_MOD          0x297000u       /* hyperframe */
#define FI_IDLE         0u
#define FI_POWER        11u
#define FI_ASK_MORE     0x8200u

/* ---- the image in NOR (e52q7a) -------------------------------------------- */
#define NOR_IMAGE       0x000cc874u
#define NOR_IMAGE_LEN   0xff18u
#define IMAGE_CRC       0xea5752b2u     /* CRC-32 (MSB first) of those bytes */
#define IMAGE_UNPACKED  267484u
#define SEG_TABLE       0x28u           /* 5 x { u32 match, u32 halfwords } */
#define SEG_MATCH       1u
#define SEG_HW          66861u
#define SEG_CRC         0x25db38c5u
#define SEG_FIRST_WORD  0x4180u         /* TeakLite br, the reset vector */
#define REC_MARK0       0x5a5au         /* parameter records follow */
#define REC_MARK1       0xa5a5u

/* waits, in microseconds */
#define WAIT_READY_US   500000u
#define WAIT_BLOCK_US   200000u
#define WAIT_CMD_US     50000u
#define KEEPALIVE_US    1000u

volatile dsp_info_t dsp_info;

static uint16_t *records;               /* the image's parameter records */
static uint32_t records_hw;
static uint32_t frame_number;
static bool irq_on, cpu_held;
static uint32_t storm_tick, storm_count;

/* ---- helpers --------------------------------------------------------------- */

static uint32_t crc32_msb(const uint8_t *p, uint32_t len, uint32_t crc)
{
    while (len--) {
        crc ^= (uint32_t)*p++ << 24;
        for (int b = 0; b < 8; b++)
            crc = crc & 0x80000000u ? crc << 1 ^ 0x04c11db7u : crc << 1;
    }
    return crc;
}

static inline volatile uint16_t *sm(uint32_t off)
{
    return (volatile uint16_t *)(SHARE + (off & (SHARE_SIZE - 2)));
}

/* The stock's share-memory discipline (FUN_00061040 / FUN_00061066):
 * write until it reads back, read until two reads agree; six tries. */
static void sm_w16(uint32_t off, uint16_t v)
{
    volatile uint16_t *p = sm(off);

    for (int i = 0; i < 6; i++) {
        *p = v;
        if (*p == v)
            return;
    }
}

static uint16_t sm_r16(uint32_t off)
{
    volatile uint16_t *p = sm(off);
    uint16_t a = *p;

    for (int i = 0; i < 6; i++) {
        uint16_t b = *p;

        if (a == b)
            break;
        a = b;
    }
    return a;
}

static void sm_clear(uint32_t off, uint32_t len)
{
    for (uint32_t i = 0; i < len; i += 2)
        *sm(off + i) = 0;
}

/* Waits for a dsp_ctl bit, re-writing arm_ctl now and then (the SDK's
 * keep-alive rule, kept from the verified diagnostic). */
static int wait_dsp(uint16_t mask, uint16_t keepalive, uint32_t budget_us)
{
    for (uint32_t us = 0, gap = 0; us < budget_us; us += 10, gap += 10) {
        if (sm_r16(2) & mask)
            return 0;
        if (gap >= KEEPALIVE_US) {
            gap = 0;
            sm_w16(0, keepalive);
        }
        udelay(10);
    }
    return -1;
}

static int wait_dsp_irq(uint32_t budget_us)
{
    for (uint32_t us = 0; us < budget_us; us += 10) {
        if (REG32(APB_INT_STS0) & 4u)
            return 0;
        udelay(10);
    }
    return -1;
}

static void hold_cpu(bool on)
{
    if (on && !cpu_held)
        cpufreq_hold();
    else if (!on && cpu_held)
        cpufreq_release();
    cpu_held = on;
}

/* ---- interrupt ------------------------------------------------------------- */

static sem_t dsp_sem;

static void dsp_isr(void)
{
    uint32_t st = REG32(APB_INT_STS0);
    uint32_t now = kernel_ticks();

    if (st & 4u)
        REG32(APB_INT_SET_CLR) = 4u;
    if (st & 8u)
        REG32(APB_INT_SET_CLR) = 8u;
    dsp_info.irqs++;
    /* A line that does not clear would starve everything: give up on it
     * after 200 interrupts within one millisecond tick. */
    if (now != storm_tick) {
        storm_tick = now;
        storm_count = 0;
    }
    if (++storm_count > 200) {
        irq_disable_line(IRQ_DSP);
        irq_on = false;
        dsp_info.irq_storms++;
    }
    sem_post(&dsp_sem);
}

static void irq_start(void)
{
    static bool registered;

    if (!registered) {
        sem_init(&dsp_sem, 0);
        irq_register(IRQ_DSP, dsp_isr);
        registered = true;
    }
    REG32(APB_INT_SET_CLR) = 4u | 8u;
    irq_enable_line(IRQ_DSP);
    irq_on = true;
}

static void irq_stop(void)
{
    if (irq_on)
        irq_disable_line(IRQ_DSP);
    irq_on = false;
}

/* ---- image ------------------------------------------------------------------ */

static uint32_t le32(const uint8_t *p)
{
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* Decodes the program segment from NOR; *seg points into the returned
 * buffer (kfree it). Keeps a copy of the parameter records. */
static uint8_t *load_program(const uint16_t **seg, int *err)
{
    const uint8_t *nor = (const uint8_t *)NOR_IMAGE;
    uint32_t want = SEG_TABLE + SEG_HW * 2, off = SEG_TABLE;
    uint8_t *buf;
    const uint16_t *w;
    uint32_t i;

    if (sprd_lzma_size(nor, NOR_IMAGE_LEN) != IMAGE_UNPACKED ||
        crc32_msb(nor, NOR_IMAGE_LEN, 0xffffffffu) != IMAGE_CRC) {
        *err = DSP_EIMAGE;
        return NULL;
    }
    dsp_info.stage = 1;
    if (heap_free_bytes() < want + 64 * 1024 || !(buf = kmalloc(want))) {
        *err = DSP_ENOMEM;
        return NULL;
    }
    if (sprd_lzma_decode(nor, NOR_IMAGE_LEN, buf, want) != SPRD_LZMA_OK)
        goto bad;
    /* the segment table: the first entry is ours (match 1) */
    if (le32(buf) != SEG_MATCH || le32(buf + 4) != SEG_HW)
        goto bad;
    for (i = 0; i < SEG_TABLE; i += 8)
        if (le32(buf + i) == SEG_MATCH)
            break;
    if (i != 0 || crc32_msb(buf + off, SEG_HW * 2, 0xffffffffu) != SEG_CRC)
        goto bad;
    w = (const uint16_t *)(buf + off);
    if (w[0] != SEG_FIRST_WORD)
        goto bad;

    /* The stock (FUN_00074430) scans the program for 5a5a a5a5: the
     * records after it are sent once layer 1 starts. */
    kfree(records);
    records = NULL;
    records_hw = 0;
    for (i = 0; i + 1 < SEG_HW; i++) {
        if (w[i] == REC_MARK0 && w[i + 1] == REC_MARK1) {
            records_hw = SEG_HW - (i + 2);
            records = kmalloc(records_hw * 2);
            if (records)
                memcpy(records, w + i + 2, records_hw * 2);
            else
                records_hw = 0;
            break;
        }
    }
    *seg = w;
    dsp_info.stage = 2;
    return buf;
bad:
    kfree(buf);
    *err = DSP_EDECODE;
    return NULL;
}

/* ---- boot ------------------------------------------------------------------- */

static int download_block(const uint16_t *prog, uint32_t off, uint32_t n)
{
    volatile uint16_t *data = sm(8);

    sm_w16(4, (uint16_t)off);           /* dl_offset (16 bits, as verified) */
    sm_w16(6, (uint16_t)n);             /* dl_block_size */
    for (uint32_t i = 0; i < n; i++)
        data[i] = prog[off + i];        /* halfword stores only */

    sm_w16(0, ARM_READY | ARM_DATA_READY);
    if (wait_dsp(DSP_READY_TO_COPY, ARM_READY | ARM_DATA_READY, WAIT_BLOCK_US))
        return -1;
    sm_w16(0, sm_r16(0) & ~ARM_DATA_READY);
    sm_w16(0, sm_r16(0) | ARM_START_COPY);
    if (wait_dsp(DSP_COPY_DONE, ARM_START_COPY, WAIT_BLOCK_US))
        return -1;
    if (wait_dsp_irq(WAIT_BLOCK_US))
        return -1;
    REG32(APB_INT_SET_CLR) = 4u | 8u;
    sm_w16(0, sm_r16(0) & ~ARM_START_COPY);
    return 0;
}

static int download(const uint16_t *prog, uint32_t words)
{
    uint32_t blocks = (words + BLOCK_HW - 1) / BLOCK_HW;

    /* reset held, control block cleared, ARM_READY */
    REG32(APB_EB0_SET) = DSP_RESET;
    for (uint32_t i = 0; i < 10; i++)
        sm_w16(i * 2, 0);
    sm_w16(0, ARM_READY);

    REG32(APB_DSP_CTL0) = (REG32(APB_DSP_CTL0) & ~0xf8u) | (USER_RST_MODE << 3);
    REG32(APB_MCU_CTL0) = (REG32(APB_MCU_CTL0) & 0xffff0000u) | BOOT_VECTOR;
    REG32(APB_MCU_CTL0) |= 0x10000u;    /* ARM_BOOT_EN */
    sm_w16(0, ARM_READY);
    dsp_info.stage = 3;

    REG32(APB_EB0_CLR) = DSP_RESET;     /* the boot ROM runs */
    REG32(APB_INT_SET_CLR) = 4u;
    if (wait_dsp(DSP_READY, ARM_READY, WAIT_READY_US))
        return DSP_ENOREADY;
    dsp_info.ready_word = sm_r16(2);
    dsp_info.stage = 4;

    for (uint32_t b = 0; b < blocks; b++) {
        uint32_t off = b * BLOCK_HW;
        uint32_t n = off + BLOCK_HW <= words ? BLOCK_HW : words - off;

        if (download_block(prog, off, n))
            return DSP_EBLOCK;
        dsp_info.blocks = b + 1;
        dsp_info.program_words = off + n;
    }
    sm_w16(0, ARM_BOOT_DONE);
    dsp_info.stage = 5;

    /* the stock sleeps 13 ms before changing the strap */
    thread_sleep(13);
    REG32(APB_DSP_CTL0) = (REG32(APB_DSP_CTL0) & ~0xf8u) | (EXT_PROG_MODE << 3);
    REG32(APB_MCU_CTL0) &= ~0x10000u;
    REG32(APB_EB0_SET) = DSP_RESET;
    udelay(10);
    REG32(APB_EB0_CLR) = DSP_RESET;     /* the program runs */
    dsp_info.stage = 6;
    return DSP_OK;
}

int dsp_boot(void)
{
    const uint16_t *prog;
    uint8_t *buf;
    uint32_t t0 = kernel_ticks();
    int err = DSP_OK;

    if (dsp_info.state == DSP_BOOTING)
        return DSP_EBLOCK;
    dsp_stop();
    dsp_info.state = DSP_BOOTING;
    dsp_info.stage = 0;
    dsp_info.blocks = dsp_info.program_words = 0;
    dsp_info.ready_word = 0;
    hold_cpu(true);

    buf = load_program(&prog, &err);
    if (buf) {
        sm_clear(0, SHARE_CLEAR);       /* stock: before the download */
        err = download(prog, SEG_HW);
        kfree(buf);
    }
    dsp_info.boot_ms = kernel_ticks() - t0;
    dsp_info.error = err;
    if (err != DSP_OK) {
        REG32(APB_EB0_SET) = DSP_RESET;
        REG32(APB_MCU_CTL0) &= ~0x10000u;
        dsp_info.state = DSP_FAILED;
        hold_cpu(false);
        klog("dsp: boot failed (%s) at stage %lu, %lu blocks, %lu ms",
             dsp_strerror(err), (unsigned long)dsp_info.stage,
             (unsigned long)dsp_info.blocks, (unsigned long)dsp_info.boot_ms);
        return err;
    }
    dsp_info.state = DSP_RUNNING;
    klog("dsp: booted, %lu halfwords in %lu blocks, ready %04lx, %lu ms",
         (unsigned long)dsp_info.program_words, (unsigned long)dsp_info.blocks,
         (unsigned long)dsp_info.ready_word, (unsigned long)dsp_info.boot_ms);
    return DSP_OK;
}

void dsp_stop(void)
{
    irq_stop();
    if (dsp_info.state == DSP_RUNNING || dsp_info.state == DSP_BOOTING) {
        REG32(APB_EB0_SET) = DSP_RESET;
        REG32(APB_MCU_CTL0) &= ~0x10000u;
        REG32(APB_INT_SET_CLR) = 4u | 8u;
    }
    if (dsp_info.state != DSP_FAILED)
        dsp_info.state = DSP_OFF;
    hold_cpu(false);
}

const char *dsp_strerror(int err)
{
    switch (err) {
    case DSP_OK: return "ok";
    case DSP_EIMAGE: return "no DSP image in NOR";
    case DSP_ENOMEM: return "out of memory";
    case DSP_EDECODE: return "image did not decode";
    case DSP_ENOREADY: return "boot ROM silent";
    case DSP_EBLOCK: return "download stalled";
    case DSP_ENOTRUN: return "DSP not running";
    case DSP_ECMD: return "DSP took no command";
    case DSP_EFULL: return "frame ring full";
    }
    return "error";
}

/* ---- layer 1 ---------------------------------------------------------------- */

static void note_fi_out(void)
{
    uint32_t out = sm_r16(FI_OUT);

    if (out != dsp_info.fi_out) {
        dsp_info.fi_out = out;
        dsp_info.fi_moves++;
    }
}

/* ARM->DSP command (FUN_0005f474): 8-slot ring, then the doorbell. */
static int post_cmd(const uint16_t cmd[5])
{
    uint32_t in = sm_r16(CMD_IN), out = sm_r16(CMD_OUT), next;

    if (in >= CMD_SLOTS || out >= CMD_SLOTS)
        return DSP_ECMD;
    next = (in + 1) & (CMD_SLOTS - 1);
    if (next == out)
        return DSP_ECMD;                /* full */
    for (int i = 0; i < 5; i++)
        sm_w16(CMD_BASE + next * 10 + i * 2, cmd[i]);
    sm_w16(CMD_IN, (uint16_t)next);
    REG32(APB_INT_SET_CLR) = 1u;        /* MCU_IRQ_SET (FUN_00067fee) */
    dsp_info.cmds++;
    return DSP_OK;
}

/* Taken: the DSP's "out" caught up, or it interrupted since. */
static int wait_cmd_taken(uint32_t irqs_before)
{
    for (uint32_t us = 0; us < WAIT_CMD_US; us += 100) {
        if (sm_r16(CMD_OUT) == sm_r16(CMD_IN) || dsp_info.irqs != irqs_before)
            return DSP_OK;
        udelay(100);
    }
    return DSP_ECMD;
}

/* The image's parameter records (SharedMemProc_StartTranferDataToDsp,
 * FUN_00060650): each { u16 length (bit 15 a flag), length halfwords
 * starting with the DSP address } goes to +0x6f6, then command 0 with
 * the length word. */
static int send_records(void)
{
    uint32_t p = 0;

    while (p < records_hw) {
        uint16_t len = records[p], n = len & 0x7fffu;
        uint16_t cmd[5] = { 0, 0, len, 0, 0 };
        uint32_t irqs;
        int rc;

        if (!n || n > REC_MAX_HW || p + 1 + n > records_hw)
            break;
        for (uint32_t i = 0; i < n; i++)
            sm_w16(REC_DATA + i * 2, records[p + 1 + i]);
        irqs = dsp_info.irqs;
        rc = post_cmd(cmd);
        if (rc == DSP_OK)
            rc = wait_cmd_taken(irqs);
        if (rc != DSP_OK)
            return rc;
        dsp_info.records++;
        p += 1 + n;
    }
    return DSP_OK;
}

int l1_start(void)
{
    int rc;

    if (dsp_info.state != DSP_RUNNING)
        return DSP_ENOTRUN;
    dsp_info.records = dsp_info.cmds = dsp_info.frames_posted = 0;
    dsp_info.fi_moves = dsp_info.pm_results = 0;
    /* stock FUN_0002e98e: clear, interrupt on, clear, start marker */
    sm_clear(0, SHARE_CLEAR);
    irq_start();
    sm_clear(0, SHARE_CLEAR);
    sm_w16(SHARE_START, START_MARK);
    frame_number = 0;
    dsp_info.fi_out = sm_r16(FI_OUT);
    rc = send_records();
    dsp_info.error = rc;
    if (rc == DSP_OK)
        dsp_info.stage = 7;
    klog("dsp: layer 1 %s, %lu of %lu record halfwords sent, %lu irqs",
         rc == DSP_OK ? "up" : dsp_strerror(rc), (unsigned long)dsp_info.records,
         (unsigned long)records_hw, (unsigned long)dsp_info.irqs);
    return rc;
}

/* [G] the five bands in the stock's index order; their sizes add up to
 * RF_TOTAL_CHANS (971). */
uint16_t l1_arfcn_index(int band, int arfcn)
{
    switch (band) {
    case L1_BAND_GSM900:
        if (arfcn >= 0 && arfcn <= 124)
            return (uint16_t)arfcn;
        if (arfcn >= 975 && arfcn <= 1023)
            return (uint16_t)(125 + arfcn - 975);
        break;
    case L1_BAND_DCS1800:
        if (arfcn >= 512 && arfcn <= 885)
            return (uint16_t)(174 + arfcn - 512);
        break;
    case L1_BAND_PCS1900:
        if (arfcn >= 512 && arfcn <= 810)
            return (uint16_t)(548 + arfcn - 512);
        break;
    case L1_BAND_GSM850:
        if (arfcn >= 128 && arfcn <= 251)
            return (uint16_t)(847 + arfcn - 128);
        break;
    }
    return L1_NO_INDEX;
}

int l1_fi_free(void)
{
    uint32_t in = sm_r16(FI_IN), out = sm_r16(FI_OUT);

    if (in >= FI_SLOTS || out >= FI_SLOTS)
        return 0;
    note_fi_out();
    return (int)((out + FI_SLOTS - in - 1) % FI_SLOTS);
}

/* FUN_0005ef76: the slot after "in" gets the 6 extra bytes and the 22
 * bytes, then "in" moves to it. */
static int post_fi(const uint16_t fi[11], const uint16_t ext[3])
{
    uint32_t in = sm_r16(FI_IN), out = sm_r16(FI_OUT), next;

    if (dsp_info.state != DSP_RUNNING)
        return DSP_ENOTRUN;
    if (in >= FI_SLOTS || out >= FI_SLOTS)
        return DSP_EFULL;
    next = (in + 1) % FI_SLOTS;
    if (next == out)
        return DSP_EFULL;
    for (int i = 0; i < 3; i++)
        sm_w16(FI_EXT + next * 6 + i * 2, ext[i]);
    for (int i = 0; i < 11; i++)
        sm_w16(FI_BASE + next * FI_SIZE + i * 2, fi[i]);
    sm_w16(FI_IN, (uint16_t)next);
    dsp_info.frames_posted++;
    frame_number = (frame_number + 1) % FN_MOD;

    /* FUN_0005f208(3): ask the DSP for an interrupt three frames before
     * the ring runs dry. */
    if ((next + FI_SLOTS - out) % FI_SLOTS > 5) {
        uint32_t slot = (next + FI_SLOTS - 3) % FI_SLOTS;

        sm_w16(FI_BASE + slot * FI_SIZE, sm_r16(FI_BASE + slot * FI_SIZE) | FI_ASK_MORE);
    }
    note_fi_out();
    return DSP_OK;
}

/* The frame instruction skeleton (FUN_0004f5fc / FUN_0004f628): bytes
 * 0..11 zero, 12..21 and the extension 0xff; the frame number in word 1
 * bits 15:9 (high 7 bits) and word 2. */
static void fi_init(uint16_t fi[11], uint16_t ext[3], unsigned type)
{
    for (int i = 0; i < 11; i++)
        fi[i] = i < 6 ? 0 : 0xffffu;
    for (int i = 0; i < 3; i++)
        ext[i] = 0xffffu;
    fi[0] = (uint16_t)(type & 0x1f);
    fi[1] = (uint16_t)(((frame_number >> 16) & 0x7f) << 9);
    fi[2] = (uint16_t)frame_number;
}

int l1_idle_post(int frames)
{
    uint16_t fi[11], ext[3];

    for (int i = 0; i < frames; i++) {
        int rc;

        fi_init(fi, ext, FI_IDLE);
        if ((rc = post_fi(fi, ext)) != DSP_OK)
            return rc;
    }
    return DSP_OK;
}

/* FUN_0004f646: word 0 = 11; measurements 1-4 in words 6-9, 5-6 in the
 * extension's words 0-1; each arfcn index in bits 9:0, the gain hint in
 * 15:10; empty ones stay 0xffff. */
int l1_pm_post(const uint16_t *index, const uint8_t *gain, int n)
{
    uint16_t fi[11], ext[3];

    if (n < 1 || n > L1_PM_PER_FRAME)
        return DSP_ECMD;
    fi_init(fi, ext, FI_POWER);
    for (int k = 0; k < n; k++) {
        uint16_t v = (uint16_t)((index[k] & 0x3ffu) | (uint16_t)(gain[k] & 0x3f) << 10);

        if (k < 4)
            fi[6 + k] = v;
        else
            ext[k - 4] = v;
    }
    return post_fi(fi, ext);
}

/* FUN_0005ed64: "in" is the slot the DSP wrote last, so read from out+1
 * up to and including in. */
int l1_pm_read(l1_pm_t *res, int max)
{
    uint32_t in, out;
    int n = 0;

    if (dsp_info.state != DSP_RUNNING)
        return DSP_ENOTRUN;
    in = sm_r16(PM_IN);
    out = sm_r16(PM_OUT);
    if (in >= PM_SLOTS || out >= PM_SLOTS)
        return 0;
    while (out != in && n < max) {
        out = (out + 1) % PM_SLOTS;
        res[n].index = sm_r16(PM_BASE + out * 8);
        res[n].level = sm_r16(PM_BASE + out * 8 + 2);
        res[n].aux = sm_r16(PM_BASE + out * 8 + 4);
        res[n].status = sm_r16(PM_BASE + out * 8 + 6);
        n++;
    }
    sm_w16(PM_OUT, (uint16_t)out);
    dsp_info.pm_results += (uint32_t)n;
    note_fi_out();
    return n;
}
