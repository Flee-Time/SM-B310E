/*
 * B310E-OS — arch/diag_dsp_main.c
 *
 * DSP BOOT DIAGNOSTIC (`make dsp-boot` -> os-dsp-boot.bin).
 * Boots the SC6530C's TeakLite DSP on real hardware and displays EVERY
 * register value on the LCD (hex) so the user can read the state off and
 * report.
 *
 * HW STATUS (user-tested 2026-08-27): the download machinery WORKS — stage
 * 04 (DSP_READY after the reset release) and stage 05 (all 66 download
 * blocks with per-block READY_TO_COPY/COPY_DONE/DSP_IRQ) passed. The
 * post-boot status check (stage 07) got NO response. This build keeps the
 * boot sequence EXACTLY as-tested and expands the diagnostics: every
 * register write is read back and printed (before/after), and the status
 * check pokes BOTH control areas for 2 s while sampling the DSP's message
 * area (0x1111/0x2222 @ 0x10000DC30/DC32), its working state (0x10000EB0x)
 * and the status area — so the report distinguishes "the DSP program never
 * runs" from "the status-check base is wrong".
 *
 * SEQUENCE (register-for-register from the stock firmware downloader,
 * verified with the Ghidra MCP on dump_firmware.bin; see
 * .omo/notepads/b310e-rockbox-port/learnings.md "DSP boot"):
 *
 *   FUN_0003aa76 (download orchestrator, dump 0x3aa76):
 *     [0x0422E5A0] = 0x10000000            share-mem control base
 *     FUN_00032ea4(1)                      0x8B000060 = 0x10000   hold DSP reset
 *     FUN_0003ad6e                         clear share-mem control (20 B)
 *     arm_ctl = ARM_READY (1)
 *     FUN_0003ad16 (boot-enable, dump 0x3ad16):
 *       FUN_00032eac(4)                    0x8B0001C0: STRAP = USER_RST_MODE (4<<3)
 *       FUN_0003a9ec -> 0xA800            boot vector (DSP_DL_BASE_ADDRESS)
 *       FUN_00032eec                      0x8B0001A0[15:0] = 0xA800
 *       FUN_00032ef4(1)                    0x8B0001A0 |= 0x10000 (ARM_BOOT_EN)
 *       arm_ctl = ARM_READY (1)
 *       FUN_00032ea4(0)                    0x8B000064 = 0x10000   RELEASE reset
 *       FUN_00032ecc(0)                    0x8B000160 = 4         DSP_IRQ_CLR
 *     FUN_0003a92a (download loop, dump 0x3a92a):
 *       wait dsp_ctl & DSP_READY (1)
 *       blocks of 1024 halfwords via FUN_0003a810 (dl_offset/dl_block_size/
 *       memcpy(share+8)/DATA_READY/READY_TO_COPY/DATA_READY-clr/START_COPY/
 *       COPY_DONE/INT_STS0 DSP_IRQ/INT_SET_CLR0 4|8)
 *       arm_ctl = BOOT_DONE (8)
 *     strap EXT_PROG_MODE; BOOT_EN off; hold reset; delay; release -> DSP runs
 *
 *   FUN_0001a516 (stock post-boot status check, dump 0x1a516): the control
 *     base is the runtime pointer [0x0422E598] (shared with the UART
 *     downloader FUN_000185fc; the QEMU observation says 0x10000FE0):
 *       [base+4] = 0; [base+0] = 3; [0x8B000160] |= 2 (MCU_FRQ_SET); wait
 *       ~30 ticks; result: (u16)[base+4] == 1.
 *
 * REGISTER-MAP NOTE (SC6530C differs from the SC6531EFM SDK): the DSP
 * soft-reset is bit 16 of the APB_EB0 SET/CLR pair 0x8B000060/0x64 (the
 * dump's FUN_00067fc8), NOT the SDK's APB_RST0 0x8B001068/0x8B002068
 * (0 literal hits). The strap/boot-vector/BOOT_EN registers are read back
 * and printed — the before/after shows whether they stuck.
 *
 * SAFETY: no 0x8cxxxxxx writes; bounded waits only; USB skipped
 * (-DSD_BOOT_NO_USB — the fpmain card-boot launch has USB unpowered).
 */

#include "os.h"
#include "chip.h"
#include "../drivers/lcd.h"
#include "../drivers/led.h"
#include "../drivers/usb_debug.h"

/* ---- embedded DSP program (build/dsp_seg0.o, objcopy -I binary) ------- */
extern const uint8_t _binary_build_dsp_seg0_bin_start[];
extern const uint8_t _binary_build_dsp_seg0_bin_end[];

#define DSP_PROG_HW \
    ((uint32_t)((_binary_build_dsp_seg0_bin_end - _binary_build_dsp_seg0_bin_start) >> 1))

/* ---- chip init (verbatim from arch/main.c / diag_sd_main.c — main.o is
 * excluded from diag links, so this image carries its own copy) ---------- */
#define SMC_INIT_BUF (0x40000000u + 0xa000u - 1024u)   /* 0x40009c00 */

extern int sc6530_init_smc_asm[1];
extern int sc6530_init_smc_asm_end[1];

static void dsp_init_freq(void)
{
    uint32_t a = MEM4(0x8b000040);

    MEM4(0x8b000040) = a |= 4;
    MEM4(0x8b000040) = (a & ~3) | 1;    /* 208 MHz */
    DELAY(100);
}

static void dsp_init_smc(void)
{
    int *p = sc6530_init_smc_asm;
    int n = sc6530_init_smc_asm_end - p;
    int *d = (int *)SMC_INIT_BUF, *f = d;

    do *d++ = *p++; while (--n);
    ((void (*)(void))(uintptr_t)f)();
}

static void dsp_init_adi(void)
{
    MEM4(0x8b0000a0) = 1 << 24;
    MEM4(0x8b000060) = 1 << 19;
    DELAY(100);
    MEM4(0x8b000064) = 1 << 19;
    MEM4(0x82000000) &= ~(1 << 4);
    MEM4(0x82000004) = 0x55000;
}

static void dsp_init_iram(void)
{
    MEM4(0x8b0001a0) |= 7 << 19;
}

static void dsp_init_power(void)
{
    MEM4(0x20500060) |= 0x40;           /* LCM   */
    MEM4(0x20500060) |= 0x1000;         /* LCDC  */
    MEM4(0x20500060) |= 0x400;          /* SDIO0 */
    MEM4(0x8b0000a0) |= 0x80040;        /* keypad */
    MEM4(0x8b0000a0) |= 0x800080;       /* GPIO_D */
}

