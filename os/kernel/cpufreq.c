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
 * With the panel asleep, the last hold released (or the panel going to
 * sleep with no hold) drops straight to 26 MHz instead of waiting the 1 s of
 * quiet windows: a card write or USB stop in the dark left about a second
 * at 104 MHz each time (power audit A8, 2026-11-18). Busy work after it
 * raises the clock again in the next window.
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
static bool low_ok;                     /* the panel asleep */
/* Music in bursts (power audit B2, 2026-11-18; on by default since the
 * phone's tests that day: playback speed exact, 0 underruns, the owner heard
 * it clean in the dark; poke 0 for the old way): the codec no longer holds the clock (audio.c, from its next
 * power-up), the decoder holds it only while it fills the ring (media/
 * engine.c), and in between the idle loop waits at 26 MHz (WFI) with the PLL
 * kept up, so the DMA interrupt never waits for a PLL restart (sleep.c). Any
 * non-zero value: 2 (the system sleep as well) stalled the audio DMA and was
 * removed. */
volatile uint32_t power_burst_dev = 1;
volatile uint32_t cpufreq_lit_low_dev; /* 26 MHz with the panel awake too (dev: poke) */
#define lit_low cpufreq_lit_low_dev
static int quiet;                       /* quiet windows in a row */

static int floor_level(void)
{
    return holds || !(low_ok || lit_low) ? L_104 : L_26;
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

/* nothing holds and the panel sleeps: 26 MHz now (irqs off) */
static void drop_if_idle(void)
{
    if (!holds && low_ok && mode == CPUFREQ_AUTO && clock_scalable()) {
        quiet = 0;
        apply(L_26);
    }
}

void cpufreq_release(void)
{
    uint32_t s = arch_irq_save();

    if (holds)
        holds--;
    drop_if_idle();
    arch_irq_restore(s);
}

/* 26 MHz allowed with the panel awake (power audit B1, 2026-11-18): the
 * panel shows its own GRAM, and lcd.c holds the clock up through each
 * transfer (its LCM timing is for the 104 MHz AHB). Off by default. */
void cpufreq_lit_low(bool on)
{
    uint32_t s = arch_irq_save();

    lit_low = on;
    apply(clock_level());
    arch_irq_restore(s);
}

bool cpufreq_lit_low_on(void)
{
    return lit_low;
}

void cpufreq_allow_low(bool on)
{
    uint32_t s = arch_irq_save();

    low_ok = on;
    apply(clock_level());
    drop_if_idle();
    arch_irq_restore(s);
}
