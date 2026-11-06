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
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/arch.h"

#define CLK_CTL     0x8b000040u
#define CLK_SEL     3u
#define CLK_HALF    4u

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

void clock_set_level(int to)
{
    uint32_t s;

    if (level < 0 || to < 0 || to >= CLOCK_LEVELS || to == level)
        return;
    s = arch_irq_save();
    while (level != to)
        step(level < to ? level + 1 : level - 1);
    arch_irq_restore(s);
}
