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
#define FI_SLEEP        1u
#define FI_POWER        11u
#define FI_FCCH         9u
#define FI_SCH          10u
#define FI_BLOCK        2u
#define FI_ACCESS       6u              /* access / RACH */
#define FI_UPLINK       7u              /* rx + uplink normal burst (dedicated chan) */
#define TXBUF_BASE      0x240u          /* TXBUFQ: 4 x 62 bytes */
#define TXBUF_SLOTS     4u
#define TXBUF_SIZE      0x3eu
#define TXBUF_IN        0x338u
#define TXBUF_OUT       0x33au          /* the DSP's consumer index */
/* The transmit power control level (PCL), double-buffered: FUN_0005f810(pcl, t)
 * writes it at +0x3ec + 2 t, and the next TX frame's word 5 says so with bit 9
 * and t in bits 11:10 (FUN_00062cce, from FUN_0004fe0a). */
#define TXPWR_PCL       0x3ecu
#define TXTA_SHARE      0x3e4u          /* dedicated TX timing advance (0..63), same
                                         * word-5 block as the power level (+0x3ec) */
#define CIPHER_KC_SHARE 0x3f8u          /* A5 key: 8 words (one Kc byte each) the DSP
                                         * copies to key reg 0xec71 when FI word-5 bit
                                         * 15 is set; just past the cipher COUNT regs at
                                         * +0x3f0. See docs/a5-cipher-re.md. */
#define CIPHER_CNT_SHARE 0x3f0u         /* A5 COUNT (GSM frame number): the DSP copies
                                         * it into 0xec4c/4d/4e when FI word-1 bit 8 is
                                         * 0, then advances per frame when bit 8 is 1.
                                         * Packing (engine 0x1645): FN = 0xec4c<<16 |
                                         * 0xec4e<<8 | 0xec4d. */
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
#define NV_IMEI_ID      5u

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

/* The phone's own IMEI, from its fixed NV: item 5, 8 bytes, the nibble 0xA and
 * then 15 digits, low nibble first. Read only. Nothing is logged: the caller
 * shows it. False unless all of it is digits and the check digit is right. */
