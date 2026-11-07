/* B310E OS — watchdog, reboot, fault screen.
 *
 * Watchdog in the analog die at 0x82001480: LOAD_LOW +0, LOAD_HIGH +4,
 * CTRL +8 (bit 3 reset enable, bit 0 interrupt, bit 1 run), LOCK +0x20
 * (0xe551 unlocks). 32 kHz. The reboot sequence is the one the Rockbox
 * port verified on the phone (system_reboot): it restarts the boot chain,
 * which brings back the SD menu.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/arch.h"
#include "os/drivers.h"
#include "os/lib.h"

#define WDG       0x82001480u
#define WDG_LOCK  (WDG + 0x20)
#define WDG_KEY   0xe551u

void watchdog_stop(void)
{
    adi_write(WDG_LOCK, WDG_KEY);
    adi_write(WDG + 8, adi_read(WDG + 8) & ~0xfu);
    adi_write(WDG_LOCK, ~WDG_KEY);
}

#define CHAIN_IRAM 0x4000f800u          /* above sd.c's bounce buffer */
#define CHAIN_TO   0x04000000u          /* where sdboot puts the menu, the menu us */

/* Run another image as if loaded fresh: the boot menu, or this OS again.
 * The image must lie above the copy's end (the heap does). */
void arch_chain(const void *image, uint32_t len)
{
    extern char chain_tramp[], chain_tramp_end[];
    void (*tramp)(const void *, uint32_t, uint32_t) = (void (*)(const void *, uint32_t, uint32_t))CHAIN_IRAM;

    arch_irq_save();
    REG32(INTC_DISABLE) = 0xffffffffu;
    dma_quiesce();
    memcpy((void *)CHAIN_IRAM, chain_tramp, (size_t)(chain_tramp_end - chain_tramp));
    dcache_clean_range((void *)CHAIN_IRAM, (size_t)(chain_tramp_end - chain_tramp));
    icache_invalidate_all();
    tramp(image, (len + 3) & ~3u, CHAIN_TO);
    for (;;)
        ;
}

bool arch_chain_fits(const void *image, uint32_t len)
{
    return (uintptr_t)image >= CHAIN_TO + len;
}

void arch_reboot(void)
{
    arch_irq_save();
    REG32(INTC_DISABLE) = 0xffffffffu;
    adi_write(0x820010e0, 4);           /* watchdog clocks */
    adi_write(0x820010e4, 2);
    adi_write(WDG_LOCK, WDG_KEY);
    adi_write(WDG + 8, adi_read(WDG + 8) | 9);
    adi_write(WDG, 0x4000);             /* 0.5 s */
    adi_write(WDG + 4, 0);
    adi_write(WDG + 8, adi_read(WDG + 8) | 2);
    adi_write(WDG_LOCK, ~WDG_KEY);
    for (;;)
        arch_wait_for_irq();
}

/* Cut the supplies (stock 0x1a448: SET1 = 0x1f, then SET0 = 0x3fff; the
 * last store removes the CPU and RAM supplies). The LEDs and vibrator
 * live in the analog die, which runs from the battery and keeps its
 * state: turn them off first, or the keypad light stays lit (found with
 * Rockbox). Callers flush the filesystem before. */
#define LDO_PD_SET0  0x82001180u
#define LDO_PD_SET1  0x82001184u

void arch_power_off(void)
{
    uint32_t n = 1000000;

    keylight_set(false);
    backlight_set(0);
    vibrator_set(0);
    torch_set(false);
    arch_irq_save();
    REG32(INTC_DISABLE) = 0xffffffffu;
    if (!adi_write(LDO_PD_SET1, 0x1f))
        arch_reboot();
    while ((REG32(0x82000020) & (1u << 9)) && --n)
        ;
    if (!n)
        arch_reboot();
    REG32(LDO_PD_SET0) = 0x3fff;
    for (;;)
        arch_wait_for_irq();
}

/* Undefined instruction, aborts, FIQ: show the registers and stop. */
void arch_fault(int kind, const uint32_t *regs, uint32_t spsr)
{
    static const char *const names[] = {
        "undefined instr", "prefetch abort", "data abort", "fiq"
    };
    extern bool console_unlocked;
    static bool in_fault;
    int i;

    if (in_fault)                       /* a fault while reporting one */
        for (;;)
            ;
    in_fault = true;
    console_unlocked = true;            /* no kernel locks from here */
    console_set_color(RGB(255, 255, 255), RGB(160, 0, 0));
    console_clear();
    console_printf("FAULT: %s\n", names[kind & 3]);
    console_printf("pc %08lx cpsr %08lx\n", (unsigned long)regs[13],
                   (unsigned long)spsr);
    for (i = 0; i < 13; i += 2)
        console_printf("r%-2d %08lx r%-2d %08lx\n", i,
                       (unsigned long)regs[i], i + 1,
                       (unsigned long)(i + 1 < 13 ? regs[i + 1] : 0));
    console_printf("\nHold END to reboot");
    console_flush();
    for (;;) {
        if ((adi_read(0x82001900) >> 3) & 1)   /* END, EIC channel 3 */
            arch_reboot();
        udelay(20000);
    }
}
