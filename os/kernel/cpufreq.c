/* B310E OS — CPU clock governor.
 *
 * The kernel tick hands over each 50 ms window's busy share, measured in
 * 26 MHz timer cycles (wall time, so the same work reads busier at a
 * lower clock). Busy over 70% jumps to 208 MHz; at 208 MHz, under 30% for
 * 200 ms steps down to 104 MHz; at 104 MHz, under 10% for 1 s steps down
 * to 26 MHz where allowed; at 26 MHz, over 40% goes back to 104 MHz.
 *
 * 26 MHz is allowed only while the panel is asleep (the LCD transfer
 * timing assumes the 104 MHz AHB) and no SD transfer holds the clock;
 * cpufreq_hold() and cpufreq_allow_low(false) raise it at once.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/arch.h"
#include "os/kernel.h"

#define L_26    0
#define L_104   1
#define L_208   2

static int mode = CPUFREQ_AUTO;
static int holds;
static bool low_ok;
static int quiet;                       /* quiet windows in a row */

static int floor_level(void)
{
    return holds || !low_ok ? L_104 : L_26;
}

static void apply(int want)
{
    if (want < floor_level())
        want = floor_level();
    if (mode == CPUFREQ_MAX)
        want = L_208;
    clock_set_level(want);
}

void cpufreq_window(uint32_t busy)
{
    int l = clock_level(), want = l;

    clock_account();
    if (!clock_scalable())
        return;
    if (busy > 700) {
        want = L_208;
        quiet = 0;
    } else if (l == L_26 && busy > 400) {
        want = L_104;
        quiet = 0;
    } else if (l > L_26 && busy < (l == L_208 ? 300u : 100u)) {
        if (++quiet >= (l == L_208 ? 4 : 20)) {
            want = l - 1;
            quiet = 0;
        }
    } else {
        quiet = 0;
    }
    apply(want);
}

int cpufreq_holds(void)
{
    return holds;
}

void cpufreq_set_mode(int m)
{
    uint32_t s = arch_irq_save();

    mode = m;
    apply(clock_level());
    arch_irq_restore(s);
}

void cpufreq_hold(void)
{
    uint32_t s = arch_irq_save();

    holds++;
    apply(clock_level());
    arch_irq_restore(s);
}

void cpufreq_release(void)
{
    uint32_t s = arch_irq_save();

    if (holds)
        holds--;
    arch_irq_restore(s);
}

void cpufreq_allow_low(bool on)
{
    uint32_t s = arch_irq_save();

    low_ok = on;
    apply(clock_level());
    arch_irq_restore(s);
}
