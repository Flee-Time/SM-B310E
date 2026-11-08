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

/* The control block at +0xfe0 (DSP address 0xaff0: share memory is DSP word
 * address 0xa800, so ARM offset n is 0xa800 + n / 2). */
#define FE0_CMD         0xfe0u          /* 3: the ping / dump request */
#define FE0_START       0xfe2u          /* dump: first DSP word of the block */
#define FE0_REPLY       0xfe4u          /* ping: the DSP writes 1 */
#define FE0_COUNT       0xfeeu          /* dump: words per block */
#define FE0_MSG         0xff0u          /* DSP->ARM message code */
#define FE0_PARAM       0xff2u
#define FE0_ACK         0xff4u          /* ARM sets 1: the DSP may post another */
#define FE0_LEN         0x1eu
#define SHARE_CLEAR_BOOT 0x2000u        /* FUN_0003ad6e: 8 KB, before the download */
#define NORMAL_START    0xfdeu          /* DSP 0xafef: 0xc396 asks for another start
                                         * path; the stock writes 0xffff (FUN_00060f0a) */

/* The trace (see trace_program()): one word per point, DSP 0xafe0.. */
/* dsp_set_trace(): off, the program goes unpatched, as the stock sends it. */
#define TRACE_OFF       0xfc0u
#define TRACE_DSP       0xafe0u
#define TRACE_MARK(i)   (0x40u + (i))

/* 0x8b0001a0 bits 21:19: stock's PBL sets them, then its boot clears them for
 * good (NOR 0x1a7c0), so they are clear whenever stock's DSP runs. With them
 * set, IRAM above 0x4000c000 holds the ARM's data. The OS sets them at entry. */
#define IRAM_BANKS      (7u << 19)

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
#define RESP_BASE       0x390u          /* DSP->ARM responses: 8 x 10 bytes */
#define RESP_SLOTS      8u
#define RESP_IN         0x3e0u
#define RESP_OUT        0x3e2u
#define REC_DATA        0x6f6u          /* parameter record body */
#define REC_MAX_HW      0x280u          /* up to 0xbf6, the next area */
#define RESP_RECORD_ACK 0x000fu         /* a record's answer, word 0 */
#define CMD_START       0x10u           /* command 0, word 1: see l1_start() */
#define FN_MOD          0x297000u       /* hyperframe */
#define FI_IDLE         0u
#define FI_POWER        11u
#define FI_FCCH         9u
#define FI_SCH          10u
#define FI_BLOCK        2u
#define RX_BASE         0x40cu          /* received blocks: 6 x 72 bytes */
#define RX_SLOTS        6u
#define RX_SIZE         72u
#define RX_IN           0x5bcu
#define RX_OUT          0x5beu
#define BEP_BASE        0xfa0u          /* BEP reports: 7 x 4 bytes */
#define BEP_SLOTS       7u
#define BEP_IN          0xfbcu
#define BEP_OUT         0xfbeu
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
volatile dsp_probe_t dsp_probe_info;

static uint16_t *records;               /* the image's parameter records */
static uint32_t records_hw;
static uint32_t frame_number;
static bool irq_on, cpu_held;
static uint32_t storm_tick, storm_count;
static uint32_t block0_crc;             /* the program's first download block */

static void trace_program(uint16_t *w);
static bool trace_on;
static bool banks_for_arm;              /* the self-test's "as we were" case */

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

/* Stock FUN_0003ab7e, before the download and again after it: the control
 * block cleared and the ack set. The DSP's message routine (0x17df) spins
 * until the ack is 1, so without this its first message hangs it. */