/* ---- DSP boot registers (SC6530C, Ghidra-verified) --------------------- */
#define APB_MCU_CTL0    0x8B0001A0u     /* ARM_BOOT_ADDR [15:0], ARM_BOOT_EN b16 */
#define APB_DSP_CTL0    0x8B0001C0u     /* STRAP_BITS [7:3], DSP_CLK_FORCE_ON b1 */
#define APB_EB0_SET     0x8B000060u     /* SC6530C DSP_SOFT_RST b16 (hold)      */
#define APB_EB0_CLR     0x8B000064u     /* SC6530C DSP_SOFT_RST b16 (release)   */
#define APB_INT_STS0    0x8B000140u     /* DSP_IRQ b2                           */
#define APB_INT_SET_CLR 0x8B000160u     /* MCU_IRQ_SET b0, MCU_FRQ_SET b1,
                                           DSP_IRQ_CLR b2, DSP_FRQ_CLR b3       */
#define SHARE_BASE      0x10000000u     /* DSP share-mem base (SC6530C)         */
#define SHARE_STATUS    0x10000FE0u     /* post-boot status area (stock check)  */

#define DSP_BOOT_VECTOR 0xA800u         /* DSP_DL_BASE_ADDRESS (SC6530 class)   */
#define USER_RST_MODE   4u
#define EXT_PROG_MODE   0u
#define DSP_DL_BLOCK_HW 1024u           /* halfwords per download block         */
#define DSP_WAIT_BUDGET 10000000u       /* bounded poll budget                  */
#define DSP_WAIT_KEEP   10000u          /* keepalive cadence (DSP_DL_WAIT_TIME) */
#define DSP_STATUS_POLL_MS 2000u        /* post-boot status poll window         */

#define ARM_READY       0x0001u
#define ARM_DATA_READY  0x0002u
#define ARM_START_COPY  0x0004u
#define ARM_BOOT_DONE   0x0008u
#define DSP_READY       0x0001u
#define DSP_READY_TO_COPY 0x0002u
#define DSP_COPY_DONE   0x0004u

/* ---- LCD colours ------------------------------------------------------- */
#define COL_BG   0x001fu                 /* blue                                  */
#define COL_TITLE 0xffffu
#define COL_STAGE 0xffe0u                /* yellow                                */
#define COL_OK   0x07e0u                 /* green                                 */
#define COL_ERR  0xf800u                 /* red                                   */

/* ---- tiny hex formatters (no stdio in this image) ---------------------- */
static void hx4(char *o, uint16_t v)
{
    static const char hx[] = "0123456789ABCDEF";

    o[0] = hx[(v >> 12) & 0xf]; o[1] = hx[(v >> 8) & 0xf];
    o[2] = hx[(v >> 4) & 0xf];  o[3] = hx[v & 0xf];
    o[4] = '\0';
}

static void hx8(char *o, uint32_t v)
{
    hx4(o, (uint16_t)(v >> 16));
    hx4(o + 4, (uint16_t)v);
}

/* ---- LCD row writer (6x8 small font, 21 cols x 20 rows) ---------------- */
static void diag_row(int row, const char *s, uint16_t fg)
{
    lcd_print_small(0, row * 8, s, fg, COL_BG);
    lcd_show_bounded();
}

/* "NAME=VALUE" (16-bit) or "NAME=VALUE" (32-bit) on a fixed row. */
static void diag_hx(int row, const char *name, uint32_t v, int wide,
                    uint16_t fg)
{
    char t[22];
    int i = 0;

    while (*name && i < 17) t[i++] = *name++;
    t[i++] = '=';
    if (wide) {
        hx8(t + i, v);
    } else {
        hx4(t + i, (uint16_t)v);
    }
    diag_row(row, t, fg);
}

/* ---- share-mem access (double-access discipline, dsp_ctrl_hal.c) ------- */
static void sm_write16(uint32_t addr, uint16_t v)
{
    volatile uint16_t *p = (volatile uint16_t *)addr;

    do { *p = v; } while (*p != v);
}

static uint16_t sm_read16(uint32_t addr)
{
    volatile uint16_t *p = (volatile uint16_t *)addr;
    uint16_t a, b;

    do { a = *p; b = *p; } while (a != b);
    return a;
}

/* Wait for `mask` in the u16 at `addr`, re-asserting `keepalive` on the
 * arm_ctl word every DSP_WAIT_KEEP iterations (the SDK's keepalive rule).
 * Returns 0 on success, -1 on budget expiry. */
static int sm_wait_dsp(uint32_t arm_addr, uint32_t dsp_addr, uint16_t mask,
                       uint16_t keepalive)
{
    uint32_t gap = 0, total = 0;

    for (;;) {
        if (sm_read16(dsp_addr) & mask)
            return 0;
        if (++total >= DSP_WAIT_BUDGET)
            return -1;
        if (++gap >= DSP_WAIT_KEEP) {
            gap = 0;
            sm_write16(arm_addr, keepalive);
        }
    }
}

/* The stock's PROTOCOL_10: wait for the DSP→ARM IRQ status bit. */
static int wait_dsp_irq(void)
{
    uint32_t n = DSP_WAIT_BUDGET;

    while (!(MEM4(APB_INT_STS0) & 0x4u)) {
        if (--n == 0)
            return -1;
    }
    return 0;
}

/* Download one block (FUN_0003a810). The DATA area (share-mem + 8) is
 * WORD-ORIENTED: the stock copies it with sci_mem16cpy (16-bit stores;
 * the protocol's unit is halfwords). Byte stores corrupt the stream — the
 * DSP reads every other byte = garbage = faults on the first instruction.
 * If `dst_readback` is non-NULL, the first u16 of the just-written block is
 * read back there (proves the write landed before the DSP consumes it).
 * Returns 0 or -1. */
static int dsp_dl_block(const uint8_t *prog, uint32_t off_hw, uint32_t size_hw,
                        uint16_t *dst_readback)
{
    volatile uint16_t *data = (volatile uint16_t *)(SHARE_BASE + 8);
    const uint16_t *src = (const uint16_t *)(prog + (off_hw << 1));
    uint32_t i;

    sm_write16(SHARE_BASE + 4, (uint16_t)off_hw);      /* dl_offset     */
    sm_write16(SHARE_BASE + 6, (uint16_t)size_hw);     /* dl_block_size */
    for (i = 0; i < size_hw; i++)
        data[i] = src[i];   /* 16-bit word writes — the data port is
                             * word-oriented (sci_mem16cpy in the stock) */
    if (dst_readback)
        *dst_readback = *data;

    sm_write16(SHARE_BASE + 0, ARM_READY | ARM_DATA_READY);
    if (sm_wait_dsp(SHARE_BASE, SHARE_BASE + 2, DSP_READY_TO_COPY,
                    ARM_READY | ARM_DATA_READY) != 0)
        return -1;

    sm_write16(SHARE_BASE + 0, sm_read16(SHARE_BASE + 0) & ~ARM_DATA_READY);
    sm_write16(SHARE_BASE + 0, sm_read16(SHARE_BASE + 0) | ARM_START_COPY);
    if (sm_wait_dsp(SHARE_BASE, SHARE_BASE + 2, DSP_COPY_DONE,
                    ARM_START_COPY) != 0)
        return -1;

    if (wait_dsp_irq() != 0)
        return -1;
    MEM4(APB_INT_SET_CLR) = 0x4u | 0x8u;    /* DSP_IRQ_CLR | DSP_FRQ_CLR */

    sm_write16(SHARE_BASE + 0, sm_read16(SHARE_BASE + 0) & ~ARM_START_COPY);
    return 0;
}

