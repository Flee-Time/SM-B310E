/*
 * B310E-OS — arch/diag_sd_mmu_main.c
 *
 * SD CARD DIAGNOSTIC WITH THE MMU ON — the decisive environment bisect.
 *
 * Context: os-diag-sd.bin runs the SD probe with the MMU OFF (diag-sd,
 * works: "MBR OK"); os.bin runs it with the MMU ON + D-cache live + the
 * scheduler + the 1 ms tick and DIES at sdio_init stage 5 (SDCLK_EN) —
 * even with the tick IRQ masked around the probe and a counter-free busy
 * delay. This image replicates os.bin's EXACT pre-probe init state:
 *   chip init -> enable_dcache -> module_register+init_all (USB/LED/LCD/
 *   keypad — lights the backlight + keylight, installs the kputc channel)
 *   -> irq_init() (MMU on, high vectors)
 * but runs the SD probe DIRECTLY in main() — NO scheduler, NO tasks,
 * NO tick. Result bisects the killer:
 *   - SD probe works here  -> the scheduler/task/tick context kills it
 *   - SD probe dies stage 5 -> the MMU/D-cache/prior-peripheral state
 *     kills it (next suspect: the identity-map page permissions for the
 *     SDIO 0x20700000 region, or a D-cache/SDIO interaction).
 *
 * SAFETY: identical to diag-sd (bounded waits, no 0x8c0002a4, the only
 * 0x8c writes are the SDIO pinmux 0x8c000250-264 under test).
 */

#include "os.h"
#include "chip.h"
#include "../drivers/usb_debug.h"
#include "../drivers/led.h"
#include "../drivers/lcd.h"
#include "../drivers/keypad.h"
#include "../drivers/sdio.h"

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

    /* 1. fdl_ack FIRST (unblocks spd_dump). */
    usb_debug_init();

    /* 2. FULL chip init (same as os.bin / diag-sd). */
    sd_init_iram();
    sd_init_freq();
    sd_init_smc();
    sd_init_adi();
    sd_init_power();

    /* 3. D-cache enable (os.bin does this right after chip init). */
    enable_dcache();

    /* 4. Modules — replicates os.bin's prior peripheral state: USB kputc
     *    live, LCD up (backlight), keypad (EIC) configured, keylight on. */
    module_register(&usb_debug_module);
    module_register(&led_module);
    module_register(&lcd_module);
    module_register(&keypad_module);
    if (module_init_all() != 0)
        kprintf("diag-sdmmu: module init FAILURES\n");

    /* 5. IRQ infrastructure + MMU — the os.bin state (high vectors, MMU
     *    on, D-cache now live via the M bit). NO sched_start: no tick. */
    irq_init();

    /* 6. LCD banner. */
    lcd_fill(0x001f);
    lcd_print(4, 20, "DIAG:SDMMU", 0xffff, 0x001f);
    lcd_show_bounded();

    /* 7. The SD probe — direct in main(), no scheduler. */
    kprintf("diag-sdmmu: sdio_init\n");
    sdio_init();
    kprintf("diag-sdmmu: sdcard_init\n");
    rc = sdcard_init();
    if (rc != 0) {
        kprintf("diag-sdmmu: sdcard_init FAILED (%d)\n", rc);
        lcd_print(4, 36, "SD FAIL", 0xf800, 0x001f);
        lcd_show_bounded();
        for (;;) ;
    }
    kprintf("diag-sdmmu: card ok, shl=%u\n", (unsigned)sdio_shl);

    rc = sdio_read_block(0, s_mbr);
    sig = (uint32_t)s_mbr[510] | (uint32_t)s_mbr[511] << 8;
    if (rc == 0 && sig == 0xaa55) {
        kprintf("diag-sdmmu: MBR OK (sig 0xaa55)\n");
        lcd_print(4, 36, "SD OK MBR", 0x07e0, 0x001f);
    } else {
        kprintf("diag-sdmmu: MBR read rc=%d sig=0x%04x\n", rc, (unsigned)sig);
        lcd_print(4, 36, "SD MBR?", 0xffe0, 0x001f);
    }
    lcd_show_bounded();

    for (;;) ;
}