bool nv_imei(char out[16])
{
    const uint8_t *b = nv_item(NV_IMEI_ID, 8);
    uint32_t sum = 0;

    out[0] = 0;
    if (!b || (b[0] & 0xf) != 0xa)
        return false;
    for (int i = 0; i < 15; i++) {
        uint32_t d = i & 1 ? b[(i + 1) / 2] & 0xf : b[i / 2] >> 4;

        if (d > 9)
            return false;
        out[i] = (char)('0' + d);
        /* Luhn: every second digit from the right, the check digit not doubled */
        if (i & 1)
            d = d * 2 > 9 ? d * 2 - 9 : d * 2;
        sum += d;
    }
    if (sum % 10) {
        out[0] = 0;
        return false;
    }
    out[15] = 0;
    return true;
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

/* ---- transmit minimum-power clamp (TX test, docs/gsm-plan.md "TX power") -----
 * The DSP's TX power is the per-band APC setpoint table in NV item 2 (cal types
 * 4..7, u16 index 16..33: a DAC per power-control-level, ~226-231 at the top
 * down to 10 at the minimum). We send those tables to the DSP, so to bound the
 * PA to the calibrated minimum we overwrite, in our copy only, every setpoint to
 * the minimum (10) and zero the compensation deltas (index 34..45) before
 * sending. RX AGC (index 64+) and the ramp thresholds are untouched, and nothing
 * is written back to NOR. The burst path (l1_access_post) refuses unless this
 * clamp is on, so a transmit cannot happen without it. */
#define APC_BANDS       4
#define APC_FIRST       16              /* first setpoint (u16 index in the block) */
#define APC_LAST        33
#define APC_N           (APC_LAST - APC_FIRST + 1)      /* 18 setpoints */
#define APC_COMP_FIRST  34              /* temperature / voltage compensation */
#define APC_COMP_LAST   45
#define APC_MIN         10              /* the owner's calibrated minimum DAC */

/* TX power interlock (set before l1_start()): L1_TX_OFF refuses all transmit -
 * the safe default; L1_TX_MIN sends the per-band APC tables clamped to the
 * calibrated minimum (the SDR-verified dry run); L1_TX_NORMAL sends them as
 * calibrated, for a real (owner-cleared) registration. */
static int tx_power_mode;               /* L1_TX_OFF by default; gates TX */
static uint16_t sent_apc[APC_BANDS][APC_N];     /* setpoints actually sent */

void l1_tx_min_power(int on) { tx_power_mode = on ? L1_TX_MIN : L1_TX_OFF; }
int l1_tx_min_power_on(void) { return tx_power_mode == L1_TX_MIN; }
void l1_tx_power_set(int mode)
{
    tx_power_mode = mode < L1_TX_OFF ? L1_TX_OFF :
                    mode > L1_TX_MAX ? L1_TX_MAX : mode;
}
int l1_tx_power_mode(void) { return tx_power_mode; }

/* out[band][k] = the setpoint that was last sent for that band; the return is
 * the number of bands with the min clamp active (0 = sent as-calibrated). */
int l1_cal_readback(uint16_t out[APC_BANDS][APC_N])
{
    for (int b = 0; b < APC_BANDS; b++)
        for (int k = 0; k < APC_N; k++)
            out[b][k] = sent_apc[b][k];
    return tx_power_mode == L1_TX_MIN ? APC_BANDS : 0;
}

/* The NV per-band TX-enable bytes (item 3 "class": byte 42 GSM900, 44 DCS1800,
 * 45 PCS1900, 46 GSM850 - what send_parameters checks before uploading each
 * band's cal). out[0..3] in that band order; 0, or -1 if the item is missing. */
int l1_tx_band_caps(uint8_t out[4])
{
    const uint8_t *cls = nv_item(NV_CLASS_ID, NV_CLASS_MIN);
    static const int off[4] = { 42, 44, 45, 46 };

    if (!cls)
        return -1;
    for (int i = 0; i < 4; i++)
        out[i] = cls[off[i]];
    return 0;
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
        const uint8_t *buf = calib + 2 + cal[i].offset;
        uint16_t clamped[396];          /* a band block is 792 bytes */

        if (cal[i].wanted_by && !cls[cal[i].wanted_by])
            continue;
        /* cal types 4..7 carry the per-band TX power setpoints: record what we
         * send and, when the clamp is armed, send the minimum instead. */
        if (cal[i].type >= 4 && cal[i].type <= 7) {
            int band = cal[i].type - 4;
            const uint16_t *src = (const uint16_t *)buf;

            if (tx_power_mode == L1_TX_MIN || tx_power_mode == L1_TX_MAX) {
                uint16_t v = APC_MIN;           /* MIN: the calibrated minimum */

                if (tx_power_mode == L1_TX_MAX)
                    for (int k = APC_FIRST; k <= APC_LAST; k++)
                        if (src[k] > v)         /* MAX: the calibrated maximum setpoint */
                            v = src[k];
                for (int k = 0; k < 396; k++)
                    clamped[k] = src[k];
                for (int k = APC_FIRST; k <= APC_LAST; k++)
                    clamped[k] = v;             /* all power-control levels -> v */
                if (tx_power_mode == L1_TX_MIN)
                    for (int k = APC_COMP_FIRST; k <= APC_COMP_LAST; k++)
                        clamped[k] = 0;         /* zero the comp deltas at the floor */
                buf = (const uint8_t *)clamped;
                for (int k = 0; k < APC_N; k++)
                    sent_apc[band][k] = v;
            } else {
                for (int k = 0; k < APC_N; k++)
                    sent_apc[band][k] = src[APC_FIRST + k];
            }
        }
        rc = send_block(cal[i].type, buf, cal[i].bytes);
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

/* Per DSP cell number: the gain setting its type-2 reads use (l1_burst_post). */
#define CELL_SLOTS 64
static uint8_t cell_gain[CELL_SLOTS];   /* 0x80 | setting; 0 = unset */
static uint8_t txpwr_toggle;            /* the power-level buffer in use (0/1) */
static bool txpwr_pending;              /* flag it in the next TX frame's word 5 */
/* The FI word-5 cipher bits ORed into every RX and TX frame once ciphering is on:
 * bit 15 (re)loads Kc from share +0x3f8 into key reg 0xec71, bits 14:13 select the
 * algorithm (1 = A5/1). 0 = ciphering off. Set by l1_cipher_set; see
 * docs/a5-cipher-re.md. */
static uint16_t cipher_w5;
static bool cipher_count_load;          /* next FI loads the A5 COUNT from share +0x3f0
                                         * (word-1 bit 8 = 0); after that the FIs set bit
                                         * 8 = 1 so the DSP advances the count itself */
static uint32_t cipher_fn_offset;       /* cell FN = (frame_number + this) % FN_MOD - the
                                         * cell's absolute frame number the A5 COUNT needs
                                         * (frame_number is our local count, 0 at start) */

int l1_start(void)
{
    int rc;

    if (dsp_info.state != DSP_RUNNING)
        return DSP_ENOTRUN;
    dsp_info.records = dsp_info.cmds = dsp_info.frames_posted = 0;
    dsp_info.started = 0;
    dsp_info.param_blocks = 0;
    dsp_info.fi_moves = dsp_info.pm_results = 0;
    cipher_w5 = 0;                       /* ciphering off until a CIPHER MODE COMMAND */
    cipher_count_load = false;
    /* stock FUN_0002e98e: clear, interrupt on, clear, start marker */
    sm_clear(0, SHARE_CLEAR);
    fe0_init();
    irq_start();
    sm_clear(0, SHARE_CLEAR);
    *sm(SHARE_START) = START_MARK;      /* a plain store, as the stock's */
    frame_number = 0;
    memset(cell_gain, 0, sizeof cell_gain);     /* no cell synced in this session */
    txpwr_toggle = 0;                   /* the share is cleared: no level set */
    txpwr_pending = false;
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

/* [C] the index->band order read from the stock DSP (FUN at 0x5287), sizes adding
 * to RF_TOTAL_CHANS (971): 0-124 P-GSM, 125-498 DCS1800, 499-547 E-GSM, 548-671
 * GSM850, 672-970 PCS1900. (The earlier [G] order put DCS at 174-547 and was right
 * only for P-GSM 0-124 - see docs/gsm-plan.md "The arfcn index order".) */
uint16_t l1_arfcn_index(int band, int arfcn)
{
    switch (band) {
    case L1_BAND_GSM900:
        if (arfcn >= 0 && arfcn <= 124)
            return (uint16_t)arfcn;                     /* P-GSM: 0..124 */
        if (arfcn >= 975 && arfcn <= 1023)
            return (uint16_t)(499 + arfcn - 975);       /* E-GSM: 499..547 */
        break;
    case L1_BAND_DCS1800:
        if (arfcn >= 512 && arfcn <= 885)
            return (uint16_t)(125 + arfcn - 512);       /* 125..498 */
        break;
    case L1_BAND_GSM850:
        if (arfcn >= 128 && arfcn <= 251)
            return (uint16_t)(548 + arfcn - 128);       /* 548..671 */
        break;
    case L1_BAND_PCS1900:
        if (arfcn >= 512 && arfcn <= 810)
            return (uint16_t)(672 + arfcn - 512);       /* 672..970 */
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
    /* A5 COUNT. The DSP copies share +0x3f0 into its cipher frame-number register
     * when word-1 bit 8 is 0, and advances it per frame when bit 8 is 1. So, once
     * ciphering is on, seed the register with the cell's absolute FN on the first
     * frame (bit 8 = 0, share written) and from then on let the DSP run it (bit 8 =
     * 1) - otherwise it reloads an unset +0x3f0 every frame and the keystream frame
     * number never matches the network's. Not touched before ciphering. */
    if (cipher_w5) {
        if (cipher_count_load) {
            uint32_t cfn = (frame_number + cipher_fn_offset) % FN_MOD;

            sm_w16(CIPHER_CNT_SHARE,      (uint16_t)((cfn >> 16) & 0xffu));
            sm_w16(CIPHER_CNT_SHARE + 2u, (uint16_t)(cfn & 0xffu));
            sm_w16(CIPHER_CNT_SHARE + 4u, (uint16_t)((cfn >> 8) & 0xffu));
            sm_w16(CIPHER_CNT_SHARE + 6u, 0);
            cipher_count_load = false;  /* leave bit 8 = 0 this frame: the DSP loads it */
        } else {
            fi[1] |= 0x100u;            /* bit 8 = 1: keep the DSP's advancing count */
        }
    }
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
 * word 10 = cell << 10 | 1 << 8 (FUN_0006188a) | the card bit (card 0).
 * Word 6's gain: the stock has 0x3f and takes the gain from command 3, its
 * per-channel AGC setting. No cell is tracked here, so word 6 carries that
 * setting itself: the cell's, from l1_cell_gain_set() (rxlev / 2), else the
 * stock's starting 25. A fixed 25 lost ~3 of 4 CCCH blocks on a rxlev-33 cell
 * the stock reads at full bars (2026-11-09). */
void l1_cell_gain_set(int cell, uint8_t gain)
{
    if (cell >= 0 && cell < CELL_SLOTS)
        cell_gain[cell] = (uint8_t)(0x80u | (gain & 0x3fu));
}

int l1_burst_post(uint16_t index, int tag, int burst, int tn, int tsc, int cell)
{
    uint16_t fi[11], ext[3];
    uint16_t g = cell >= 0 && cell < CELL_SLOTS && (cell_gain[cell] & 0x80u) ?
                 (uint16_t)(cell_gain[cell] & 0x3fu) : 25u;

    fi_init(fi, ext, FI_BLOCK);
    fi[1] |= (uint16_t)((tag & 0xf) << 4 | (burst & 3));
    fi[3] = (uint16_t)((tn & 7) << 8);
    fi[4] = (uint16_t)((tsc & 7) << 8);
    fi[6] = (uint16_t)(g << 10 | (index & 0x3ffu));
    fi[5] = cipher_w5;                   /* decipher the downlink once ciphering is on */
    fi[10] = (uint16_t)((cell & 0x3f) << 10 | 0x100u | 1u);
    return post_fi(fi, ext);
}

uint32_t l1_next_fn(void)
{
    return frame_number;
}

/* Type 1, the multi-frame sleep (FUN_0004fc5c): nothing received for that many
 * frames. Word 3 the count; its frame number is the span's last, as for type 9. */
int l1_sleep_post(int frames)
{
    uint16_t fi[11], ext[3];
    uint32_t saved = frame_number;
    int rc;

    if (frames < 1 || frames >= 1000)
        return DSP_ECMD;
    frame_number = (frame_number + (uint32_t)frames - 1) % FN_MOD;
    fi_init(fi, ext, FI_SLEEP);
    fi[3] = (uint16_t)frames;
    if ((rc = post_fi(fi, ext)) != DSP_OK)
        frame_number = saved;
    return rc;
}

/* FUN_0005f332: the next TXBUFQ slot takes the tag, the length-type and then the
 * payload as halfwords, each high byte first. */
static int tx_desc_post(uint16_t tag, uint16_t type, const uint8_t *buf, uint32_t nbytes)
{
    uint32_t in = sm_r16(TXBUF_IN), out = sm_r16(TXBUF_OUT), next, base;

    if (in >= TXBUF_SLOTS)
        return DSP_EFULL;
    next = (in + 1) % TXBUF_SLOTS;
    if (out < TXBUF_SLOTS && next == out)       /* ring full: the DSP has not drained it */
        return DSP_EFULL;
    base = TXBUF_BASE + next * TXBUF_SIZE;
    sm_w16(base, tag);
    sm_w16(base + 2, type);
    for (uint32_t k = 0; k < nbytes / 2; k++)
        sm_w16(base + 8 + k * 2, (uint16_t)(buf[k * 2] << 8 | buf[k * 2 + 1]));
    if (nbytes & 1)
        sm_w16(base + 8 + (nbytes / 2) * 2, (uint16_t)(buf[nbytes - 1] << 8));
    sm_w16(TXBUF_IN, (uint16_t)next);
    return DSP_OK;
}

/* One GSM access burst (RACH) - the osmocom RACH_REQ primitive. The stock
 * FUN_0004fe0a reduced to a standalone key-up: word 7 = the channel index
 * (l1_arfcn_index) under the stock's 0x3f in bits 15:10 (its skeleton's 0xffff
 * kept; we had 0 there), word 10 = the cell slot, no hopping, band field 0. The
 * TX descriptor is the stock's {0, bsic, 0, ra}: byte 1 is the cell table's +32
 * byte (FUN_00048b90), which FUN_0004ce38 compares with SCH byte 0 >> 2 - the
 * BSIC, which the burst's parity is coded with (TS 45.003 4.6). It was read as an
 * RF-cal byte and sent as 0, so a BTS with any other BSIC dropped every burst.
 * Byte 3 is the 8-bit channel request. The arm command {4,0,1} (command group 4)
 * follows and the DSP keys the PA for that one frame. Refused in L1_TX_OFF.
 * SDR-verified 2026-11-09 to emit spec-compliant access bursts. */
uint8_t l1_access_w7hi = 0x3f;          /* access frame word 7 bits 15:10 (test knob) */

/* The stock's TX power control (FUN_00062cce / FUN_00062ca4 / FUN_0005f810): the
 * level goes to share +0x3ec + 2 t, t toggling per change, and the next TX frame
 * carries word 5 = bit 9 | t << 10 so the DSP takes it. Without it the DSP has no
 * level: with word 7's 0x3f (the stock's) it sent nothing, with other values a
 * weak, misshapen output ~50 dB under the stock's (SDR, 2026-11-10). */
void l1_tx_level_set(uint8_t pcl)
{
    txpwr_toggle ^= 1u;
    sm_w16(TXPWR_PCL + 2u * txpwr_toggle, (uint16_t)(pcl & 0x1f));
    txpwr_pending = true;
}

/* The uplink timing advance (TA, 0..63 bit-periods) the grant gives, so the SABM
 * and later uplink bursts arrive aligned at the BTS. The DSP reads it from share
 * +0x3e4 in dedicated mode (FUN_00062d92 clamps to 63; same double-buffered block
 * as the power level). Both slots get the value so the DSP reads it whichever it
 * picks, and the power-level word-5 flag signals the read. Without it a normal
 * burst arrives TA bit-periods late - outside the BTS window - and is dropped. */
void l1_tx_ta_set(uint8_t ta)
{
    uint16_t v = ta > 63u ? 63u : ta;

    sm_w16(TXTA_SHARE, v);
    sm_w16(TXTA_SHARE + 2u, v);
    txpwr_pending = true;
}

/* Word 5 for a TX frame: the power-update flag once after l1_tx_level_set. */
static uint16_t txpwr_w5(void)
{
    if (!txpwr_pending)
        return 0;
    txpwr_pending = false;
    return (uint16_t)(0x200u | (uint16_t)txpwr_toggle << 10);
}

int l1_access_post(uint16_t index, int slot, uint8_t bsic, uint8_t ra)
{
    uint8_t desc[4] = { 0, (uint8_t)(bsic & 0x3f), 0, ra };
    uint16_t fi[11], ext[3];
    int rc;

    if (dsp_info.state != DSP_RUNNING)
        return DSP_ENOTRUN;
    if (tx_power_mode == L1_TX_OFF)      /* interlock: no transmit in the OFF mode */
        return DSP_ECMD;
    fi_init(fi, ext, FI_ACCESS);
    fi[0] = (uint16_t)(FI_ACCESS | 0xa000u);    /* the stock's type-6 flag bits */
    fi[3] = 0;                           /* no hopping, power band 0 */
    fi[5] = txpwr_w5();                  /* a new power level, if one was set */
    fi[7] = (uint16_t)((uint16_t)(l1_access_w7hi & 0x3f) << 10 | (index & 0x3ffu));
    fi[10] = (uint16_t)(0x3ffu | (uint16_t)(slot & 0x3f) << 10);
    if ((rc = post_fi(fi, ext)) != DSP_OK)
        return rc;
    if ((rc = tx_desc_post(0, 0, desc, sizeof desc)) != DSP_OK)
        return rc;
    /* FUN_0004b49a(0,1): command group 4 (the TX/frame group), word 2 = 1. */
    { const uint16_t cmd[5] = { 4, 0, 1, 0, 0 }; return post_cmd(cmd); }
}

/* Uplink normal-burst TX on a dedicated channel - the osmocom DATA_REQ (uplink)
 * primitive. The stock path (FUN_000504f8 + FUN_00050426 + FUN_0004f526):
 *  - the 23-byte L2 block goes to the TXBUFQ as a type-3 descriptor;
 *  - the burst is a type-2 rx frame rewritten to type 7: word0 = 7 | 0xa000,
 *    words 6 and 7 = the ARFCN index, word 10 = the cal slot, word 3's high
 *    nibble cleared; word1 carries the tag/burst/FN and word4 the TSC as for rx;
 *  - then a {0, bsic, 0, burst} control descriptor (byte 1 the cell table's +32
 *    byte, the BSIC, as for the access burst) and the arm command {4,0,1}.
 * A caller posts the four bursts (burst 0..3) of a block in order on the
 * assigned timeslot. Serving-cell context (ARFCN index, cell slot, TN, TSC, cell,
 * BSIC) comes from the immediate assignment and the SCH; pass it in rather than
 * reading a stock scell struct.
 *
 * RE-FAITHFUL + compile-checked, but NOT HARDWARE-VERIFIED: a dedicated channel
 * only exists after a network immediate-assignment, so this can be exercised only
 * through the (gated) registration flow, where its exact block/burst framing is
 * confirmed against real behaviour. The min-power clamp interlock still applies.
 * See docs/l1-osmocom.md. */
/* Usable free slots in the TXBUFQ (one slot is always kept empty, as tx_desc_post
 * uses next==out for full). Before the DSP sets TXBUF_OUT the full-check is skipped,
 * so report the ring empty. Used to pace the uplink so a burst FI is never posted
 * without its TXBUFQ entry - a type-7 FI with no entry stalls the DSP. */
int txbuf_free(void)
{
    uint32_t in = sm_r16(TXBUF_IN), out = sm_r16(TXBUF_OUT);

    if (out >= TXBUF_SLOTS)
        return (int)(TXBUF_SLOTS - 1u);
    return (int)((out + TXBUF_SLOTS - 1u - in) % TXBUF_SLOTS);
}

/* TXBUFQ block-type for the dedicated uplink. The on-phone stock DSP logger
 * (2026-11-12) shows stock writes the 23-byte L2 block with TXBUFQ type = 2 for the
 * SDCCH (main DCCH signalling) and type = 1 for the SACCH, and carries that same
 * type in the PA-key command (0x0004) word 1 - NOT the type 3 / word-1 0 we used.
 * (The RACH is type 0 in both, which already matched.) dm_sched sets this to 1
 * before posting a SACCH block and back to 2 for the SDCCH. */
uint16_t l1_ul_block_type = 2;          /* 2 = SDCCH, 1 = SACCH (stock TXBUFQ type) */
/* Dedicated uplink FI format. Ghidra RE (2026-11-12) showed our old path (FI type
 * 7|0xa000=0xa007, a TXBUFQ control word + PA-key per burst) is stock's CCCH uplink
 * primitive (FUN_00050426 asserts AGCH/BCCH/PCH), wrong for the dedicated SDCCH. Stock's
 * dedicated TX uses FI type 0x0003/0x0005 and posts ONE block + ONE PA-key per block
 * (no per-burst control words), the DSP building the 4 bursts. l1_ul_fi_type sets the FI
 * f0 (3 = stock dedicated, 7 = old 0xa007); l1_ul_perblock=1 collapses to one block+PA-key
 * per block and drops the per-burst control words. Knobs so a broken uplink can be bisected. */
uint16_t l1_ul_fi_type  = 7;            /* uplink FI f0: 3 (stock) or 7 (old 0xa007). 7 =
                                         * known-establishing uplink, to ISOLATE the downlink
                                         * RX-tag fix on the first test; flip to 3 after. */
uint8_t  l1_ul_perblock = 0;            /* 0 = old per-burst (isolate downlink fix first) */

int l1_uplink_post(uint16_t index, int tag, int burst, int tn, int tsc, int cell,
                   uint8_t bsic, const uint8_t block[23])
{
    uint16_t fi[11], ext[3];
    int rc;

    if (dsp_info.state != DSP_RUNNING)
        return DSP_ENOTRUN;
    if (tx_power_mode == L1_TX_OFF)      /* interlock: no transmit in the OFF mode */
        return DSP_ECMD;
    /* Each uplink burst FI (type 7) makes the DSP read one TXBUFQ entry; with no
     * entry it stalls the frame clock. So per block the stock posts the 23-byte L2
     * block once (burst 0) plus a 4-byte control word per burst - block + 4 words
     * = 5 entries - and paces one burst a frame so its 4-slot ring never holds
     * more than two. We pace the same way (l1_dm_sched keeps the ring shallow and
     * checks txbuf_free before each burst). Post nothing unless the whole burst
     * fits - block+word on burst 0, word otherwise, plus one FI slot - so an FI
     * is never left without its TXBUFQ entry. */
    uint32_t need = l1_ul_perblock ? (burst == 0 ? 1u : 0u) : (burst == 0 ? 2u : 1u);
    if ((uint32_t)txbuf_free() < need || l1_fi_free() < 1)
        return DSP_EFULL;
    if (burst == 0 &&
        (rc = tx_desc_post((uint16_t)(tag & 0xf), l1_ul_block_type, block, 23)) != DSP_OK)
        return rc;
    fi_init(fi, ext, FI_BLOCK);
    fi[1] |= (uint16_t)((tag & 0xf) << 4 | (burst & 3));
    fi[3] = (uint16_t)((tn & 7) << 8);
    fi[4] = (uint16_t)((tsc & 7) << 8);
    fi[10] = (uint16_t)((cell & 0x3f) << 10 | 0x100u | 1u);
    fi[0] = l1_ul_fi_type == 7 ? (uint16_t)(FI_UPLINK | 0xa000u)   /* old CCCH 0xa007 */
                               : (uint16_t)(l1_ul_fi_type & 0x1f); /* stock dedicated: 3 */
    fi[5] = txpwr_w5() | cipher_w5;                 /* power update + cipher the uplink */
    fi[6] = (uint16_t)(index & 0x3ffu);             /* word6 = ARFCN index */
    fi[7] = (uint16_t)(index & 0x3ffu);             /* word7 = ARFCN index */
    fi[10] = (uint16_t)((fi[10] & 0x3ffu) | (uint16_t)(cell & 0x3f) << 10);
    fi[3] = (uint16_t)(fi[3] & 0x0fffu);            /* clear word3 bits 15:12 */
    if ((rc = post_fi(fi, ext)) != DSP_OK)
        return rc;
    /* Old per-burst model only: a TXBUFQ control word per burst. Stock's dedicated path
     * posts none (one 23-byte block + one PA-key per block; the DSP builds the bursts). */
    if (!l1_ul_perblock) {
        const uint8_t ctl[4] = { 0, (uint8_t)(bsic & 0x3f), 0, (uint8_t)(burst & 3) };
        if ((rc = tx_desc_post(0, 0, ctl, sizeof ctl)) != DSP_OK)
            return rc;
    }
    /* PA-key (command 4, word1 = block type): once per block in per-block mode. */
    if (!l1_ul_perblock || burst == 0) {
        const uint16_t cmd[5] = { 4, l1_ul_block_type, 1, 0, 0 };
        return post_cmd(cmd);
    }
    return DSP_OK;
}

/* The frame-synced uplink, split so the FI ring and the TXBUFQ are fed
 * independently (l1_dm_sched). l1_uplink_fi posts one burst instruction (type 7)
 * plus its PA-arm command; the four of a block go out on consecutive frames,
 * ahead of the DSP like any receive frame. l1_uplink_desc posts that burst's
 * TXBUFQ entries - the 23-byte block once (burst 0) then a control word per
 * burst - paced to how fast the DSP drains the ring, so five entries pass
 * through four slots without overrunning and without starving the frame clock. */
/* Enable A5 ciphering on the DSP with the SIM's Kc. Reverse-engineered from the
 * stock DSP (dsp-blob-CC874.dis): the ARM writes Kc to share +0x3f8 as 8 words,
 * one Kc byte per word, and sets FI word-5 bit 15 (-> 0xec70) so the DSP copies
 * them into its key register 0xec71..0xec78, with word-5 bits 14:13 = alg+1 as the
 * algorithm (the engine at 0x8640 activates only when that field == 1, i.e. A5/1).
 * The GSM frame-number COUNT the keystream also needs is the DSP's own (share
 * +0x3f0 -> 0xec4c, maintained per frame), so we supply only Kc and the mode bits.
 * alg: 0 = A5/1, 1 = A5/2, 2 = A5/3. alg 0xff disables. */
void l1_cipher_set(const uint8_t kc[8], uint8_t alg, uint32_t fn_offset)
{
    if (alg == 0xff) {
        cipher_w5 = 0;
        cipher_count_load = false;
        klog("dsp: cipher off");
        return;
    }
    for (int i = 0; i < 8; i++)
        sm_w16(CIPHER_KC_SHARE + 2u * (uint32_t)i, kc[i]);
    cipher_fn_offset = fn_offset % FN_MOD;
    cipher_count_load = true;            /* seed the COUNT on the first ciphered frame */
    cipher_w5 = (uint16_t)(0x8000u | ((uint16_t)((alg + 1) & 3) << 13));
    klog("dsp: cipher A5/%u enabled (w5 %04x, fn_off %lu)", alg + 1, cipher_w5,
         (unsigned long)cipher_fn_offset);
}

int l1_uplink_fi(uint16_t index, int tag, int burst, int tn, int tsc, int cell)
{
    uint16_t fi[11], ext[3];
    int rc;

    if (dsp_info.state != DSP_RUNNING)
        return DSP_ENOTRUN;
    if (tx_power_mode == L1_TX_OFF)
        return DSP_ECMD;
    if (l1_fi_free() < 1)
        return DSP_EFULL;
    fi_init(fi, ext, FI_BLOCK);
    fi[1] |= (uint16_t)((tag & 0xf) << 4 | (burst & 3));
    fi[3] = (uint16_t)((tn & 7) << 8);
    fi[4] = (uint16_t)((tsc & 7) << 8);
    fi[10] = (uint16_t)((cell & 0x3f) << 10 | 0x100u | 1u);
    fi[0] = (uint16_t)(FI_UPLINK | 0xa000u);
    fi[5] = txpwr_w5() | cipher_w5;
    fi[6] = (uint16_t)(index & 0x3ffu);
    fi[7] = (uint16_t)(index & 0x3ffu);
    fi[10] = (uint16_t)((fi[10] & 0x3ffu) | (uint16_t)(cell & 0x3f) << 10);
    fi[3] = (uint16_t)(fi[3] & 0x0fffu);
    if ((rc = post_fi(fi, ext)) != DSP_OK)
        return rc;
    { const uint16_t cmd[5] = { 4, 0, 1, 0, 0 }; return post_cmd(cmd); }
}

/* The TXBUFQ entries for one uplink burst: the 23-byte block (type 3) on burst 0,
 * then the 4-byte control word (type 0, byte 1 = BSIC). EFULL when the ring has
 * no room yet - the caller retries once the DSP has drained a slot. */
int l1_uplink_desc(int tag, int burst, uint8_t bsic, const uint8_t block[23])
{
    const uint8_t ctl[4] = { 0, (uint8_t)(bsic & 0x3f), 0, (uint8_t)(burst & 3) };
    int rc;

    if (txbuf_free() < (burst == 0 ? 2 : 1))
        return DSP_EFULL;
    if (burst == 0 && (rc = tx_desc_post((uint16_t)(tag & 0xf), 3, block, 23)) != DSP_OK)
        return rc;
    return tx_desc_post(0, 0, ctl, sizeof ctl);
}

/* Dedicated-mode establish (osmocom DM_EST): the DSP command-3 that moves the L1
 * toward a dedicated channel. Stock command builder FUN_0004b410: word0 = 3,
 * word1 = the channel/mode selector, word2 = the mapped gain (0/1 pass through,
 * 3->2, 4->3, else 0), word3 = the current frame, word4 = aux.
 *
 * IMPORTANT - this is only the DSP-command piece. In the stock, command 3 runs
 * inside the L1 STATE MACHINE (PSRAM FUN_003b13c6 -> FUN_000589d8), which also
 * drives the RF / AFC / timing setup (FUN_0002f348 "SetState", FUN_00062dc2..)
 * and the per-block dedicated scheduler. A working dedicated channel needs that
 * whole subsystem, which our L1 does not have yet (our RX works on broadcast
 * without it). So l1_dm_est posts the command; the L1-state/RF-control layer and
 * the scheduler are the remaining major pieces, and the path is HW-verifiable
 * only through the gated registration flow. See docs/l1-osmocom.md. */
int l1_dm_est(uint16_t sel, uint8_t gain, uint16_t aux)
{
    uint16_t g = gain <= 1 ? gain : gain == 3 ? 2 : gain == 4 ? 3 : 0;
    uint16_t cmd[5] = { 3, sel, g, (uint16_t)(frame_number & 0xffffu), aux };

    if (dsp_info.state != DSP_RUNNING)
        return DSP_ENOTRUN;
    return post_cmd(cmd);
}

/* Command 3 as the stock's idle camping sends it (FUN_000589d8): {3, cell, mode,
 * rxlev, flag | 1 << (8 + card)}, read on the DSP side by 0x9ab4 as cell, mode
 * (0..3), the gain source and bit 0 = start tracking. A tracked cell's burst
 * frequency error goes straight to the AFC (0x79b3) and 0xff72 bit 3 (camped) is
 * set, so the synthesiser follows the cell - the transmitter included. Untracked,
 * the error only feeds a per-cell receive offset (0xebe5 + cell) and the uplink
 * stays at the crystal's frequency. The Radio lab's {3, 0, 0, 38, 0x0101} read
 * 4 of 4 blocks with 8x better BEP (docs/gsm-plan.md). */
uint8_t l1_track_mode = 2;              /* dedicated-mode command 3: reaches auth (vs idle 0) */
uint16_t l1_track_flags = 0x0101u;      /* command-3 word 4 (flags): test knob */

int l1_cell_track(int cell, uint8_t rxlev)
{
    const uint16_t cmd[5] = { 3, (uint16_t)(cell & 0x3f), l1_track_mode,
                              (uint16_t)(rxlev & 0x7f), l1_track_flags };

    if (dsp_info.state != DSP_RUNNING)
        return DSP_ENOTRUN;
    return post_cmd(cmd);
}

/* Dedicated-mode hold - from the ON-PHONE stock DSP-call logger (2026-11-12, "Stock
 * OS USB logging" session), which supersedes the static-RE guesses above. The stock
 * dedicated establish posts, in strict order within one tick:
 *     command 3  {3, channel, 0, rxlev, 0x0101}   (cell/channel select; posted ONCE)
 *     command 0x1a {0x1a, 0, 0, 0, 0}              (all-zero tail)
 *     command 0x1f {0x1f, 3, 0, 1, 0}              (tune/commit tail)
 * then enters the dedicated state. It HOLDS the channel by re-posting
 *     command 0x0d {0x0d, 1, 0, 0, 0}
 * at the dedicated-active transition and ~every 0.6-1.4 s after - and it NEVER
 * re-posts command 3 during the dedicated phase (our old every-block re-post was not
 * the stock mechanism). There is NO DSP command 0x0c: the stock "SetState" is an
 * ARM-internal L1 state (the ~S lines in the logger), not a DSP post. l1_dm_est_tail
 * posts the two tail commands right after command 3; l1_dm_refresh is the 0x0d hold. */
int l1_dm_est_tail(void)
{
    const uint16_t a[5] = { 0x1a, 0, 0, 0, 0 };
    const uint16_t b[5] = { 0x1f, 3, 0, 1, 0 };
    int rc;

    if (dsp_info.state != DSP_RUNNING)
        return DSP_ENOTRUN;
    if ((rc = post_cmd(a)) != DSP_OK)
        return rc;
    return post_cmd(b);
}

uint16_t l1_dm_refresh_w1 = 1;          /* command-0x0d word 1 (stock = 1; test knob) */
uint8_t  l1_dm_reposts = 1;             /* 1 = also re-post command 3 per block (old path) */

int l1_dm_refresh(void)
{
    const uint16_t cmd[5] = { 0xd, l1_dm_refresh_w1, 0, 0, 0 };

    if (dsp_info.state != DSP_RUNNING)
        return DSP_ENOTRUN;
    return post_cmd(cmd);
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
    .si_parse = si_parse, .cell_sync = l1_cell_sync, .cell_read = l1_cell_read,
    .cell_idle = l1_cell_idle, .rxlev_measure = l1_rxlev_measure, .cell_c1 = l1_cell_c1,
    .cell_select = l1_cell_select, .cell_stay = l1_cell_stay,
    .cch_classify = l1_cch_classify, .cch_read = l1_cch_read,
    .tx_min_power = l1_tx_min_power, .tx_min_power_on = l1_tx_min_power_on,
    .cal_readback = l1_cal_readback, .access_post = l1_access_post,
    .uplink_post = l1_uplink_post, .dm_est = l1_dm_est, .dm_sched = l1_dm_sched,
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

/* Bare power-measure filler FI (type 0x0b, no ARFCN list - words 6..9 stay 0xffff),
 * i.e. measure the serving/dedicated channel this frame. Stock fills nearly every
 * non-burst dedicated frame with this (logger v6: 499 type-0x0b FIs across one 3.6 s
 * LU, 283 ~P results), which keeps the DSP's power/AFC measurement - and so the
 * dedicated lock - alive the whole LU. Our dedicated scheduler used FI_IDLE (type 0)
 * there instead, so the AFC coasted on the camp lock and the decode collapsed ~1.3 s
 * in. Use this as the dedicated filler so the AFC keeps tracking the SDCCH. */
int l1_pm_idle_post(int frames)
{
    uint16_t fi[11], ext[3];

    for (int i = 0; i < frames; i++) {
        int rc;

        fi_init(fi, ext, FI_POWER);
        if ((rc = post_fi(fi, ext)) != DSP_OK)
            return rc;
    }
    return DSP_OK;
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