/* Download + boot the DSP program (stock FUN_0003aa76 flow), printing the
 * register before/after values on the LCD as it goes. Returns the failing
 * stage number (4 = no DSP_READY, 5 = block timeout) or 0 on success. */
static int dsp_download_boot(const uint8_t *prog, uint32_t prog_hw)
{
    uint32_t blocks = (prog_hw + DSP_DL_BLOCK_HW - 1u) / DSP_DL_BLOCK_HW;
    uint32_t b;

    /* stage 1: hold reset + clear the share-mem control block */
    MEM4(APB_EB0_SET) = 0x10000u;
    for (b = 0; b < 10u; b++)
        sm_write16(SHARE_BASE + (b << 1), 0);
    sm_write16(SHARE_BASE + 0, ARM_READY);
    led_keylight_set(4);
    diag_row(2, "HOLD 060=10000", COL_STAGE);

    /* stage 2: strap USER_RST_MODE + boot vector + BOOT_EN (print the
     * before/after so the user sees whether the SC6530C registers stuck) */
    MEM4(APB_DSP_CTL0) = (MEM4(APB_DSP_CTL0) & ~0xf8u) | (USER_RST_MODE << 3);
    diag_hx(3, "STRAP 1C0", (MEM4(APB_DSP_CTL0) >> 3) & 0x1fu, 0, COL_STAGE);

    MEM4(APB_MCU_CTL0) = (MEM4(APB_MCU_CTL0) & 0xffff0000u) | DSP_BOOT_VECTOR;
    diag_hx(4, "BVEC 1A0", (uint16_t)MEM4(APB_MCU_CTL0), 0, COL_STAGE);

    MEM4(APB_MCU_CTL0) |= 0x10000u;                     /* ARM_BOOT_EN      */
    diag_hx(5, "BOOTEN 1A0", MEM4(APB_MCU_CTL0), 1, COL_STAGE);
    sm_write16(SHARE_BASE + 0, ARM_READY);

    /* stage 3: release reset -> the DSP boot ROM runs the download
     * protocol; clear any pending DSP IRQ */
    MEM4(APB_EB0_CLR) = 0x10000u;                       /* DSP_SOFT_RST rel */
    MEM4(APB_INT_SET_CLR) = 0x4u;                       /* DSP_IRQ_CLR      */
    led_keylight_set(5);
    diag_row(6, "RELS 064=10000", COL_STAGE);

    /* stage 4: PROTOCOL_3 — wait for DSP_READY (print the value seen) */
    if (sm_wait_dsp(SHARE_BASE, SHARE_BASE + 2, DSP_READY, ARM_READY) != 0)
        return 4;
    diag_hx(7, "RDY sm2", sm_read16(SHARE_BASE + 2), 0, COL_STAGE);
    led_keylight_set(6);

    /* stage 5: download all blocks (word writes). Block 0's first u16 is
     * read back IMMEDIATELY after the write (before the DSP consumes it):
     * 0x4180 = the write landed; 0xFFFF = the data area is a write-only/
     * consumed port. */
    {
        uint16_t blk0 = 0;
        for (b = 0; b < blocks; b++) {
            uint32_t off = b * DSP_DL_BLOCK_HW;
            uint32_t sz = (off + DSP_DL_BLOCK_HW <= prog_hw)
                              ? DSP_DL_BLOCK_HW
                              : (prog_hw - off);
            if (dsp_dl_block(prog, off, sz, b == 0 ? &blk0 : NULL) != 0)
                return 5;
        }
        diag_hx(8, "DL0 SRC", (uint16_t)((uint16_t)prog[0] | ((uint16_t)prog[1] << 8)),
                0, COL_STAGE);
        diag_hx(9, "DL0 DST", blk0, 0, blk0 == 0x4180 ? COL_OK : COL_STAGE);
        diag_hx(10, "DL blk", (uint16_t)blocks, 0, COL_STAGE);
    }
    sm_write16(SHARE_BASE + 0, ARM_BOOT_DONE);
    led_keylight_set(7);

    /* stage 6: strap back to EXT_PROG_MODE, BOOT_EN off, hold+release
     * reset so the DSP runs the downloaded program. The stock sleeps
     * ~13 ms (FUN_00010852(0xd)) between BOOT_DONE and the strap change —
     * the boot ROM finalizes the download internally; mirror that. */
    lcd_delay_ms(13);
    MEM4(APB_DSP_CTL0) = (MEM4(APB_DSP_CTL0) & ~0xf8u) | (EXT_PROG_MODE << 3);
    MEM4(APB_MCU_CTL0) &= ~0x10000u;
    MEM4(APB_EB0_SET) = 0x10000u;
    DELAY(100);
    MEM4(APB_EB0_CLR) = 0x10000u;
    {
        char t[22];
        t[0] = 'R'; t[1] = 'E'; t[2] = 'L'; t[3] = 'S'; t[4] = '2';
        t[5] = ' '; t[6] = '1'; t[7] = 'A'; t[8] = '0'; t[9] = '=';
        hx8(t + 10, MEM4(APB_MCU_CTL0));
        t[18] = ' '; t[19] = 'S';
        {
            static const char hx[] = "0123456789ABCDEF";
            t[20] = hx[(MEM4(APB_DSP_CTL0) >> 3) & 0xf];
        }
        t[21] = '\0';
        diag_row(11, t, COL_STAGE);
    }
    led_keylight_set(8);
    return 0;
}

