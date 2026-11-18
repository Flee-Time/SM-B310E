/* B310E OS — the CPU clock: 208, 104 or 26 MHz.
 *
 * 0x8b000040: bits 1-0 select the ARM clock (0 the 26 MHz crystal, 1 208,
 * 2 104, 3 156 MHz), bit 2 halves the AHB clock. From the stock firmware:
 * the clock-by-name functions (NOR 0x665a0 source, 0x66606 divider), the
 * frequency getter (0x1a384, 0x1a3a8: the same table the LCD driver reads)
 * and the boot switch to 208 MHz (0x33ca4: divider first, then source,
 * a short wait after each write).
 *
 * The boot menu leaves 208 MHz with the AHB at 104 MHz. The levels keep
 * the AHB at 104 MHz except at 26 MHz, so at 104 and 208 every peripheral
 * (LCD timing, SDIO, ADI) runs exactly as at boot:
 *
 *     level 0   26 MHz   crystal, AHB 26 MHz   only with the panel asleep
 *     level 1  104 MHz   AHB 104 MHz           and no SD transfer
 *     level 2  208 MHz   AHB 104 MHz (half)
 *
 * During a switch the AHB never goes above 104 MHz: going up the divider
 * is set before the source, going down the source changes first. The
 * 26 MHz tick timer is not affected. udelay() scales with the clock.
 *
 * Bit 0 of 0x8b000020 is the PLL's enable; the status word 0x8b000120 shows bit 3
 * (PLL down) within a read of clearing it (phone, 2026-11-09). The ARM and the
 * timers run without it at 26 MHz, but the analog die's bus does not: reads
 * return 0. So only the idle loop stops it, just before sleeping (sleep_idle),
 * and clock_pll_need() starts it again: on the way up here, for anything holding
 * the clock up (SD, USB, the codec, the DSP, the SIM), and in every ADI access.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/arch.h"

#define CLK_CTL     0x8b000040u
#define CLK_SEL     3u
#define CLK_HALF    4u
#define PLL_CTL     0x8b000020u
#define PLL_EN      1u
/* No flag says the PLL is locked after a software enable. The status word
 * 0x8b000120 is the sleep controller's (APB_SLP_STS0): its bit 9, the PLL's
 * count done, reports that controller's own wake only and never set after an
 * enable here (phone; stock's code agrees). Stock never stops the PLL in
 * software: only the sleep controller drops it in a deep sleep.
 *
 * The hardware holds the PLL's clock back itself after the enable: 320
 * slow-clock cycles, 9.77 ms on the phone. An analog read right after the
 * enable comes back right but only 9.83 ms later, at any delay from 0 to
 * 2 ms, and the switch up onto the PLL stalls the same way (Sleep test,
 * 2026-11-18). The
 * 2 ms waited here before then only moved part of the stall into a counted
 * wait: the rest, interrupts off and the CPU stalled, was lost from the
 * kernel's time (8.9 ms a restart). So the wait covers the gate, plus a
 * margin.
 *
 * The gate is not the PLL_WAIT field (0x8b000024 [9:0], 320 at reset, never
 * written by stock): written to 64 (the SC6530 SDK's figure, read back so),
 * and XTL_WAIT (0x8b000000 [9:0], also 320) to 64 as well, each left the gate
 * at 9.76 ms, every read right (Sleep test, 2026-11-18). A wait taken from
 * the register then ended 7.7 ms early and the switch up stalled for the
 * rest, so the wait is the measured figure, whatever the register holds. */
#define PLL_GATE_US    9766u            /* 320 cycles of 32768 Hz */
#define PLL_MARGIN_US  100u

static const struct {
    uint8_t sel, half, shift;           /* shift: udelay loops per us, log2 */
    uint16_t mhz;
} levels[CLOCK_LEVELS] = {
    { 0, 0, 3, 26 },                    /* 8 loops/us (6.5 needed) */
    { 2, 0, 5, 104 },                   /* 32 (26) */
    { 1, 1, 6, 208 },                   /* 64 (52) */
};