static void fe0_init(void)
{
    sm_clear(FE0_CMD, FE0_LEN);
    *sm(FE0_ACK) = 1;
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
static void l1_service(void);

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
    /* As stock's (FUN_0004ee54): the responses are read here. The ring has 8
     * slots, and the DSP asserts (id 3, at 0x178a) when it finds it full. */
    l1_service();
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
    if (trace_on)
        trace_program((uint16_t *)w);

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

/* ---- start-up trace --------------------------------------------------------- */

/* Is the program executing, and how far does it get? The program goes to the
 * DSP from a heap copy, so that copy can carry small stubs (never the NOR):
 * each writes one word to share memory and goes on as before. They sit in a
 * gap of unused words below 0x6000. Nothing of the program is carried here:
 * the instructions a stub replaces are read from the copy. */
#define TRACE_STUBS     0x5f77u
#define TRACE_STUBS_END 0x6000u

enum { TR_VECTOR, TR_CALL, TR_POINTER };

static const struct {
    uint16_t at;
    uint8_t kind;
} trace_points[] = {
    { 0x0000, TR_VECTOR },              /*  0 reset: it runs at all */
    { 0x0329, TR_CALL },                /*  1 data memory cleared, main called */
    { 0x0493, TR_CALL },                /*  2 main entered */
    { 0x0495, TR_CALL },                /*  3 first init returned */
    { 0x0497, TR_CALL },                /*  4 second init returned */
    { 0x1919, TR_CALL },                /*  5 handlers installed */
    { 0x1925, TR_POINTER },             /*  6 fast-interrupt handler 0x3abe ran */
    { 0x1924, TR_CALL },                /*  7 about to wait, interrupts next */
    { 0x0499, TR_CALL },                /*  8 the wait ended */
    { 0x1916, TR_POINTER },             /*  9 command handler 0x98ab ran */
    { 0x0006, TR_VECTOR },              /* 10 interrupt vector */
    { 0x000e, TR_VECTOR },              /* 11 interrupt vector */
    { 0x0016, TR_VECTOR },              /* 12 interrupt vector */
};
#define TRACE_POINTS (sizeof(trace_points) / sizeof(trace_points[0]))

static void trace_program(uint16_t *w)
{
    uint32_t p = TRACE_STUBS;

    for (uint32_t i = 0; i < TRACE_POINTS; i++) {
        uint32_t at = trace_points[i].at, stub = p;
        bool reset = at == 0;

        if (p + 11 > TRACE_STUBS_END)
            return;
        if (!reset) {
            w[p++] = 0x5e40;            /* push r0 */
            w[p++] = 0x5e42;            /* push r2 */
        }
        w[p++] = 0x5e00;                /* mov addr, r0 */
        w[p++] = (uint16_t)(TRACE_DSP + i);
        w[p++] = (uint16_t)(0x2b00 | TRACE_MARK(i));    /* mov +mark, r2 */
        w[p++] = 0x1840;                /* mov r2, [r0] */
        if (!reset) {
            w[p++] = 0x5e62;            /* pop r2 */
            w[p++] = 0x5e60;            /* pop r0 */
        }
        switch (trace_points[i].kind) {
        case TR_VECTOR:                 /* "br target": the stub, then target */
            w[p++] = 0x4180;
            w[p++] = w[at + 1];
            w[at + 1] = (uint16_t)stub;
            break;
        case TR_POINTER:                /* a handler's address in a mov */
            w[p++] = 0x4180;
            w[p++] = w[at];
            w[at] = (uint16_t)stub;
            break;
        case TR_CALL:                   /* a two-word instruction, moved here */
            w[p++] = w[at];
            w[p++] = w[at + 1];
            w[p++] = 0x4580;            /* ret */
            w[at] = 0x41c0;             /* call stub */
            w[at + 1] = (uint16_t)stub;
            break;
        }
    }
}

static uint32_t trace_read(void)
{
    uint32_t mask = 0;

    for (uint32_t i = 0; i < TRACE_POINTS; i++)
        if (sm_r16(TRACE_OFF + i * 2) == TRACE_MARK(i))
            mask |= 1u << i;
    return mask;
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
    REG32(APB_INT_SET_CLR) = 4u;        /* the stock clears one at a time */
    REG32(APB_INT_SET_CLR) = 8u;
    sm_w16(0, sm_r16(0) & ~ARM_START_COPY);
    return 0;
}

static int download(const uint16_t *prog, uint32_t words)
{
    uint32_t blocks = (words + BLOCK_HW - 1) / BLOCK_HW;

    /* reset held, control block cleared, ARM_READY */
    REG32(APB_EB0_SET) = DSP_RESET;
    if (banks_for_arm)
        REG32(APB_MCU_CTL0) |= IRAM_BANKS;
    else
        REG32(APB_MCU_CTL0) &= ~IRAM_BANKS;
    for (uint32_t i = 0; i < SHARE_CLEAR_BOOT; i += 2)
        *(volatile uint16_t *)(SHARE + i) = 0;
    fe0_init();
    sm_w16(0, ARM_READY);

    REG32(APB_DSP_CTL0) = (REG32(APB_DSP_CTL0) & ~0xf8u) | (USER_RST_MODE << 3);
    REG32(APB_MCU_CTL0) = (REG32(APB_MCU_CTL0) & 0xffff0000u) | BOOT_VECTOR;
    REG32(APB_MCU_CTL0) |= 0x10000u;    /* ARM_BOOT_EN */
    udelay(20);                         /* the stock: 400 loops here */
    sm_w16(0, ARM_READY);
    dsp_info.stage = 3;

    REG32(APB_EB0_CLR) = DSP_RESET;     /* the boot ROM runs */
    REG32(APB_INT_SET_CLR) = 4u;
    if (wait_dsp(DSP_READY, ARM_READY, WAIT_READY_US))
        return DSP_ENOREADY;
    dsp_info.ready_word = sm_r16(2);
    dsp_info.stage = 4;

    /* Stock FUN_0003a92a: offset (b & 0x3f) << 10 is both dl_offset and the
     * source, so blocks 64 and 65 send words 0..1324 again and the boot ROM
     * never gets words 65536.. (the parameter records, sent later by
     * command 0). Copying from b * 1024 put the records over the reset
     * vector. */
    for (uint32_t b = 0; b < blocks; b++) {
        uint32_t pos = b * BLOCK_HW;
        uint32_t n = pos + BLOCK_HW <= words ? BLOCK_HW : words - pos;

        if (download_block(prog, (b & 63u) * BLOCK_HW, n))
            return DSP_EBLOCK;
        dsp_info.blocks = b + 1;
        dsp_info.program_words = pos + n;
    }
    sm_w16(0, ARM_BOOT_DONE);
    dsp_info.stage = 5;

    /* FUN_0003aa76 goes straight on (its "13" is the IRQ line it disables) */
    *sm(NORMAL_START) = 0xffffu;
    REG32(APB_DSP_CTL0) = (REG32(APB_DSP_CTL0) & ~0xf8u) | (EXT_PROG_MODE << 3);
    REG32(APB_MCU_CTL0) &= ~0x10000u;
    REG32(APB_EB0_SET) = DSP_RESET;
    udelay(50);                         /* the stock: 99 loops; at least as long */
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
        fe0_init();
        block0_crc = crc32_msb((const uint8_t *)prog, BLOCK_HW * 2, 0xffffffffu);
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

void dsp_hold_reset(void)
{
    REG32(APB_EB0_SET) = DSP_RESET;
}

void dsp_set_trace(bool on)
{
    trace_on = on;
}

void dsp_stop(void)
{
    irq_stop();
    if (dsp_info.state == DSP_RUNNING || dsp_info.state == DSP_BOOTING) {
        REG32(APB_EB0_SET) = DSP_RESET;
        REG32(APB_MCU_CTL0) &= ~0x10000u;
        REG32(APB_INT_SET_CLR) = 4u | 8u;
    }
    REG32(APB_MCU_CTL0) |= IRAM_BANKS;  /* the reset is held: the ARM's again */
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

/* What the stock's two DSP interrupts do (FUN_0004ee54, FUN_0003ab0a), by
 * polling: the response ring read out and kept, a message on the control
 * block taken and acknowledged, the status bits cleared. */
static void l1_service(void)
{
    volatile dsp_probe_t *p = &dsp_probe_info;
    uint32_t irq = arch_irq_save();     /* the thread polls it, the interrupt too */
    uint32_t in = sm_r16(RESP_IN), out = sm_r16(RESP_OUT);
    uint32_t st = REG32(APB_INT_STS0);

    if (in < RESP_SLOTS && out < RESP_SLOTS && in != out) {
        while (out != in) {
            out = (out + 1) & (RESP_SLOTS - 1);
            /* kept unless it is an ack: there are 64 of those */
            if (sm_r16(RESP_BASE + out * 10) == RESP_RECORD_ACK) {
                for (uint32_t i = 0; i < 5; i++)
                    p->last_ack[i] = sm_r16(RESP_BASE + out * 10 + i * 2);
            } else if (p->reply_kept < DSP_PROBE_REPLIES) {
                for (uint32_t i = 0; i < 5; i++)
                    p->reply[p->reply_kept][i] = sm_r16(RESP_BASE + out * 10 + i * 2);
                p->reply_kept++;
            }
            p->replies++;
        }
        sm_w16(RESP_OUT, (uint16_t)out);
    }
    /* The DSP adds a BEP report per decoded block (DSP 0xfacc). With the ring
     * full it keeps up to 5 more in its own list, then asserts 0x1900. */
    in = sm_r16(BEP_IN);
    out = sm_r16(BEP_OUT);
    if (in < BEP_SLOTS && out < BEP_SLOTS && in != out) {
        while (out != in) {
            out = (out + 1) % BEP_SLOTS;
            p->bep_last[0] = sm_r16(BEP_BASE + out * 4);
            p->bep_last[1] = sm_r16(BEP_BASE + out * 4 + 2);
            p->beps++;
        }
        sm_w16(BEP_OUT, (uint16_t)out);
    }
    if (sm_r16(FE0_ACK) == 0) {
        uint32_t m = (uint32_t)sm_r16(FE0_MSG) << 16 | sm_r16(FE0_PARAM);

        if (!p->msgs) {
            p->msg_first = m;
            p->msg_phase = p->phase;
        }
        p->msg_last = m;
        p->msg_log[p->msgs % 8] = m;
        p->msgs++;
        *sm(FE0_ACK) = 1;
    }
    if (st & 8u)
        REG32(APB_INT_SET_CLR) = 8u;
    p->int_sts |= st;
    arch_irq_restore(irq);
}

void l1_poll(void)
{
    l1_service();
}

/* Taken: the DSP's "out" caught up, or it answered, or it interrupted. */
static int wait_cmd_taken(uint32_t irqs_before)
{
    uint32_t replies = dsp_probe_info.replies;

    for (uint32_t us = 0; us < WAIT_CMD_US; us += 100) {
        l1_service();
        if (sm_r16(CMD_OUT) == sm_r16(CMD_IN) || dsp_probe_info.replies != replies ||
            dsp_info.irqs != irqs_before)
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

/* ---- the parameter blocks ----------------------------------------------------- */

/* The fixed NV list (stock FUN_0002253c): after a 4-byte header, items of
 * { u16 id, u16 bytes, data }, each padded to 4 bytes, up to id 0xffff. Read
 * from this phone's own NOR: it is its calibration, and nothing of it is
 * carried here. */
#define NV_BASE         0x00680000u
#define NV_SPAN         0x00020000u
#define NV_RF_ID        1u              /* 1012 bytes, type 1 */
#define NV_RF_BYTES     0x3f4u
#define NV_CAL_ID       2u              /* types 3..15, after a version word */
#define NV_CAL_BYTES    0x1784u
#define NV_CLASS_ID     3u              /* bytes 42, 44..46: types 4..7 wanted */
#define NV_CLASS_MIN    47u

static const uint8_t *nv_item(uint32_t id, uint32_t min_bytes)
{
    uint32_t p = NV_BASE + 4;

    while (p + 4 <= NV_BASE + NV_SPAN) {
        uint32_t item = *(const uint16_t *)p, len = *(const uint16_t *)(p + 2);

        if (item == 0xffffu)
            return NULL;
        if (item == id)
            return len >= min_bytes ? (const uint8_t *)(p + 4) : NULL;
        p = (p + 4 + len + 3) & ~3u;
    }
    return NULL;
}

/* FUN_000606ea: a block to +0x6f6, then command 0 with the type in word 1. */
static int send_block(uint32_t type, const uint8_t *src, uint32_t bytes)
{
    const uint16_t *w = (const uint16_t *)src;
    uint16_t cmd[5] = { 0, (uint16_t)type, 0, 0, 0 };
    uint32_t irqs;
    int rc;

    for (uint32_t i = 0; i < bytes / 2; i++)
        *sm(REC_DATA + i * 2) = w[i];
    irqs = dsp_info.irqs;
    rc = post_cmd(cmd);
    if (rc == DSP_OK)
        rc = wait_cmd_taken(irqs);
    return rc;
}

/* The list's types 1, 3..15 (table at FUN_000606ea): type 1 is all of NV item
 * 1. The rest lie end to end in NV item 2 behind its version word, in this
 * order of offset: 3, 4, 12, 5, 13, 6, 14, 7, 15 (8..11 are parts of them). */
static int send_parameters(void)
{
    static const struct {
        uint8_t type, wanted_by;        /* a byte of NV item 3, or 0: always */
        uint16_t offset, bytes;
    } cal[] = {
        { 3, 0, 0x0000, 126 }, { 4, 42, 0x007e, 792 }, { 5, 44, 0x0616, 792 },
        { 6, 45, 0x0bae, 792 }, { 7, 46, 0x1146, 792 }, { 8, 0, 0x0300, 70 },
        { 9, 0, 0x0898, 150 }, { 10, 0, 0x0e30, 120 }, { 11, 0, 0x13c8, 50 },
        { 12, 0, 0x0396, 640 }, { 13, 0, 0x092e, 640 }, { 14, 0, 0x0ec6, 640 },
        { 15, 0, 0x145e, 640 },
    };
    const uint8_t *rf = nv_item(NV_RF_ID, NV_RF_BYTES);
    const uint8_t *calib = nv_item(NV_CAL_ID, NV_CAL_BYTES);
    const uint8_t *cls = nv_item(NV_CLASS_ID, NV_CLASS_MIN);
    int rc;

    if (!rf || !calib || !cls)
        return DSP_EIMAGE;
    if ((rc = send_block(1, rf, NV_RF_BYTES)) != DSP_OK)
        return rc;
    dsp_info.param_blocks++;
    for (uint32_t i = 0; i < sizeof(cal) / sizeof(cal[0]); i++) {
        if (cal[i].wanted_by && !cls[cal[i].wanted_by])
            continue;
        rc = send_block(cal[i].type, calib + 2 + cal[i].offset, cal[i].bytes);
        if (rc != DSP_OK)
            return rc;
        dsp_info.param_blocks++;
    }
    return DSP_OK;
}

static int send_start(void)
{
    static const uint16_t start[5] = { 0, CMD_START, 0, 0, 0 };
    uint32_t irqs = dsp_info.irqs;
    int rc = post_cmd(start);

    return rc == DSP_OK ? wait_cmd_taken(irqs) : rc;
}

int l1_start(void)
{
    int rc;

    if (dsp_info.state != DSP_RUNNING)
        return DSP_ENOTRUN;
    dsp_info.records = dsp_info.cmds = dsp_info.frames_posted = 0;
    dsp_info.started = 0;
    dsp_info.param_blocks = 0;
    dsp_info.fi_moves = dsp_info.pm_results = 0;
    /* stock FUN_0002e98e: clear, interrupt on, clear, start marker */
    sm_clear(0, SHARE_CLEAR);
    fe0_init();
    irq_start();
    sm_clear(0, SHARE_CLEAR);
    *sm(SHARE_START) = START_MARK;      /* a plain store, as the stock's */
    frame_number = 0;
    dsp_info.fi_out = sm_r16(FI_OUT);
    rc = send_records();
    /* The stock's list (NOR 0xc800c) is 0, 0x10, 1, 3..15, 0x10: the records,
     * the start, the parameters, the start. The program sits in its wait at
     * 0x1907 until command 0 with word 1 = 0x10 sets 0x1063 and enables its
     * interrupt 9 (handler 0x9961); nothing else makes it go on. */
    if (rc == DSP_OK) {
        rc = send_start();
        dsp_info.started = rc == DSP_OK;
    }
    if (rc == DSP_OK)
        rc = send_parameters();
    if (rc == DSP_OK)
        rc = send_start();
    dsp_info.error = rc;
    if (rc == DSP_OK)
        dsp_info.stage = 7;
    klog("dsp: layer 1 %s, %lu records, start %lu, %lu parameter blocks, %lu irqs",
         rc == DSP_OK ? "up" : dsp_strerror(rc), (unsigned long)dsp_info.records,
         (unsigned long)dsp_info.started, (unsigned long)dsp_info.param_blocks,
         (unsigned long)dsp_info.irqs);
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

/* FUN_0005096e (the search before camping) for types 9 and 10: word 1 bits 7:4
 * = 1, word 4 = no serving cell (0xff), word 6 = index | setting << 10,
 * word 10 = cell << 10 and, for type 10, FUN_0006188a's value (1 with the
 * stock's 0x0422f036 = 2) in bits 9:8 over a low byte of 0xff. */
static void fi_cell(uint16_t fi[11], uint16_t index, uint8_t gain, int cell)
{
    fi[1] |= 0x10;
    fi[4] = 0x00ff;
    fi[6] = (uint16_t)((index & 0x3ffu) | (uint16_t)(gain & 0x3f) << 10);
    fi[10] = (uint16_t)((cell & 0x3f) << 10 | 0x3ff);
}

/* Type 9 covers window frames and carries the last one's number. */
int l1_fcch_post(uint16_t index, uint8_t gain, int window, int cell)
{
    uint16_t fi[11], ext[3];
    uint32_t saved = frame_number;
    int rc;

    if (window < 1 || window > 0xfff)
        return DSP_ECMD;
    frame_number = (frame_number + (uint32_t)window - 1) % FN_MOD;
    fi_init(fi, ext, FI_FCCH);
    fi_cell(fi, index, gain, cell);
    fi[3] = (uint16_t)window;
    if ((rc = post_fi(fi, ext)) != DSP_OK)
        frame_number = saved;
    return rc;
}

int l1_sch_post(uint16_t index, uint8_t gain, int cell)
{
    uint16_t fi[11], ext[3];

    fi_init(fi, ext, FI_SCH);
    fi_cell(fi, index, gain, cell);
    fi[10] = (uint16_t)((fi[10] & 0xfc00u) | 0x100u | 0xffu);
    return post_fi(fi, ext);
}

/* FUN_000504f8, one instruction per burst: word 1 = tag << 4 | burst; word 3
 * bits 10:8 = the timeslot, word 4 bits 11:8 = the TSC (the CBCH tables,
 * FUN_00050f6a, fill them from a channel description's TN and TSC [L]);
 * word 6 = the serving index over 0xfc00; words 7-9 no monitor channel;
 * word 10 = cell << 10 | 1 << 8 (FUN_0006188a) | the card bit (card 0). */
int l1_burst_post(uint16_t index, int tag, int burst, int tn, int tsc, int cell)
{
    uint16_t fi[11], ext[3];

    fi_init(fi, ext, FI_BLOCK);
    fi[1] |= (uint16_t)((tag & 0xf) << 4 | (burst & 3));
    fi[3] = (uint16_t)((tn & 7) << 8);
    fi[4] = (uint16_t)((tsc & 7) << 8);
    fi[6] = (uint16_t)(0xfc00u | (index & 0x3ffu));
    fi[10] = (uint16_t)((cell & 0x3f) << 10 | 0x100u | 1u);
    return post_fi(fi, ext);
}

uint32_t l1_next_fn(void)
{
    return frame_number;
}

int l1_fi_post(const uint16_t fi[11], const uint16_t ext[3])
{
    uint16_t f[11];

    for (int i = 0; i < 11; i++)
        f[i] = fi[i];
    f[1] = (uint16_t)((f[1] & 0x1ffu) | ((frame_number >> 16) & 0x7f) << 9);
    f[2] = (uint16_t)frame_number;
    return post_fi(f, ext);
}

uint16_t l1_share_r16(uint32_t off)
{
    return off < 0x1000u && !(off & 1) ? sm_r16(off) : 0;
}

void l1_share_w16(uint32_t off, uint16_t v)
{
    if (off < 0x1000u && !(off & 1))
        sm_w16(off, v);
}

int l1_cmd_post(const uint16_t cmd[5])
{
    return dsp_info.state == DSP_RUNNING ? post_cmd(cmd) : DSP_ENOTRUN;
}

const l1_api_t l1_api = {
    .dsp_boot = dsp_boot, .dsp_stop = dsp_stop, .start = l1_start, .run = l1_run,
    .poll = l1_poll, .fi_free = l1_fi_free, .idle_post = l1_idle_post,
    .ask_more = l1_ask_more, .arfcn_index = l1_arfcn_index, .pm_post = l1_pm_post,
    .pm_read = l1_pm_read, .fcch_post = l1_fcch_post, .sch_post = l1_sch_post,
    .burst_post = l1_burst_post, .next_fn = l1_next_fn, .rx_read = l1_rx_read,
    .fi_post = l1_fi_post, .share_r16 = l1_share_r16, .share_w16 = l1_share_w16,
    .cmd_post = l1_cmd_post, .info = &dsp_info, .probe = &dsp_probe_info,
};

/* FUN_0005f692 / FUN_0005f674: "in" is the slot written last; the bytes from
 * +0x12 come swapped pairwise, so each data halfword is high byte first. */
int l1_rx_read(l1_rx_t *rx)
{
    uint32_t in, out, base;

    if (dsp_info.state != DSP_RUNNING)
        return DSP_ENOTRUN;
    in = sm_r16(RX_IN);
    out = sm_r16(RX_OUT);
    if (in >= RX_SLOTS || out >= RX_SLOTS || in == out)
        return 0;
    out = (out + 1) % RX_SLOTS;
    base = RX_BASE + out * RX_SIZE;
    for (int i = 0; i < 9; i++)
        rx->hdr[i] = sm_r16(base + i * 2);
    for (int i = 0; i < (int)sizeof(rx->data) / 2; i++) {
        uint16_t v = sm_r16(base + 0x12 + i * 2);

        rx->data[i * 2] = (uint8_t)(v >> 8);
        rx->data[i * 2 + 1] = (uint8_t)v;
    }
    sm_w16(RX_OUT, (uint16_t)out);
    return 1;
}

/* FUN_0005f208(n): one instruction, n slots back from the last written, asks
 * the DSP for an interrupt when it gets there. */
void l1_ask_more(int back)
{
    uint32_t in = sm_r16(FI_IN);
    uint32_t slot;

    if (in >= FI_SLOTS || back < 0 || back >= (int)FI_SLOTS)
        return;
    slot = (in + FI_SLOTS - (uint32_t)back) % FI_SLOTS;
    sm_w16(FI_BASE + slot * FI_SIZE, sm_r16(FI_BASE + slot * FI_SIZE) | FI_ASK_MORE);
}

/* The stock's start of the frame clock (0x5a874, also 0x2ea1a): frame number 0,
 * four idle frames, the ask-more flag two back, then command 1 with 0, whose
 * handler (0x9a20) sets bit 2 of 0xff72. Frames go first: the DSP starts on
 * them at once. */
int l1_run(void)
{
    static const uint16_t go[5] = { 1, 0, 0, 0, 0 };
    uint32_t irqs = dsp_info.irqs;
    int rc;

    if (dsp_info.state != DSP_RUNNING)
        return DSP_ENOTRUN;
    frame_number = 0;
    if ((rc = l1_idle_post(4)) != DSP_OK)
        return rc;
    l1_ask_more(2);
    rc = post_cmd(go);
    return rc == DSP_OK ? wait_cmd_taken(irqs) : rc;
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

/* ---- our own small program --------------------------------------------------- */

volatile dsp_selftest_t dsp_selftest_info;

#define TINY_WORDS      (2 * BLOCK_HW)  /* two blocks: the path the real one takes */
#define TINY_START      0x20u
#define TINY_MARK0      0xfc0u          /* DSP 0xafe0: the first thing it does */
#define TINY_MARK1      0xfc2u          /* 0xafe1: after stock's first set-up */
#define TINY_COUNT      0xfc4u          /* 0xafe2: the running count */
#define TINY_IN         0xfc6u          /* 0xafe3: it reads this ... */
#define TINY_ECHO       0xfc8u          /* 0xafe4: ... and writes it here */

/* Hand-assembled TeakLite, nothing from the stock program. The first set-up
 * is the register writes stock's reset code starts with (0x2bc), in case the
 * memory needs them; a mark on each side tells. Each pass of the loop takes
 * about 266 DSP cycles, so the count gives the DSP's clock. */
static void tiny_build(uint16_t *w)
{
    static const uint16_t setup[][2] = {
        { 0xffff, 0xbd49 }, { 0x0000, 0xbd49 }, { 0x1387, 0xbd46 },
        { 0x0001, 0xbd47 }, { 0x0800, 0xbdf0 }, { 0x0007, 0xbdf1 },
    };
    uint32_t p = TINY_START, loop;

    memset(w, 0, TINY_WORDS * 2);       /* nop */
    w[0] = 0x4180;                      /* br start */
    w[1] = TINY_START;
#define STORE(v, addr) do { w[p++] = 0x5e18; w[p++] = (v);  /* mov v, a0 */ \
                            w[p++] = 0xd4bc; w[p++] = (addr); } while (0)
    STORE(0x5a01, 0xafe0);
    for (uint32_t i = 0; i < sizeof(setup) / sizeof(setup[0]); i++)
        STORE(setup[i][0], setup[i][1]);
    STORE(0x5a02, 0xafe1);
#undef STORE
    w[p++] = 0x6760;                    /* clr a0 */
    loop = p;
    w[p++] = 0x67d0;                    /* inc a0 */
    w[p++] = 0xd4bc;                    /* mov a0l, [0xafe2] */
    w[p++] = 0xafe2;
    w[p++] = 0xd5b8;                    /* mov [0xafe3], a1 */
    w[p++] = 0xafe3;
    w[p++] = 0xd5bc;                    /* mov a1l, [0xafe4] */
    w[p++] = 0xafe4;
    w[p++] = 0x0cff;                    /* rep 0xff: */
    w[p++] = 0x0000;                    /*   nop */
    w[p++] = 0x4180;                    /* br loop */
    w[p++] = (uint16_t)loop;
}

static void selftest_case(volatile dsp_selftest_case_t *r, bool arm_banks)
{
    uint16_t *w = kmalloc(TINY_WORDS * 2);
    uint32_t c0, c1, c2, t;

    if (!w) {
        r->result = DSP_ENOMEM;
        return;
    }
    tiny_build(w);
    banks_for_arm = arm_banks;
    r->result = download(w, TINY_WORDS);
    kfree(w);
    r->mcu_ctl0 = REG32(APB_MCU_CTL0);
    r->dsp_ctl0 = REG32(APB_DSP_CTL0);
    if (r->result == DSP_OK) {
        *sm(TINY_IN) = 0x1234;
        t = kernel_ticks();
        while (kernel_ticks() - t < 5)
            ;
        r->mark0 = sm_r16(TINY_MARK0);
        r->mark1 = sm_r16(TINY_MARK1);
        r->echo = sm_r16(TINY_ECHO);
        /* the count over 20 ms with the ARM busy, then with it idle */
        c0 = sm_r16(TINY_COUNT);
        t = kernel_ticks();
        while (kernel_ticks() - t < 20)
            ;
        c1 = sm_r16(TINY_COUNT);
        thread_sleep(20);
        c2 = sm_r16(TINY_COUNT);
        r->busy = (c1 - c0) & 0xffffu;
        r->idle = (c2 - c1) & 0xffffu;
    }
    REG32(APB_EB0_SET) = DSP_RESET;
    REG32(APB_MCU_CTL0) &= ~0x10000u;
    REG32(APB_MCU_CTL0) |= IRAM_BANKS;
    REG32(APB_INT_SET_CLR) = 4u;
    REG32(APB_INT_SET_CLR) = 8u;
    banks_for_arm = false;
    klog("dsp: own program, IRAM bits %s: %d, marks %04lx %04lx, echo %04lx, "
         "count %lu busy, %lu idle per 20 ms", arm_banks ? "set" : "clear", (int)r->result,
         (unsigned long)r->mark0, (unsigned long)r->mark1, (unsigned long)r->echo,
         (unsigned long)r->busy, (unsigned long)r->idle);
}

void dsp_selftest(void)
{
    if (dsp_info.state == DSP_BOOTING || dsp_info.state == DSP_RUNNING)
        return;
    memset((void *)&dsp_selftest_info, 0, sizeof(dsp_selftest_info));
    hold_cpu(true);
    selftest_case(&dsp_selftest_info.banks_set, true);
    selftest_case(&dsp_selftest_info.banks_clear, false);
    dsp_info.stage = 0;
    dsp_info.blocks = dsp_info.program_words = 0;
    dsp_info.ready_word = 0;
    hold_cpu(false);
}

/* ---- does it run? ----------------------------------------------------------- */

/* Compares a dumped block with the parameter records, which give each
 * word's DSP address: the words the DSP holds that the ARM sent it. */
static void compare_records(const uint16_t *block, uint32_t first)
{
    volatile dsp_probe_t *p = &dsp_probe_info;
    uint32_t r = 0;

    while (r < records_hw) {
        uint32_t n = records[r] & 0x7fffu;

        if (!n || r + 1 + n > records_hw)
            break;
        for (uint32_t i = 1; i < n; i++) {
            uint32_t addr = records[r + 1] + i - 1;

            if (addr >= first && addr < first + BLOCK_HW) {
                p->rec_words++;
                if (block[addr - first] == records[r + 1 + i])
                    p->rec_match++;
            }
        }
        r += 1 + n;
    }
}

static void save_file(const char *path, const void *data, uint32_t len, FIL *f, bool *open)
{
    UINT n;

    if (!*open && f_open(f, path, FA_CREATE_ALWAYS | FA_WRITE) == FR_OK)
        *open = true;
    if (*open && len)
        f_write(f, data, len, &n);
}

/* The stock's crash dump (FUN_0001a516, FUN_000185fc): ask with 3 and the
 * fast interrupt, the DSP answers 1; then for each of 64 blocks it copies
 * 1024 words of its memory to the start of share memory. */
static void ping_and_dump(void)
{
    volatile dsp_probe_t *p = &dsp_probe_info;
    uint16_t *block = kmalloc(BLOCK_HW * 2);
    FIL *f = kmalloc(sizeof(FIL));
    bool open = false;

    p->phase = DSP_P_PING;
    thread_sleep(250);                  /* the screen shows it, should this stall */
    p->ping_before = sm_r16(FE0_REPLY);
    *sm(FE0_REPLY) = 0;
    *sm(FE0_CMD) = 3;
    REG32(APB_INT_SET_CLR) |= 2u;       /* MCU_FRQ_SET, read-modify-write as stock */
    for (uint32_t t = 0; t < 200; t++) {
        thread_sleep(1);
        l1_service();
        p->ping_after = sm_r16(FE0_REPLY);
        if (p->ping_after == 1) {
            p->ping_ms = t + 1;
            break;
        }
    }
    if (p->ping_after != 1 || !block || !f)
        goto done;

    p->phase = DSP_P_DUMP;
    thread_sleep(250);
    for (uint32_t b = 0; b < 64; b++) {
        uint32_t first = b * BLOCK_HW, t;

        *sm(FE0_START) = (uint16_t)first;
        *sm(FE0_COUNT) = BLOCK_HW;
        *sm(FE0_REPLY) = 0;
        for (t = 0; t < 100 && sm_r16(FE0_REPLY) == 0; t++)
            thread_sleep(1);
        if (t == 100)
            break;
        for (uint32_t i = 0; i < BLOCK_HW; i++) {
            block[i] = sm_r16(i * 2);
            if (block[i])
                p->dump_nonzero++;
        }
        if (b == 0) {
            for (uint32_t i = 0; i < 4; i++)
                p->dump_head[i] = block[i];
            p->dump_is_program = crc32_msb((const uint8_t *)block, BLOCK_HW * 2,
                                           0xffffffffu) == block0_crc;
        }
        if (records)
            compare_records(block, first);
        save_file(OS_DIR "/dsp-dump.bin", block, BLOCK_HW * 2, f, &open);
        p->dump_blocks = b + 1;
    }
done:
    if (open)
        f_close(f);
    kfree(f);
    kfree(block);
}

/* Share memory as it is, for reading on a PC. */
static void save_share(void)
{
    uint16_t *copy = kmalloc(SHARE_CLEAR);
    FIL *f = kmalloc(sizeof(FIL));
    bool open = false;

    if (copy && f) {
        for (uint32_t i = 0; i < SHARE_CLEAR; i += 2)
            copy[i / 2] = sm_r16(i);
        save_file(OS_DIR "/dsp-share.bin", copy, SHARE_CLEAR, f, &open);
        if (open)
            f_close(f);
    }
    kfree(f);
    kfree(copy);
}

const char *dsp_probe_verdict(void)
{
    volatile dsp_probe_t *p = &dsp_probe_info;

    /* Only what the DSP itself must have produced. QEMU's model answers the
     * ping and nothing else, and the ping alone is not counted. */
    if (p->rec_words && p->rec_match * 2 > p->rec_words)
        return "runs: memory matches";
    if ((p->trace_start | p->trace_end) && !(p->replies || p->msgs || p->idle_consumed))
        return "runs, stops early";
    if (p->replies || p->msgs || p->records_taken)
        return "runs: it answers";
    if (p->ping_after != 1)
        return "never started";
    if (p->ping_after == 1)
        return "ping only";
    return "no answer";
}

static void save_report(void)
{
    volatile dsp_probe_t *p = &dsp_probe_info;
    char *t = kmalloc(2048);
    FIL *f = kmalloc(sizeof(FIL));
    bool open = false;
    int n = 0;

    if (t && f) {
#define OUT(...) n += snprintf(t + n, 2048 - (size_t)n, __VA_ARGS__)
        OUT("verdict %s\nphase %lu\nboot: %lu blocks, %lu ms, ready %04lx\n",
            dsp_probe_verdict(), (unsigned long)p->phase, (unsigned long)dsp_info.blocks,
            (unsigned long)dsp_info.boot_ms, (unsigned long)dsp_info.ready_word);
        for (int i = 0; i < 2; i++) {
            volatile dsp_selftest_case_t *r = i ? &dsp_selftest_info.banks_clear :
                                                  &dsp_selftest_info.banks_set;

            OUT("own program, %s: %d, marks %04lx %04lx, echo %04lx, count %lu busy "
                "%lu idle, 1a0 %08lx 1c0 %08lx\n",
                i ? "IRAM bits clear" : "IRAM bits set",
                (int)r->result,
                (unsigned long)r->mark0, (unsigned long)r->mark1, (unsigned long)r->echo,
                (unsigned long)r->busy, (unsigned long)r->idle, (unsigned long)r->mcu_ctl0,
                (unsigned long)r->dsp_ctl0);
        }
        OUT("stock with marks: trace %04lx at start, %04lx after records, %04lx at end\n",
            (unsigned long)dsp_selftest_info.trace_start,
            (unsigned long)dsp_selftest_info.trace_records,
            (unsigned long)dsp_selftest_info.trace_end);
        OUT("stock unpatched below%s\n", trace_on ? " (marks on)" : "");
        OUT("at start: %lu non-zero share words, first %08lx\n",
            (unsigned long)p->early_words, (unsigned long)p->early_first);
        OUT("records %lu of %lu, irqs %lu, storms %lu, status bits %08lx\n",
            (unsigned long)p->records_taken, (unsigned long)p->records_total,
            (unsigned long)dsp_info.irqs, (unsigned long)dsp_info.irq_storms,
            (unsigned long)p->int_sts);
        OUT("start command %s\ncommands: in %lu out %lu\nreplies %lu, other than acks %lu\n",
            dsp_info.started ? "taken" : "not taken", (unsigned long)p->cmd_in,
            (unsigned long)p->cmd_out, (unsigned long)p->replies, (unsigned long)p->reply_kept);
        for (uint32_t i = 0; i < p->reply_kept; i++)
            OUT(" %04lx %04lx %04lx %04lx %04lx\n", (unsigned long)p->reply[i][0],
                (unsigned long)p->reply[i][1], (unsigned long)p->reply[i][2],
                (unsigned long)p->reply[i][3], (unsigned long)p->reply[i][4]);
        OUT("messages %lu, first %08lx in phase %lu, last %08lx (ffff....: DSP assert)\n",
            (unsigned long)p->msgs, (unsigned long)p->msg_first, (unsigned long)p->msg_phase,
            (unsigned long)p->msg_last);
        OUT("parameter blocks %lu\n", (unsigned long)dsp_info.param_blocks);
        OUT("run command %s; idle frames: %lu posted, %lu taken in 400 ms, %lu us each "
            "(GSM: 4615)\n", p->wake_taken ? "taken" : "not taken",
            (unsigned long)p->idle_posted, (unsigned long)p->idle_consumed,
            (unsigned long)p->frame_us);
        OUT("last ack: %04lx %04lx %04lx %04lx %04lx\n", (unsigned long)p->last_ack[0],
            (unsigned long)p->last_ack[1], (unsigned long)p->last_ack[2],
            (unsigned long)p->last_ack[3], (unsigned long)p->last_ack[4]);
        OUT("ping: before %04lx, after %04lx, %lu ms\n", (unsigned long)p->ping_before,
            (unsigned long)p->ping_after, (unsigned long)p->ping_ms);
        OUT("dump: %lu blocks, %lu non-zero words, head %04lx %04lx %04lx %04lx, "
            "program %lu, record words %lu of %lu\n", (unsigned long)p->dump_blocks,
            (unsigned long)p->dump_nonzero, (unsigned long)p->dump_head[0],
            (unsigned long)p->dump_head[1], (unsigned long)p->dump_head[2],
            (unsigned long)p->dump_head[3], (unsigned long)p->dump_is_program,
            (unsigned long)p->rec_match, (unsigned long)p->rec_words);
#undef OUT
        save_file(OS_DIR "/dsp-report.txt", t, (uint32_t)n, f, &open);
        if (open)
            f_close(f);
    }
    kfree(f);
    kfree(t);
}

/* The frame ring's "out", watched over several steps. The ring is kept topped
 * up with idle frames, so the time per frame is a sustained rate: the DSP's
 * frame clock if the frames are taken one by one, near nothing if it only
 * flushes them. */
static struct {
    uint32_t out, moves, first, last;
} watch;

static void watch_start(void)
{
    watch.out = sm_r16(FI_OUT);         /* before anything is posted */
    watch.moves = watch.first = watch.last = 0;
}

/* Frames taken in these ms. */
static uint32_t watch_frames(uint32_t ms)
{
    uint32_t before = watch.moves;

    for (uint32_t t = 0; t < ms; t++) {
        uint32_t now;
        int free_slots;

        thread_sleep(1);
        l1_service();
        now = sm_r16(FI_OUT);
        if (now < FI_SLOTS && now != watch.out) {
            if (!watch.moves)
                watch.first = kernel_ticks();
            watch.last = kernel_ticks();
            watch.moves += (now + FI_SLOTS - watch.out) % FI_SLOTS;
            watch.out = now;
        }
        free_slots = l1_fi_free();
        if (free_slots > 8 && l1_idle_post(free_slots - 4) == DSP_OK) {
            dsp_probe_info.idle_posted += (uint32_t)(free_slots - 4);
            l1_ask_more(3);
        }
    }
    if (watch.moves > 1)
        dsp_probe_info.frame_us = (watch.last - watch.first) * 1000 / (watch.moves - 1);
    return watch.moves - before;
}

int dsp_probe(void)
{
    volatile dsp_probe_t *p = &dsp_probe_info;
    int rc;

    memset((void *)p, 0, sizeof(*p));
    if (dsp_info.state != DSP_RUNNING)
        return DSP_ENOTRUN;

    /* What it wrote on its own: everything above the download area was
     * cleared before the download. */
    p->phase = DSP_P_START;
    thread_sleep(100);
    p->trace_start = trace_read();
    for (uint32_t i = 0x808; i < SHARE_CLEAR; i += 2) {
        uint16_t v = sm_r16(i);

        if (v) {
            if (!p->early_words)
                p->early_first = i << 16 | v;
            p->early_words++;
        }
    }

    p->phase = DSP_P_RECORDS;
    for (uint32_t r = 0; r < records_hw; ) {
        uint32_t n = records[r] & 0x7fffu;

        if (!n || r + 1 + n > records_hw)
            break;
        p->records_total++;
        r += 1 + n;
    }
    rc = l1_start();
    p->records_taken = dsp_info.records;
    p->cmd_in = sm_r16(CMD_IN);
    p->cmd_out = sm_r16(CMD_OUT);

    /* Idle frames only, so no radio use: the "out" index shows whether the
     * DSP's frame clock runs, at one frame per 4.615 ms if it does. */
    if (rc == DSP_OK && p->records_taken) {
        p->phase = DSP_P_FRAMES;
        thread_sleep(250);
        watch_start();
        if (l1_run() == DSP_OK) {
            p->wake_taken = 1;
            p->idle_posted = 4;
            p->idle_consumed = watch_frames(400);
        }
    }

    p->trace_records = trace_read();
    save_share();
    ping_and_dump();
    l1_service();
    p->trace_end = trace_read();
    p->phase = DSP_P_DONE;
    save_report();
    klog("dsp: trace %04lx at start, %04lx after records, %04lx at end",
         (unsigned long)p->trace_start, (unsigned long)p->trace_records,
         (unsigned long)p->trace_end);
    klog("dsp: probe: %s; records %lu/%lu, irqs %lu, replies %lu, messages %lu, "
         "frames %lu at %lu us, ping %04lx, dump %lu blocks, record words %lu/%lu",
         dsp_probe_verdict(), (unsigned long)p->records_taken,
         (unsigned long)p->records_total, (unsigned long)dsp_info.irqs,
         (unsigned long)p->replies, (unsigned long)p->msgs, (unsigned long)p->idle_consumed,
         (unsigned long)p->frame_us, (unsigned long)p->ping_after,
         (unsigned long)p->dump_blocks, (unsigned long)p->rec_match,
         (unsigned long)p->rec_words);
    return DSP_OK;
}