/* The extended post-boot status probe (H1 + H3): let the DSP init settle
 * (~500 ms), sample a 10-location map (T0), poke BOTH control areas +
 * the DSP FRQ for 2 s while re-sampling (T1), and print the CHANGED-location
 * count + the key response values. Returns:
 *   2 = a real response (0x1111/0x2222 message, or dl_offset==1 in either
 *       control base),
 *   1 = DSP activity only (a location changed — the DSP executes but the
 *       response mechanism differs),
 *   0 = nothing changed (the DSP never ran).
 *
 * AHB-STALL SAFETY: the map is EXACTLY the 10 locations PROVEN readable on
 * HW (the completed build: DC30/DC32, FE4/0004, FE4A/FE78, EB02/EB14,
 * EC30/EC32). The DSP I/O-window registers 0x1000FB9D-0x1000FFFC and the
 * never-proven 0x1000EB00/EC1B/ECAD/ED85/EDB6/FE0 are CLOCK-GATED / unknown:
 * a read STALLS THE AHB permanently (the hard freeze at "PROBE 2500ms") —
 * NEVER add unproven addresses to a poll.
 *
 * KEYLIGHT PINS (the user reports the last level): 13 = T0 reads begin,
 * 8 = T0 done, then per read (1=DC30 2=DC32 3=FE4 4=0004 5=FE4A 6=FE78
 * 7=EB02 8=EB14 9=EC30 10=EC32), 11 = about to FRQ-poke, 6 = loop done.
 * A hang shows the EXACT stalling read (8 = EB14; 11 = the FRQ poke). */
static int dsp_status_probe(void)
{
    static const uint32_t map[10] = {
        0x1000DC30u, 0x1000DC32u,        /* DSP->ARM messages 1111/2222 */
        0x10000FE4u, 0x10000004u,        /* status + download dl_offset  */
        0x1000FE4Au, 0x1000FE78u,        /* previously-proven reads      */
        0x1000EB02u, 0x1000EB14u,        /* working state (RAM)          */
        0x1000EC30u, 0x1000EC32u,        /* E000-window-shift guesses    */
    };
    uint16_t t0[10], t1[10];
    uint32_t i, j, changed = 0, alive = 0;

    diag_row(12, "PROBE 2500ms", COL_STAGE);
    lcd_delay_ms(500);

    led_keylight_set(13);               /* T0 reads begin */
    for (j = 0; j < 10; j++)
        t0[j] = sm_read16(map[j]);
    led_keylight_set(8);                /* T0 done */

    for (i = 0; i < DSP_STATUS_POLL_MS; i++) {
        if ((i % 100) == 0) {
            sm_write16(SHARE_BASE + 4, 0);
            sm_write16(SHARE_BASE + 0, ARM_READY | ARM_DATA_READY);
            sm_write16(SHARE_STATUS + 4, 0);
            sm_write16(SHARE_STATUS + 0, ARM_READY | ARM_DATA_READY);
            led_keylight_set(11);       /* about to FRQ-poke the DSP */
            MEM4(APB_INT_SET_CLR) |= 0x2u;
        }
        for (j = 0; j < 10; j++) {
            led_keylight_set((int)j + 1);   /* pin: level = read index+1 */
            t1[j] = sm_read16(map[j]);
        }
        lcd_delay_ms(1);
    }
    led_keylight_set(6);                /* loop done */

    for (j = 0; j < 10; j++)
        if (t1[j] != t0[j])
            changed++;
    if (t1[2] == 1 || t1[3] == 1)
        alive = 2;
    if (t1[0] == 0x1111 || t1[1] == 0x2222)
        alive = 2;

    /* DLTA n/10 — how many of the 10 locations the DSP wrote */
    {
        char t[22];
        t[0] = 'D'; t[1] = 'L'; t[2] = 'T'; t[3] = 'A'; t[4] = ' ';
        t[5] = (char)('0' + (changed / 10));
        t[6] = (char)('0' + (changed % 10));
        t[7] = '/'; t[8] = '1'; t[9] = '0'; t[10] = '\0';
        diag_row(12, t, changed ? COL_OK : COL_STAGE);
    }
    /* key response rows (rows 13-16; rows 17-18 are the verdict) */
    {
        char t[22];
        t[0] = 'D'; t[1] = 'C'; t[2] = '3'; t[3] = '0'; t[4] = '=';
        hx4(t + 5, t1[0]);
        t[9] = ' '; t[10] = 'D'; t[11] = 'C'; t[12] = '3'; t[13] = '2'; t[14] = '=';
        hx4(t + 15, t1[1]);
        t[19] = '\0';
        diag_row(13, t,
                 (t1[0] == 0x1111 || t1[1] == 0x2222) ? COL_OK : COL_STAGE);
    }
    {
        char t[22];
        t[0] = 'F'; t[1] = 'E'; t[2] = '4'; t[3] = '=';
        hx4(t + 4, t1[2]);
        t[8] = ' '; t[9] = '0'; t[10] = '0'; t[11] = '4'; t[12] = '=';
        hx4(t + 13, t1[3]);
        t[17] = '\0';
        diag_row(14, t, (t1[2] == 1 || t1[3] == 1) ? COL_OK : COL_STAGE);
    }
    {
        char t[22];
        t[0] = 'F'; t[1] = 'E'; t[2] = '4'; t[3] = 'A'; t[4] = '=';
        hx4(t + 5, t1[4]);
        t[9] = ' '; t[10] = 'F'; t[11] = 'E'; t[12] = '7'; t[13] = '8'; t[14] = '=';
        hx4(t + 15, t1[5]);
        t[19] = '\0';
        diag_row(15, t, COL_STAGE);
    }
    {
        char t[22];
        t[0] = 'E'; t[1] = 'B'; t[2] = '1'; t[3] = '4'; t[4] = '=';
        hx4(t + 5, t1[7]);
        t[9] = ' '; t[10] = 'E'; t[11] = 'C'; t[12] = '3'; t[13] = '0'; t[14] = '=';
        hx4(t + 15, t1[8]);
        t[19] = '\0';
        diag_row(16, t, COL_STAGE);
    }

    return alive ? 2 : (changed ? 1 : 0);
}

/* ---- scheduler-free codec + VBC tone (5B2). Extracted VERBATIM from
 * drivers/audio.c's proven chain (audio_power_on/aud_dac_path_on/
 * audio_dac_on/audio_dp_dac_on/audio_pa_enable/audio_pa_mute + the VBC DA
 * block of audio_beep), which uses sched_ticks()/task_yield() — unavailable
 * in the diag app. Order + registers are audio.c's HW-tested safe sequence
 * (gains only after clocks/DAC; r2=1 DAC path only — all-three HANGS).
 * ADI writes are direct stores (SC6530: no mailbox write command); reads go
 * through the ADI mailbox. The tone is a sustained 440 Hz square wave at
 * the 8 kHz VBC rate — the old DAC_DATA_TX_ADDR probe (0x82001288 bits 0-1,
 * 4 frequencies) was DROPPED after 0 literal hits in the dump (learnings
 * WS1); the VBC ping-pong buffers are refilled every 10 ms. ------------- */

#define ADI_MBX 0x82000000u