uint32_t udelay_shift = 6;
static int level = -1;                  /* -1: unknown boot clock, fixed */
static uint32_t pll_starts;
static uint64_t level_cycles[CLOCK_LEVELS];
static uint32_t level_mark;

static void settle(void)
{
    for (volatile int i = 0; i < 100; i++)
        ;
}

static void write_ctl(uint32_t v)
{
    REG32(CLK_CTL) = v;
    settle();
}

/* One step up or down, interrupts off. */
static void step(int to)
{
    uint32_t v = REG32(CLK_CTL);

    if (to > level) {
        clock_pll_need();
        if (levels[to].half)
            write_ctl(v |= CLK_HALF);
        write_ctl(v = (v & ~CLK_SEL) | levels[to].sel);
    } else {
        write_ctl(v = (v & ~CLK_SEL) | levels[to].sel);
        if (!levels[to].half)
            write_ctl(v & ~CLK_HALF);
    }
    level = to;
    udelay_shift = levels[to].shift;
}

/* On the crystal only. */
void clock_pll_stop(void)
{
    if (level == 0)
        REG32(PLL_CTL) &= ~PLL_EN;
}

uint32_t clock_pll_lock_us(void)
{
    return PLL_GATE_US + PLL_MARGIN_US;
}

/* Interrupts off: back, and given out by the hardware (dark idle: about 0.1
 * restarts a second on the phone, 2026-11-18). Timed on timer 2, which keeps
 * the kernel's time across it: udelay's loops are a power of two a us, 8 at
 * 26 MHz where 6.5 are needed, so its 2 ms were about 2.46 ms. */
void clock_pll_need(void)
{
    if (!(REG32(PLL_CTL) & PLL_EN)) {
        REG32(PLL_CTL) |= PLL_EN;
        pll_starts++;
        timer_wait_us(clock_pll_lock_us());
    }
}

uint32_t clock_pll_starts(void)
{
    return pll_starts;
}

void clock_init(void)
{
    uint32_t v = REG32(CLK_CTL);

    level = -1;
    for (int i = 0; i < CLOCK_LEVELS; i++)
        if ((v & CLK_SEL) == levels[i].sel && !!(v & CLK_HALF) == levels[i].half)
            level = i;
    if (level >= 0)
        udelay_shift = levels[level].shift;
}

bool clock_scalable(void)
{
    return level >= 0;
}

int clock_level(void)
{
    return level;
}

int clock_mhz(void)
{
    static const uint16_t sel_mhz[4] = { 26, 208, 104, 156 };
    uint32_t v = REG32(CLK_CTL);

    return level >= 0 ? levels[level].mhz : sel_mhz[v & CLK_SEL];
}

int clock_level_mhz(int l)
{
    return levels[l].mhz;
}

/* Time at each speed, for the drain log. The governor's window calls this
 * often enough that the 32-bit cycle count cannot wrap between calls. */
void clock_account(void)
{
    uint32_t now = timer_cycles(), d = now - level_mark;

    /* A read a few cycles behind the last one is not 2^32 cycles (165 s)
     * later: with music in bursts (many level changes a second) a 540 s
     * bench once summed 2853 s of levels (phone, 2026-11-18). Real gaps are
     * far below 2^31 cycles (the tickless cap is 60 s). */
    if ((int32_t)d < 0)
        return;
    if (level >= 0)
        level_cycles[level] += d;
    level_mark = now;
}

uint32_t clock_level_ms(int l)
{
    uint32_t s = arch_irq_save();
    uint64_t c;

    clock_account();
    c = level_cycles[l];
    arch_irq_restore(s);
    return (uint32_t)(c / (TIMER_HZ / 1000u));
}

void clock_set_level(int to)
{
    uint32_t s;

    if (level < 0 || to < 0 || to >= CLOCK_LEVELS || to == level)
        return;
    s = arch_irq_save();
    clock_account();
    while (level != to)
        step(level < to ? level + 1 : level - 1);
    arch_irq_restore(s);
}
