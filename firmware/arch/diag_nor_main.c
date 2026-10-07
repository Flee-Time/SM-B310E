/* B310E-OS — arch/diag_nor_main.c
 *
 * NOR-BOOT DIAGNOSTIC (`make diag-nor` -> os-diag-nor.bin). The FIRST
 * test that proves whether ANY of our code executes from the NOR boot
 * vector at power-on (2026-08-23: the merged menu's NOR install showed
 * "doesn't power on" — nothing had ever been NOR-booted before, all prior
 * verification ran post-FDL1).
 *
 * The image uses the MERGED boot stub (arch/menu_boot.s compiled with
 * -DMENU_NO_KEYCHECK, so it skips the keypad probe and the stock chain —
 * it just brings up the PSRAM window, copies itself to 0x34000000 and
 * jumps to _start -> main()). main() here runs the PROVEN chip init
 * (same sequence os.bin and the menu run on every USB boot) and then
 * lights the KEYPAD (led_keylight_set) and HALTS. Visible output:
 *
 *   keypad light ON  = NOR boot + stub remap/copy + chip init ALL work.
 *   nothing          = the stub or chip init hangs at power-on; bisect
 *                      by moving the keylight earlier in the sequence
 *                      (bare registers, no chip init) to find the hang.
 *
 * Layout mirrors the diag images: `arch/diag_nor_main.o` replaces
 * arch/main.o in the link (arch/main.o is excluded from diag links).
 */

#include "os.h"
#include "chip.h"
#include "../drivers/led.h"

/* ---- chip init (verbatim from arch/main.c — main.o is excluded from
 * diag links, so this image carries its own copy). ---------------- */

#define SMC_INIT_BUF (0x40000000u + 0xa000u - 1024u)   /* 0x40009c00 */

extern int sc6530_init_smc_asm[1];
extern int sc6530_init_smc_asm_end[1];

static void nor_init_iram(void)
{
    MEM4(0x8b0001a0) |= 7 << 19;
}

static void nor_init_freq(void)
{
    uint32_t a = MEM4(0x8b000040);

    MEM4(0x8b000040) = a |= 4;
    MEM4(0x8b000040) = (a & ~3) | 1;
    DELAY(100);
}

static void nor_init_smc(void)
{
    int *p = sc6530_init_smc_asm;
    int n = sc6530_init_smc_asm_end - p;
    int *d = (int *)SMC_INIT_BUF, *f = d;

    do *d++ = *p++; while (--n);
    ((void (*)(void))(uintptr_t)f)();
}

static void nor_init_adi(void)
{
    MEM4(0x8b0000a0) = 1 << 24;
    MEM4(0x8b000060) = 1 << 19;
    DELAY(100);
    MEM4(0x8b000064) = 1 << 19;
    MEM4(0x82000000) &= ~(1 << 4);
    MEM4(0x82000004) = 0x55000;
}

static void nor_init_power(void)
{
    MEM4(0x20500060) |= 0x40;          /* LCM   */
    MEM4(0x20500060) |= 0x1000;        /* LCDC  */
    MEM4(0x20500060) |= 0x400;         /* SDIO0 */
    MEM4(0x8b0000a0) |= 0x80040;       /* keypad */
    MEM4(0x8b0000a0) |= 0x800080;      /* GPIO_D */
}

void main(void)
{
    /* The boot stub already did the PSRAM window bring-up (MEM_REMAP +
     * SMC) and copied the image to 0x34000000. This is the full chip
     * init the menu runs on a USB boot — proven on every os.bin boot. */
    nor_init_iram();
    nor_init_freq();
    nor_init_smc();
    nor_init_adi();
    nor_init_power();

    /* Visible "we are alive" marker: keypad light ON (the proven RMW
     * pattern, HW-verified). The phone should glow under the keys. */
    led_keylight_set(15);

    for (;;) ;
}