static void adi_w(uint32_t addr, uint32_t v)
{
    uint32_t n = 1000000u;

    while (MEM4(ADI_MBX + 0x20) & 0x200u)       /* FIFO_FULL */
        if (--n == 0) return;
    MEM4(addr) = v;
    n = 1000000u;
    while (!(MEM4(ADI_MBX + 0x20) & 0x100u))    /* wait FIFO_EMPTY */
        if (--n == 0) return;
}

static uint32_t adi_r(uint32_t addr)
{
    uint32_t n = 1000000u;

    MEM4(ADI_MBX + 0x18) = addr & 0xFFFu;
    while (MEM4(ADI_MBX + 0x1C) & 0x80000000u)
        if (--n == 0) return 0;
    return MEM4(ADI_MBX + 0x1C) & 0xFFFFu;
}

static void aud_gpio_set(uint32_t id, uint32_t off, int state)
{
    uint32_t base = 0x8A000000u + ((id >> 4) << 7);
    uint32_t bit = 1u << (id & 0xFu);
    uint32_t v = MEM4(base + off);

    MEM4(base + off) = state ? (v | bit) : (v & ~bit);
}

/* The SC6530C subsystem power phase for the audio (P2-P6 of the emulator
 * capture — docs/dsp-audio-route.md §5; the D1-D4 divergences).
 * The stock enables the ANA audio clocks + the APB/AHB device gates + the
 * ANA pads BEFORE any codec register write. WITHOUT this the codec analog
 * output stage never powers (the D10 structural gap — our earlier tones
 * were silent for exactly this reason). The 0x8c pinmap half stays BANNED
 * on HW; only the mailbox-safe ANA pad half is applied here. */
static void dsp_audio_subsys_power(void)
{
    static const uint32_t apb[23] = {
        0x01000000u, 0x00400000u, 0x00010000u, 0x40000000u, 0x00000002u,
        0x00200000u, 0x00000200u, 0x00080000u, 0x00000040u, 0x00800000u,
        0x00000080u, 0x04000000u, 0x02000000u, 0x00000100u, 0x00002000u,
        0x00000400u, 0x00000800u, 0x00000010u, 0x00008000u, 0x00004000u,
        0x00020000u, 0x00000008u, 0x80000000u,
    };
    static const uint32_t ahb[14] = {
        0x00004000u, 0x00000002u, 0x00000400u, 0x00000100u, 0x00000001u,
        0x00000004u, 0x00000008u, 0x00000800u, 0x00010000u, 0x00008000u,
        0x00000020u, 0x00001000u, 0x00000040u, 0x00000080u,
    };
    static const uint32_t aclk[15] = {       /* the D1 ANA clock block    */
        0x0004u, 0x0002u, 0x0010u, 0x0010u, 0x0020u, 0x0002u, 0x0001u,
        0x0008u, 0x0004u, 0x0002u, 0x0008u, 0x0040u, 0x0020u, 0x0080u,
        0x0100u,
    };
    static const uint16_t apad[6] = {        /* the ANA pad half (D4)     */
        0x18Au, 0x18Au, 0x10Au, 0x0100u, 0x0100u, 0x0101u,
    };
    uint32_t i;

    MEM4(0x205000C0) = MEM4(0x205000C0) | 0x60u;            /* P2: AHB gate */

    for (i = 0; i < 23u; i++)                                /* P3: APB */
        MEM4(0x8B0000A0) = MEM4(0x8B0000A0) | apb[i];

    for (i = 0; i < 15u; i++)                                /* P4: ANA clk */
        adi_w((i & 1u) ? 0x820010E4u : ((i == 5u || i == 6u) ? 0x82001040u
                                                             : 0x820010E0u),
              aclk[i]);

    for (i = 0; i < 14u; i++)                                /* P5: AHB */
        MEM4(0x20500060) = MEM4(0x20500060) | ahb[i];

    adi_w(0x82001874, apad[0]);                              /* P6: ANA pads */
    adi_w(0x82001850, apad[1]);
    adi_w(0x82001878, apad[2]);
    adi_w(0x8200187C, apad[3]);
    adi_w(0x82001884, apad[4]);
    adi_w(0x82001880, apad[5]);
}

/* VBC DA feed modes (the phase variable of the 2026-08-28 round). The SDK
 * DAI plays via the SYSTEM DMA (audio_dai_vbc.c _VBC_DMA_DAC_Send) — a
 * DMA-EN bit (b13/b14 on 0x82003018) makes the VBC assert its DMA request
 * line, and WITHOUT a servicing DMA channel the DA may never consume
 * CPU-written data. vbc_phy_v0.c's __vbc_write (the CPU path) never touches
 * the DMA-EN bits and writes VBDAL/VBDAR directly; and its __vbc_clear_da_buffer
 * comment says the MCU buffer access (VBRAMSW_EN b10) requires VBENABLE LOW —
 * the previous tone toggled VBRAMSW_NUMBER while VBENABLE was HIGH. All three
 * feed modes are tested with DISTINCT frequencies so ONE audible tone tells
 * the user which combination connects. */
#define VBC_MODE_DIRECT 0u  /* VBRAMSW off, DMA-EN off: raw CPU writes to
                             * VBDAL/VBDAR (the __vbc_write register path) */
#define VBC_MODE_VRAMSW 1u  /* stop-start ping-pong: VBENABLE LOW -> VBRAMSW
                             * fill -> VBENABLE HIGH (the hardware-legal
                             * MCU-buffer dance, choppy by design) */
#define VBC_MODE_DMAEN  2u  /* DMA-EN 0x6000 set (VBC asserts its DMA request)
                             * + direct writes — the current-build mode */

/* Write a 160-sample square wave (8 kHz VBC rate) to VBDAL/VBDAR. */
static void tone_fill_direct(uint32_t freq_hz)
{
    uint32_t half_n = 8000u / 2u / freq_hz, j;

    if (half_n < 2u) half_n = 2u;
    for (j = 0; j < 160u; j++) {
        int16_t s = ((j / half_n) & 1u) ? 0x1800 : (int16_t)-0x1800;
        MEM4(0x82003000) = (uint32_t)(uint16_t)s;
        MEM4(0x82003004) = (uint32_t)(uint16_t)s;
    }
}

/* Fill ONE ping-pong buffer through the MCU RAM switch. VBENABLE must be
 * LOW for the buffer access (vbc_phy_v0.c __vbc_clear_da_buffer rule). */
static void tone_fill_vramsw(uint32_t freq_hz, uint32_t which)
{
    MEM4(0x82003018) = MEM4(0x82003018) & ~0x8000u;         /* VBENABLE low */
    MEM4(0x82003018) = MEM4(0x82003018) | 0x400u;           /* VBRAMSW_EN   */
    if (which)
        MEM4(0x82003018) = MEM4(0x82003018) | 0x200u;       /* buffer 1     */
    else
        MEM4(0x82003018) = MEM4(0x82003018) & ~0x200u;      /* buffer 0     */
    tone_fill_direct(freq_hz);
    MEM4(0x82003018) = MEM4(0x82003018) & ~0x400u;          /* VBRAMSW_EN   */
}

