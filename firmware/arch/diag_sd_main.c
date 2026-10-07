/*
 * B310E-OS — arch/diag_sd_main.c
 *
 * SD CARD DIAGNOSTIC boot image (`make diag-sd` -> os-diag-sd.bin).
 * Validates the SD/SDIO path on real hardware — the FIRST boot image that
 * touches the SDIO controller (0x20700000) and the SDIO pinmux registers
 * 0x8c000250-0x264.
 *
 * Why this exists: the SDIO pinmux writes are the ONLY 0x8cxxxxxx writes
 * in the tree. The AGENTS.md rule "never write 0x8c0002a4" exists because
 * the UART-TX pinmux hangs the B310E; the SDIO regs (0x250-0x268) are a
 * DIFFERENT set that fpdoom's sdboot uses on this exact phone, but they
 * are live pin config and MUST be proven before the 4-bit/high-speed path
 * is trusted. This image runs the full production path (chip init -> LCD
 * banner -> USB) and then sdio_init -> sdcard_init -> reads sector 0
 * (the MBR) -> displays the result. A successful MBR read proves the
 * whole SD stack; failure pinpoints controller vs card vs pinmux.
 *
 * Sequence:
 *   1. usb_debug_init()   — fdl_ack first, unblocks spd_dump
 *   2. sc6530_chip_init() — FULL chip init (SDIO needs ADI + power gates;
 *      the ADI mailbox that drives the SD power LDOs is only alive after
 *      sc6530_init_adi)
 *   3. lcd_init()         — banner on screen ("DIAG:SD")
 *   4. sdio_init()        — controller power/reset/clock/pinmux/LDO
 *   5. sdcard_init()      — CMD0/8/55+ACMD41/2/3/9/7, 4-bit, blocklen
 *   6. sdio_read_block(0) — read the MBR sector
 *   7. Show on LCD + USB: "SD OK" + MBR signature (0xaa55 at +510),
 *      or the exact failure stage
 *
 * SAFETY: no 0x8c0002a4; the 0x8c000250-264 pinmux writes are the thing
 * under test. Bounded waits only (the SD command engine polls with
 * timeouts; a missing card returns a failure instead of hanging).
 */

#include "os.h"
#include "chip.h"
#include "../drivers/usb_debug.h"
#include "../drivers/lcd.h"
#include "../drivers/sdio.h"

#define DIAG_SD_DWELL_MS 1500u

/* ---- chip init (verbatim from arch/main.c — main.o is excluded from
 * diag links, so this image carries its own copy; same pattern the old
 * diag_main.c used). ------------------------------------------------------ */

#define SMC_INIT_BUF (0x40000000u + 0xa000u - 1024u)   /* 0x40009c00 */

extern int sc6530_init_smc_asm[1];
extern int sc6530_init_smc_asm_end[1];

static void sd_init_freq(void)
{
    uint32_t a = MEM4(0x8b000040);

    MEM4(0x8b000040) = a |= 4;
    MEM4(0x8b000040) = (a & ~3) | 1;
    DELAY(100);
}

static void sd_init_smc(void)
{
    int *p = sc6530_init_smc_asm;
    int n = sc6530_init_smc_asm_end - p;
    int *d = (int *)SMC_INIT_BUF, *f = d;

    do *d++ = *p++; while (--n);
    ((void (*)(void))(uintptr_t)f)();
}

static void sd_init_adi(void)
{
    MEM4(0x8b0000a0) = 1 << 24;
    MEM4(0x8b000060) = 1 << 19;
    DELAY(100);
    MEM4(0x8b000064) = 1 << 19;
    MEM4(0x82000000) &= ~(1 << 4);
    MEM4(0x82000004) = 0x55000;
}

static void sd_init_iram(void)
{
    MEM4(0x8b0001a0) |= 7 << 19;
}

static void sd_init_power(void)
{
    MEM4(0x20500060) |= 0x40;          /* LCM   */
    MEM4(0x20500060) |= 0x1000;        /* LCDC  */
    MEM4(0x20500060) |= 0x400;         /* SDIO0 */
    MEM4(0x8b0000a0) |= 0x80040;       /* keypad */
    MEM4(0x8b0000a0) |= 0x800080;      /* GPIO_D */
}

void main(void)
{
    static uint8_t s_mbr[512] __attribute__((aligned(32)));
    uint32_t sig;
    int rc;

    /* 1. fdl_ack FIRST (unblocks spd_dump before anything risky). */
    usb_debug_init();

    /* 2. FULL chip init — SDIO needs the ADI mailbox (SD power LDOs) and
     *    the AHB power gates, so this is NOT the minimal diag-s0 path. */
    sd_init_iram();
    sd_init_freq();
    sd_init_smc();
    sd_init_adi();
    sd_init_power();

    /* 3. LCD banner so the user sees the image is alive. */
    lcd_init();
    lcd_fill(0x001f);
    lcd_print(4, 20, "DIAG:SD", 0xffff, 0x001f);
    lcd_show_bounded();

    /* 4. SD controller + card init. */
    kprintf("diag-sd: sdio_init\n");
    sdio_init();
    kprintf("diag-sd: sdcard_init\n");
    rc = sdcard_init();
    if (rc != 0) {
        /* rc = failing stage (1 CMD8 echo, 2 no card, 3 CID, 4 RCA,
         * 5 CSD, 6 select, 7 4-bit, 8 blocklen) — see sdio.c */
        kprintf("diag-sd: sdcard_init FAILED (%d)\n", rc);
        lcd_print(4, 36, "SD FAIL", 0xf800, 0x001f);
        lcd_show_bounded();
        for (;;) ;
    }
    kprintf("diag-sd: card ok, shl=%u — reading MBR\n", (unsigned)sdio_shl);

    /* 5. Read sector 0 (MBR). */
    rc = sdio_read_block(0, s_mbr);
    sig = (uint32_t)s_mbr[510] | (uint32_t)s_mbr[511] << 8;

    if (rc == 0 && sig == 0xaa55) {
        kprintf("diag-sd: MBR OK (sig 0xaa55)\n");
        lcd_print(4, 36, "SD OK MBR", 0x07e0, 0x001f);
        lcd_print(4, 52, "sig 0xaa55", 0xffff, 0x001f);
    } else {
        kprintf("diag-sd: MBR read rc=%d sig=0x%04x (no FAT32?)\n",
                rc, (unsigned)sig);
        lcd_print(4, 36, "SD MBR?", 0xffe0, 0x001f);
    }
    lcd_show_bounded();

    /* 6. Halt — panel keeps the last frame. */
    for (;;) ;
}