/* Play `freq_hz` for `ms` in `mode` (VBC_MODE_*). Every phase keeps the
 * full codec chain (already open) and just changes the VBC feed style. */
static void tone_phase(uint32_t freq_hz, uint32_t ms, uint32_t mode)
{
    uint32_t t;

    if (mode == VBC_MODE_DMAEN)
        MEM4(0x82003018) = MEM4(0x82003018) | 0x6000u;      /* DMA-EN on  */
    else
        MEM4(0x82003018) = MEM4(0x82003018) & ~0x6000u;     /* DMA-EN off */

    for (t = 0; t < (ms / 10u); t++) {
        if (mode == VBC_MODE_VRAMSW) {
            tone_fill_vramsw(freq_hz, t & 1u);
            MEM4(0x82003018) = MEM4(0x82003018) | 0x8000u;  /* VBENABLE   */
        } else {
            if (t == 0)
                MEM4(0x82003018) = MEM4(0x82003018) | 0x8000u;
            tone_fill_direct(freq_hz);
        }
        if ((t % 50u) == 0u) {
            adi_w(0x820011C0, adi_r(0x820011C0) | 0x19u);
            adi_w(0x820011A0, adi_r(0x820011A0) | 0x2u);
        }
        lcd_delay_ms(10);
    }
}

/* One tone phase with an LCD tag + keylight pin (the user's report anchor). */
static void tone_tag(const char *tag, uint32_t freq_hz, uint32_t ms,
                     uint32_t mode, uint32_t keylight)
{
    diag_row(13, tag, COL_STAGE);
    led_keylight_set(keylight);
    tone_phase(freq_hz, ms, mode);
}

static void dsp_audio_tone(void)
{
    uint32_t j;

    diag_row(12, "AUDIO CHAIN", COL_STAGE);
    led_keylight_set(14);

    /* The subsystem power phase FIRST (D10 ordering — the D1-D4 gaps). */
    dsp_audio_subsys_power();
    diag_row(13, "P2-6", COL_STAGE);

    /* P7: the NCP2817BFC AMP_EN = GPIO_0 (bank 0, bit 0) — the emulator
     * captured the audio burst driving 0x8A000000 = 0 with DMSK/DIR set;
     * the playback enable = DATA high. */
    MEM4(0x8A000004) = 1;           /* DMSK  */
    MEM4(0x8A000008) = 1;           /* DIR = output */
    MEM4(0x8A000018) = 0;           /* IE off */
    MEM4(0x8A000000) |= 1;          /* DATA high = AMP_EN on (B18) */
    diag_row(13, "AMP0", COL_STAGE);

    /* The codec power ladder + the companion LDO rails. THE FIX (the
     * no-click root cause): the stock's "GPIO 31/30/29/2" (audio-hal §3)
     * are NOT 0x8A GPIOs — they are LDO ids driven on the ANA LDO bank
     * 0x82001164 (the LDO table @0xCA38C: id 0x1F/31 -> 0x10, 0x1E/30 ->
     * 0x20, 0x1D/29 -> 0x40, 0x02/2 -> 0x80, 0x1C/28 -> 0x100). Our old
     * chain wrote the 0x8A GPIO bank — the rails never powered. */
    adi_w(0x820012C0, adi_r(0x820012C0) | 0x02u);           /* BG     */
    adi_w(0x820012C0, adi_r(0x820012C0) | 0x08u);           /* IB     */
    adi_w(0x82001164, adi_r(0x82001164) | 0x100u);          /* LDO 28 */
    adi_w(0x820012C0, adi_r(0x820012C0) | 0x80u);           /* VCM    */
    adi_w(0x82001164, adi_r(0x82001164) | 0x10u);           /* LDO 31 */
    adi_w(0x820012C0, adi_r(0x820012C0) | 0x40u);           /* VCM_BUF*/
    adi_w(0x82001164, adi_r(0x82001164) | 0x20u);           /* LDO 30 */
    adi_w(0x820012C0, adi_r(0x820012C0) | 0x20u);           /* VB     */
    adi_w(0x82001164, adi_r(0x82001164) | 0x40u);           /* LDO 29 */
    adi_w(0x820012C0, adi_r(0x820012C0) | 0x10u);           /* VBO    */
    adi_w(0x82001164, adi_r(0x82001164) | 0x80u);           /* LDO 2  */
    adi_w(0x82001294, 0u);
    diag_row(13, "PWR", COL_STAGE);

    /* GATED: the IF0 (0x82001180) writes are SKIPPED. On this silicon the
     * IF0 bank stalls the AHB (the user's IF1a tag: the IF0 |= 0x10 write
     * freezes the bus — the AHB-stall class), while the IF1 (0x820011A0)
     * bank + the old IF1-only pattern always completed. The stock's pre-IF0
     * power is runtime-dispatched (FUN_00031abe's function tables — not
     * statically pinnable), so rather than risk another freeze the IF0
     * writes are gated and the IF1 writes keep the old completing pattern.
     *
     * LCD STEP LABELS (row 13 — the PRIMARY readout): each tag prints AFTER
     * its write succeeds, so the LAST tag visible = the last SUCCESSFUL
     * step; the NEXT one in the map is the staller. Map: P2-6 / AMP0 / PWR
     * / IF1a IF1b (IF1 |= 0x10 / |= 0x8000 — IF0 gated) / GRP0 GRP1 GRP08
     * LDO MUX88 MUX88b MUXA4 MUX6C / DACON DP PA VBC / TONE 440. */
    led_keylight_set(1);
    adi_w(0x820011A0, adi_r(0x820011A0) | 0x10u);
    diag_row(13, "IF1a", COL_STAGE);
    lcd_delay_ms(3);
    led_keylight_set(2);
    adi_w(0x820011A0, adi_r(0x820011A0) | 0x8000u);
    diag_row(13, "IF1b", COL_STAGE);
    lcd_delay_ms(3);

    led_keylight_set(5);
    adi_w(0x820012A0, (adi_r(0x820012A0) & ~0x10u) | 0x20u);
    diag_row(13, "GRP0", COL_STAGE);
    lcd_delay_ms(3);
    led_keylight_set(6);
    adi_w(0x820012A4, (adi_r(0x820012A4) & ~0x40u) | 0x80u);
    diag_row(13, "GRP1", COL_STAGE);
    lcd_delay_ms(3);

    led_keylight_set(7);
    adi_w(0x820012A0, (adi_r(0x820012A0) & ~4u) | 8u);
    diag_row(13, "GRP08", COL_STAGE);
    lcd_delay_ms(3);
    led_keylight_set(8);
    adi_w(0x82001014, (adi_r(0x82001014) & 0xBFFFu) | 0x4000u);
    diag_row(13, "LDO", COL_STAGE);
    lcd_delay_ms(3);

    led_keylight_set(9);
    adi_w(0x82001288, adi_r(0x82001288) | 0x400u);
    diag_row(13, "MUX88", COL_STAGE);
    lcd_delay_ms(3);
    led_keylight_set(10);
    adi_w(0x82001288, adi_r(0x82001288) & 0xFFFDu);
    diag_row(13, "MUX88b", COL_STAGE);
    lcd_delay_ms(3);
    led_keylight_set(11);
    adi_w(0x820012A4, adi_r(0x820012A4) | 0x4000u);
    diag_row(13, "MUXA4", COL_STAGE);
    lcd_delay_ms(3);
    led_keylight_set(12);
    adi_w(0x8200116C, adi_r(0x8200116C) | 0x400u);
    diag_row(13, "MUX6C", COL_STAGE);
    lcd_delay_ms(3);

    /* THE 2026-08-28 EMULATOR-KEEPALIVE FINDS (stock-sound.log periodic
     * cycle, ~every 4.4k lines): the stock's audio driver writes THREE
     * codec-IF values our chain never touches:
     *   0x820011C0 = 0x19  (bits 0,3,4 — an unlisted codec IF register)
     *   0x82001180 = 0x0   (IF0 cleared — GATED on HW: IF0 |= 0x10 stalls
     *                       the AHB on this silicon; the clear is left out)
     *   0x820011A0 = 0x2   (IF1 bit 1 — we only ever set bits 4/15)
     * The SC6531EFM map calls +0x1C0 a clock-test register; on the SC6530C
     * it is written 0x19 by the audio driver every cycle — a candidate for
     * the missing audio-IF clock enable. IF1 |= 0x2 completes the stock's
     * IF1 state (open bits 4/15 + keepalive bit 1). */
    led_keylight_set(14);
    adi_w(0x820011C0, adi_r(0x820011C0) | 0x19u);
    diag_row(13, "IFC0", COL_STAGE);
    lcd_delay_ms(3);
    adi_w(0x820011A0, adi_r(0x820011A0) | 0x2u);
    diag_row(13, "IF1+2", COL_STAGE);
    lcd_delay_ms(3);

    led_keylight_set(13);       /* the chain is done — the tone is next */

    adi_w(0x8200132C, adi_r(0x8200132C) | 0x18u);       lcd_delay_ms(3);
    adi_w(0x82001280, adi_r(0x82001280) | 0x05u);       lcd_delay_ms(3);
    adi_w(0x820012F4, adi_r(0x820012F4) | 0xC0u);       lcd_delay_ms(3);
    adi_w(0x820012FC, adi_r(0x820012FC) | 0x84u);       lcd_delay_ms(3);
    adi_w(0x82001304, adi_r(0x82001304) | 0xD0u);       lcd_delay_ms(3);
    adi_w(0x82001314, 0x44u);                           lcd_delay_ms(3);
    adi_w(0x82001318, 0x40u);                           lcd_delay_ms(3);
    adi_w(0x8200131C, 0x44u);                           lcd_delay_ms(3);
    diag_row(13, "DACON", COL_STAGE);

    /* DP DAC (0x8A002000). THE 2026-08-28 FIXES (dump-verified):
     *   - bit14 (0x4000) = the DP DAC POWER/ENABLE — the stock's DAC-on
     *     FUN_0008131e(1) sets it; we NEVER wrote it (only FS + unmute +
     *     DAC_EN_L/R) — a silent DP stage with no enable.
     *   - FS mode: the 0x8123E table maps the DAC FS to a RATE — mode 10
     *     (0xA) = 8 kHz (the default 0x1F40=8000), mode 9 = 9600 Hz
     *     (0x2580). The old `fs=9` was a misread (audio-hal.md §3) — a
     *     9600 Hz config on an 8 kHz VBC stream = FS mismatch = silence.
     *     Phase T4 re-tests fs=9 to isolate it. */
    MEM4(0x8A00200C) = (MEM4(0x8A00200C) & ~0xFu) | 0xAu;     /* fs 8k (10) */
    MEM4(0x8A00200C) = MEM4(0x8A00200C) & ~(1u << 15);        /* unmute     */
    MEM4(0x8A00200C) = MEM4(0x8A00200C) | 0x4000u;            /* DP DAC EN  */
    MEM4(0x8A002000) = MEM4(0x8A002000) | 0x05u;              /* DAC_EN L/R */
    diag_row(13, "DP", COL_STAGE);

    MEM4(0x8B000060) = 0x200000u;                       /* PA power b21 */
    MEM4(0x8B000064) = 0x200000u;
    adi_w(0x82001450, 1u);
    adi_w(0x82001454, 1u);
    MEM4(0x8B0000A0) = 0x10000000u;                     /* PA power b28 */
    MEM4(0x8B0000A4) = 0x10000000u;
    adi_w(0x82001440, 4u);
    adi_w(0x82001444, 4u);

    MEM4(0x8B0001C4) = MEM4(0x8B0001C4) | 0x604u;       /* ARM-owns 0x604
                                                           (D5: b9|b10 codec
                                                           + b2 VBC)   */
    adi_w(0x82001290, adi_r(0x82001290) & ~1u);
    diag_row(13, "PA", COL_STAGE);

    /* VBC power half b18 (D8) — the device gate for the sound data path.
     * The stock pulse (doc §2 0x0B080C): SET 1<<18 -> delay(10) -> CLR. */
    MEM4(0x8B000060) = MEM4(0x8B000060) | 0x40000u;
    lcd_delay_ms(10);
    MEM4(0x8B000064) = MEM4(0x8B000064) | 0x40000u;

    /* The VBC ping-pong init (vbc_phy_v5.c __vbc_clear_da_buffer +
     * __vbc_set_buffer_size). THE key missing pieces:
     *   - the DA buffer size lives at VBBUFFERSIZE bits 8-15 (the old
     *     code wrote bits 0-7 = the AD size -> DA size 0 -> nothing plays);
     *   - the VBRAMSW_EN/NUMBER ping-pong dance clears both DA buffers
     *     (VBENABLE is LOW here — the legal MCU-buffer window).
     *   - the DA DMA-EN bits b13/b14 are NOT set here — the tone phases
     *     toggle them per feed mode (VBC_MODE_DMAEN). */
    MEM4(0x8B0001C4) = MEM4(0x8B0001C4) | 0x100u;       /* ARM_VB_ANAON b8 */
    MEM4(0x82003018) = MEM4(0x82003018) | 0x400u;       /* VBRAMSW_EN b10 */
    MEM4(0x82003010) = (MEM4(0x82003010) & ~0xFFFFu) | 0x9F9Fu; /* DA+AD
                                                           sizes 0x9F9F
                                                           (160-1 both) */
    MEM4(0x82003018) = MEM4(0x82003018) | 0x200u;       /* VBRAMSW_NUM=1  */
    for (j = 0; j < 160u; j++) { MEM4(0x82003000) = 0; MEM4(0x82003004) = 0; }
    MEM4(0x82003018) = MEM4(0x82003018) & ~0x200u;      /* VBRAMSW_NUM=0  */
    for (j = 0; j < 160u; j++) { MEM4(0x82003000) = 0; MEM4(0x82003004) = 0; }
    MEM4(0x82003018) = MEM4(0x82003018) & ~0x400u;      /* VBRAMSW_EN=0   */
    diag_row(13, "VBC", COL_STAGE);

    /* THE 2026-08-28 DECISIVE TONE ROUND. The codec chain is open + the DP
     * DAC enable (bit14) + fs=10 (8 kHz) are live. Five phases vary the TWO
     * remaining unknowns — the VBC FEED MODE and the AMP_EN GPIO — each with
     * a DISTINCT frequency so ONE audible tone identifies the working combo:
     *   T1 440 Hz  DIRECT  (DMA-EN off, VBRAMSW off, raw VBDAL/VBDAR)
     *   T2 660 Hz  VRAMSW  (stop-start: fill low -> VBENABLE high)
     *   T3 880 Hz  DMAEN   (VBC DMA-request mode + direct writes)
     *   T4 990 Hz  DIRECT  with fs=9 (isolates the FS-mode fix)
     *   T5 550 Hz  DIRECT  with the GPIO-39 amp (second AMP_EN candidate)
     * The tag on row 13 shows the CURRENT phase; the user reports which
     * frequencies sound. */
    aud_gpio_set(18, 8, 1);         /* DIR output */
    aud_gpio_set(18, 0, 1);         /* DATA high  (amp 18 on) */

    tone_tag("T1 440",  440u, 1000u, VBC_MODE_DIRECT, 3);
    {
        char t[22];
        t[0] = 'V'; t[1] = 'B'; t[2] = 'C'; t[3] = '=';
        hx4(t + 4, (uint16_t)MEM4(0x82003018));
        t[8] = '\0';
        diag_row(13, t, COL_STAGE);
    }
    tone_tag("T2 660",  660u, 1000u, VBC_MODE_VRAMSW, 4);
    tone_tag("T3 880",  880u, 1000u, VBC_MODE_DMAEN,  5);

    MEM4(0x8A00200C) = (MEM4(0x8A00200C) & ~0xFu) | 0x9u;  /* fs=9 probe */
    tone_tag("T4 990",  990u, 1000u, VBC_MODE_DIRECT, 6);
    MEM4(0x8A00200C) = (MEM4(0x8A00200C) & ~0xFu) | 0xAu;  /* fs=10 back */

    aud_gpio_set(18, 0, 0);         /* GPIO 18 off */
    aud_gpio_set(39, 8, 1);         /* DIR output */
    aud_gpio_set(39, 0, 1);         /* DATA high  (amp 39 on) */
    tone_tag("T5 550",  550u, 1000u, VBC_MODE_DIRECT, 7);
    aud_gpio_set(39, 0, 0);         /* GPIO 39 off */

    MEM4(0x8A000180) &= ~(1u << 15);/* GPIO_63 off */
    MEM4(0x8A000000) &= ~1u;        /* GPIO_0 off */

    MEM4(0x82003018) = MEM4(0x82003018) & ~0x8000u;     /* VBENABLE off */
    MEM4(0x8B0001C4) = MEM4(0x8B0001C4) & ~(0x604u | 0x100u);
    diag_row(13, "TONE DONE", COL_OK);
}

void main(void)
{
    const uint8_t *prog = _binary_build_dsp_seg0_bin_start;
    uint32_t prog_hw = DSP_PROG_HW;
    int stage;
    int alive;

    usb_debug_init();   /* no-op under SD_BOOT_NO_USB */

    dsp_init_iram();
    dsp_init_freq();
    dsp_init_smc();
    dsp_init_adi();
    dsp_init_power();
    led_keylight_set(1);
    lcd_init();
    lcd_fill(COL_BG);
    diag_row(0, "DSP BOOT DIAG", COL_TITLE);
    {
        char t[22];
        t[0] = 'p'; t[1] = 'r'; t[2] = 'o'; t[3] = 'g'; t[4] = ' ';
        hx4(t + 5, (uint16_t)prog_hw);
        t[9] = 'w'; t[10] = ' '; t[11] = '0'; t[12] = 'x';
        hx8(t + 13, prog_hw << 1);
        t[21] = '\0';
        diag_row(1, t, COL_STAGE);
    }
    led_keylight_set(3);

    stage = dsp_download_boot(prog, prog_hw);
    if (stage != 0) {
        led_keylight_set(10);
        lcd_print_small(0, 136, "DSP FAIL", COL_ERR, COL_BG);
        lcd_show_bounded();
        for (;;) ;
    }

    alive = dsp_status_probe();
    if (alive == 2) {
        led_keylight_set(15);
        lcd_print_small(0, 136, "DSP BOOTED", COL_OK, COL_BG);
        lcd_print_small(0, 152, "resp seen", COL_OK, COL_BG);
        lcd_show_bounded();
    } else if (alive == 1) {
        led_keylight_set(12);
        lcd_print_small(0, 136, "DSP RUNNING?", COL_STAGE, COL_BG);
        lcd_print_small(0, 152, "no msg resp", COL_STAGE, COL_BG);
        lcd_show_bounded();
    } else {
        led_keylight_set(10);
        lcd_print_small(0, 136, "DSP FAIL 07", COL_ERR, COL_BG);
        lcd_print_small(0, 152, "no activity", COL_ERR, COL_BG);
        lcd_show_bounded();
    }

    /* 5B2: with the DSP up, bring the codec + VBC DA up (audio.c's safe
     * chain) and play a sustained 440 Hz tone. The user LISTENS — any
     * audible signal from the speaker/earpiece is the goal. The DSP's
     * running init may now configure the audio data path the ARM could
     * not reach before. */
    dsp_audio_tone();
    led_keylight_set(15);
    lcd_print_small(0, 136, "TONE DONE", COL_OK, COL_BG);
    lcd_print_small(0, 152, "listen 440", COL_OK, COL_BG);
    lcd_show_bounded();

    for (;;) ;
}
