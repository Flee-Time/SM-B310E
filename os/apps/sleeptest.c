/* B310E OS — Tools -> Sleep test: tries the chip's sleep levels on the phone
 * and logs what happened; the idle loop does not use them until they are
 * proven here (arch/sc6530c/sleep.c).
 *
 * Each step sleeps a second on timer 0. Three things say how deep it went:
 *   - the 1 ms counter: how long it slept (it runs on the slow clock);
 *   - timer 2, when it runs and its bus clock stays on: far fewer cycles than
 *     the counter's time means 26 MHz stopped. On the phone (2026-11-08) it
 *     counted the whole second in a plain system sleep, and closing the
 *     timers' bus clock stops it whatever the oscillator does, so 0 there
 *     proves nothing;
 *   - timer 1 (32768 Hz) started with timer 0: how many slow ticks after timer
 *     0's end the code ran again. Restarting the oscillator and the PLL takes
 *     milliseconds, tens of ticks; a core stop takes next to none.
 * Before each step the log is written to the card and a watchdog armed (8 s),
 * so a step that never comes back shows as the last one started. In QEMU this
 * only shows that the steps run and come back.
 *
 * The status word 0x8b000120 is live: bit 0 ARM system sleep, 1 DSP stop, 2 the
 * scheduler's sleep, 3 PLL down, 4 oscillator enable, 5 DSP deep sleep, 6 DSP
 * system sleep, 8 and 9 the oscillator's and the PLL's wait counts done (the
 * sleep controller's APB_SLP_STS0: 9 never set after a software enable). The
 * "states" lines sample it with the ARM awake, to see which DSP state sets
 * those bits; each step also samples it right as the ARM wakes, where a PLL
 * that went down could still show.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/arch.h"
#include "os/drivers.h"
#include "os/dsp.h"
#include "os/fs.h"
#include "os/kernel.h"
#include "os/lib.h"
#include "os/ui.h"
#include "os/usb.h"

#define INTC_EN     0x80000008u
#define INTC_DIS    0x8000000cu
#define IRQ_T0      4
#define T1_LOAD     0x81000020u
#define T1_COUNT    0x81000024u
#define T1_CTL      0x81000028u
#define T1_INT      0x8100002cu
#define T1_TICKS    0x7fffffu
#define T2_CTL      0x81000048u
#define T2_LONG     (3u * TIMER_HZ)
#define APB_GATES   0xba01efffu
#define APB_TIMERS  (1u << 16)          /* the timers' bus clock */
#define APB_STS     0x8b000120u         /* live: see the top of this file */
#define PLL_CTL     0x8b000020u
#define DSP_RESET   0x10000u            /* 0x8b000060 set, 0x64 clear; held since
                                         * dsp_hold_reset() at boot */

enum {
    S_CORE = 1,                         /* the core only, not the system */
    S_GATES = 2,                        /* close the clocks stock closes */
    S_KEEP_TIMERS = 4,                  /* ... but the timers' bus clock */
    S_T2_OFF = 8,                       /* timer 2 stopped */
    S_DSP_RELEASED = 16,                /* the DSP's boot ROM running, as it does
                                         * from power-on if nobody holds it */
    S_LOW = 32,                         /* the ARM on the crystal, not the PLL */
};

static char report[2400];
static int used;

static void show_state(const char *when)
{
    klog("sleep test: %s: AHB sts %08lx, APB sts %08lx, 8b000000 %08lx, EB %08lx, AHB EB %08lx, "
         "SLP %08lx %08lx", when,
         (unsigned long)REG32(0x20500100), (unsigned long)REG32(0x8b000120),
         (unsigned long)REG32(0x8b000000), (unsigned long)REG32(0x8b0000a8),
         (unsigned long)REG32(0x20500080), (unsigned long)REG32(0x205000d0),
         (unsigned long)REG32(0x205000d4));
}

static uint32_t t1_read(void)
{
    uint32_t a = REG32(T1_COUNT);

    for (int i = 0; i < 100; i++) {
        uint32_t b = REG32(T1_COUNT);

        if (a == b)
            break;
        a = b;
    }
    return a;
}

/* The masks are stock's (SCI_EnterDeepSleep, NOR 0x374b4; restored at 0x3767a).
 * Returns the milliseconds the counter saw. */
static uint32_t step(const char *name, unsigned flags)
{
    uint32_t s, en, ms0, ms1, t2, t0, slow, apb = 0, apb_slp = 0, ahb = 0, sts_a, sts_b, lost, k0, c0;
    uint32_t mask = flags & S_KEEP_TIMERS ? APB_GATES & ~APB_TIMERS : APB_GATES;
    uint32_t wake_or = 0, wake_and = 0xffffu;
    int late, level = clock_level();

    klog("sleep test: %s: start", name);
    fs_sync();
    thread_sleep(200);                  /* the panel's last transfer */
    k0 = kernel_ticks();
    c0 = sleep_counter_ms();
    s = arch_irq_save();
    en = REG32(INTC_EN);
    REG32(INTC_DIS) = 0xffffffffu;
    watchdog_arm();
    timer_restart(T2_LONG);
    if (flags & S_T2_OFF)
        REG32(T2_CTL) = 0;
    if (flags & S_DSP_RELEASED)
        REG32(0x8b000064) = DSP_RESET;
    ms0 = sleep_counter_ms();
    REG32(T1_CTL) = 0;
    REG32(T1_INT) = 8;
    REG32(T1_LOAD) = T1_TICKS;
    REG32(T1_CTL) = 0x80;
    sleep_timer_start(1000);
    REG32(INTC_EN) = 1u << IRQ_T0 | 1u << IRQ_TIMER2 | 1u << IRQ_KEYPAD;
    if (flags & S_LOW) {
        clock_set_level(0);
        clock_pll_stop();
    }
    if (flags & S_GATES) {
        apb = REG32(0x8b0000a8) & mask;
        apb_slp = REG32(0x8b000100) & 4u;
        ahb = REG32(0x20500080) & 0x1fc7fu;
        REG32(0x8b000100) &= ~4u;
        REG32(0x8b0000a8) &= ~mask;
        REG32(0x20500080) &= ~0x1fc7fu;
    }
    sleep_stop(flags & S_CORE ? 1 : 0);
    for (int i = 0; i < 64; i++) {
        uint32_t v = REG32(APB_STS);

        wake_or |= v;
        wake_and &= v;
    }
    if (flags & S_GATES) {
        REG32(0x20500080) |= ahb;
        REG32(0x8b0000a8) |= apb;
        REG32(0x8b000100) |= apb_slp;
    }
    slow = T1_TICKS - t1_read();
    t2 = T2_LONG - timer_count();
    ms1 = sleep_counter_ms();
    t0 = sleep_timer_stop();
    REG32(T1_CTL) = 0;
    if (flags & S_LOW)
        clock_set_level(level);
    sts_a = REG32(0x20500100);
    sts_b = REG32(0x8b000120);
    watchdog_stop();
    if (flags & S_DSP_RELEASED)
        REG32(0x8b000060) = DSP_RESET;
    REG32(INTC_DIS) = 0xffffffffu;
    REG32(INTC_EN) = en;
    /* the time timer 2 did not see, for the kernel's clock */
    lost = ms1 - ms0;
    lost = lost * (TIMER_HZ / 1000u) > t2 ? lost * (TIMER_HZ / 1000u) - t2 : 0;
    timer_skip(lost);
    timer_restart(timer_load());
    arch_irq_restore(s);
    /* The kernel's ms count only on the next tick: logged at once, this line
     * carried the time from before the sleep (core stop: 1000 ms by the
     * counter, 211 ms between the two lines' stamps, phone 2026-11-18). */
    thread_sleep(2);
    k0 = kernel_ticks() - k0;
    c0 = sleep_counter_ms() - c0;

    late = (int)slow - (int)(1000u * 32768u / 1000u);
    klog("sleep test: %s: %lu ms by the counter, timer 2 %lu ms, woke %d slow ticks late, T0 %02lx, sts %08lx %08lx, "
         "at wake any %04lx all %04lx; across it kernel %lu ms, counter %lu ms; timer 2 %lu cycles a slow "
         "tick x1000 (crystal 793457)",
         name, (unsigned long)(ms1 - ms0), (unsigned long)(t2 / (TIMER_HZ / 1000u)), late,
         (unsigned long)t0, (unsigned long)sts_a, (unsigned long)sts_b,
         (unsigned long)wake_or, (unsigned long)wake_and, (unsigned long)k0, (unsigned long)c0,
         (unsigned long)(slow ? (uint64_t)t2 * 1000u / slow : 0));
    used += snprintf(report + used, sizeof(report) - (size_t)used, "%s:\n %lu ms, T2 %lu ms, late %d\n",
                     name, (unsigned long)(ms1 - ms0),
                     (unsigned long)(t2 / (TIMER_HZ / 1000u)), late);
    fs_sync();
    return ms1 - ms0;
}

static void show_slow_clock(const char *when)
{
    klog("sleep test: %s: slow clock 1288 %04lx 12a0 %04lx 12a4 %04lx 116c %04lx 1014 %04lx, LDO 1180 %04lx "
         "11a0 %04lx, sleep 1160 %04lx 1164 %04lx 1168 %04lx", when,
         (unsigned long)adi_read(0x82001288), (unsigned long)adi_read(0x820012a0),
         (unsigned long)adi_read(0x820012a4), (unsigned long)adi_read(0x8200116c),
         (unsigned long)adi_read(0x82001014), (unsigned long)adi_read(0x82001180),
         (unsigned long)adi_read(0x820011a0), (unsigned long)adi_read(0x82001160),
         (unsigned long)adi_read(0x82001164), (unsigned long)adi_read(0x82001168));
}

/* The status word's values over ms, ARM awake, and how often each came. */
static void states(const char *name, uint32_t ms, bool poll)
{
    uint32_t val[6], cnt[6], other = 0, t = kernel_ticks();
    char line[100];
    int n = 0, p = 0;

    while (kernel_ticks() - t < ms) {
        uint32_t v = REG32(APB_STS) & 0xffffu;
        int i;

        for (i = 0; i < n && val[i] != v; i++)
            ;
        if (i < n) {
            cnt[i]++;
        } else if (n < 6) {
            val[n] = v;
            cnt[n++] = 1;
        } else {
            other++;
        }
        if (poll)
            l1_poll();
    }
    line[0] = 0;
    for (int i = 0; i < n; i++)
        p += snprintf(line + p, sizeof(line) - (size_t)p, " %04lx x%lu", (unsigned long)val[i],
                      (unsigned long)cnt[i]);
    klog("sleep test: states, %s:%s, others %lu", name, line, (unsigned long)other);
}

static void sample_sts(uint32_t *any, uint32_t *all)
{
    *any = 0;
    *all = 0xffffu;
    for (int i = 0; i < 256; i++) {
        uint32_t v = REG32(APB_STS);

        *any |= v;
        *all &= v;
    }
}

/* The ARM on the crystal, the PLL off as in dark idle: the status word, then how
 * long after its enable the analog die's bus answers right, which is what
 * PLL_LOCK_US in clock.c must cover. Then back up. Watchdog armed. */
static void pll_try(void)
{
    uint32_t any[2], all[2], ctl, id, right, s, t0, cycles = 0, us = 0;
    int level = clock_level();

    if (level < 0)
        return;
    klog("sleep test: PLL off at 26 MHz: start");
    fs_sync();
    thread_sleep(200);
    s = arch_irq_save();
    watchdog_arm();
    right = sleep_analog_id();
    clock_set_level(0);
    clock_pll_stop();
    ctl = REG32(PLL_CTL);
    sample_sts(&any[0], &all[0]);
    t0 = timer_count();
    REG32(PLL_CTL) = ctl | 1u;          /* no wait: adi_read sees it enabled */
    /* Interrupts off: timer_cycles() stops at about two tick periods and
     * could never reach 5 ms, so counted 1 us steps bound the wait; the
     * time is summed from the down-counter every half period. */
    for (int n = 0; ; n++) {
        uint32_t e;

        id = sleep_analog_id();
        e = timer_elapsed(t0);
        us = (cycles + e) / (TIMER_HZ / 1000000u);
        if (id == right || n >= 5000)
            break;
        if (e > timer_load() / 2) {
            cycles += e;
            t0 = timer_count();
        }
        udelay(1);
    }
    udelay(2000);
    clock_set_level(level);
    sample_sts(&any[1], &all[1]);
    watchdog_stop();
    arch_irq_restore(s);
    klog("sleep test: PLL at 26 MHz: control %08lx, any %04lx all %04lx; analog id %08lx after %lu us "
         "(right %08lx); back at %d MHz: control %08lx, any %04lx all %04lx", (unsigned long)ctl,
         (unsigned long)any[0], (unsigned long)all[0], (unsigned long)id, (unsigned long)us,
         (unsigned long)right, clock_mhz(), (unsigned long)REG32(PLL_CTL),
         (unsigned long)any[1], (unsigned long)all[1]);
    fs_sync();
}

/* The PLL's registers, read only, as the SDK names them (SC6531EFM header; the
 * SC6530's 0x24 is not checked): 0x00 [25:16] XTL_BUF_WAIT and [9:0] XTL_WAIT,
 * 0x24 [9:0] PLL_WAIT, all in 32 kHz cycles (30.52 us); 0x28 [15:0] the MPLL's
 * N (13 MHz x N, 0x30 for 624 MHz if the layout holds); 0x120 the sleep
 * controller's status. If 0x24 holds a count, it is the hardware's own budget
 * for the PLL after a sleep, what clock.c's PLL_LOCK_US stands in for. */
static const uint16_t pll_regs[] = { 0x000, 0x020, 0x024, 0x028, 0x02c, 0x030, 0x034, 0x100, 0x120 };

#define SLOW_US(c) ((uint32_t)(c) * 1000000u / 32768u)

static void pll_regs_log(const char *when)
{
    uint32_t v[9];
    char line[150];
    int p = 0;

    for (int i = 0; i < 9; i++)
        v[i] = REG32(0x8b000000u + pll_regs[i]);
    for (int i = 0; i < 9; i++)
        p += snprintf(line + p, sizeof(line) - (size_t)p, " %03x %08lx", pll_regs[i],
                      (unsigned long)v[i]);
    klog("sleep test: PLL regs, %s:%s", when, line);
    klog("sleep test: PLL regs, %s: MPLL N (028 & ffff) 0x%lx = %lu MHz at 13 MHz x N (SDK: 0x30); "
         "PLL_WAIT (024 & 3ff) %lu = %lu us; XTL_BUF_WAIT (000 [25:16]) %lu = %lu us; "
         "XTL_WAIT (000 [9:0]) %lu = %lu us", when,
         (unsigned long)(v[3] & 0xffffu), (unsigned long)(v[3] & 0xffffu) * 13u,
         (unsigned long)(v[2] & 0x3ffu), (unsigned long)SLOW_US(v[2] & 0x3ffu),
         (unsigned long)(v[0] >> 16 & 0x3ffu), (unsigned long)SLOW_US(v[0] >> 16 & 0x3ffu),
         (unsigned long)(v[0] & 0x3ffu), (unsigned long)SLOW_US(v[0] & 0x3ffu));
}

/* The registers with the PLL running, then one restart as dark idle does it
 * (26 MHz, the PLL stopped 100 us, clock_pll_need's timed wait), the dump
 * right after it; the wait is timed too, and udelay(2000) at 26 MHz beside
 * it, which clock.c used before. Interrupts off and the watchdog armed. A
 * load of 3 s on timer 2 for the time, as in step(). */
static void pll_regs_measure(void)
{
    uint32_t s, t0, need, ud;
    int level = clock_level();

    pll_regs_log("running");
    s = arch_irq_save();
    watchdog_arm();
    timer_restart(T2_LONG);
    clock_set_level(0);
    clock_pll_stop();
    timer_wait_us(100);
    t0 = timer_count();
    clock_pll_need();
    need = timer_elapsed(t0);
    pll_regs_log("after a restart");
    t0 = timer_count();
    udelay(2000);
    ud = timer_elapsed(t0);
    clock_set_level(level);
    timer_restart(timer_load());
    watchdog_stop();
    arch_irq_restore(s);
    klog("sleep test: PLL restart wait: %lu cycles = %lu us (clock_pll_lock_us %lu); udelay(2000) at 26 MHz "
         "%lu cycles = %lu us", (unsigned long)need, (unsigned long)(need / (TIMER_HZ / 1000000u)),
         (unsigned long)clock_pll_lock_us(), (unsigned long)ud,
         (unsigned long)(ud / (TIMER_HZ / 1000000u)));
}

/* How soon after the enable the analog die's bus answers right and at its
 * normal speed: whether the restarts only for an ADI access (about 1.9 a
 * second in dark idle) need clock.c's whole wait. For each delay after the
 * enable, 16 back-to-back reads of the die's id (0x1000) timed on timer 2,
 * each checked against the id read with the PLL long up; 4 tries each, the
 * PLL stopped 100 us before. The CPU stays on the crystal; after the reads the
 * whole wait, then back up. Reads without clock_pll_need (adi_read_once):
 * a read that stays busy ends the batch. Interrupts off and the watchdog
 * armed for each try. */
#define PC_READS 16
#define PC_TRIES 4
#define PC_ADDR  0x82001000u

static const uint16_t pc_delay_us[] = { 0, 2, 5, 10, 20, 50, 100, 200, 500, 1000, 2000 };
#define PC_DELAYS ((int)(sizeof(pc_delay_us) / sizeof(pc_delay_us[0])))

/* Interrupts off, timer 2 on a long load. Returns the batch's cycles. */
static uint32_t pc_batch(uint32_t want, int *right, uint32_t *wrong_raw)
{
    uint32_t v[PC_READS], t0 = timer_count(), cycles;
    int n = 0;

    while (n < PC_READS) {
        v[n] = adi_read_once(PC_ADDR);
        if (v[n++] >> 31)
            break;                      /* the budget ran out: busy */
    }
    cycles = timer_elapsed(t0);
    *right = 0;
    for (int i = 0; i < n; i++) {
        if (!(v[i] >> 31) && (v[i] & 0xffffu) == want)
            (*right)++;
        else if (*wrong_raw == 0xffffffffu)
            *wrong_raw = v[i];
    }
    return cycles;
}

/* A trial wait count, written in each try with interrupts off and put back
 * before they come on (pll_wait_trials): register, field, value; none if 0. */
static uint32_t trial_reg, trial_mask, trial_val, trial_back;
static char trial_tag[24];

static uint32_t trial_apply(void)
{
    uint32_t old = trial_reg ? REG32(trial_reg) : 0;

    if (trial_reg) {
        REG32(trial_reg) = (old & ~trial_mask) | trial_val;
        trial_back = REG32(trial_reg);  /* whether the write took */
    }
    return old;
}

static void trial_undo(uint32_t old)
{
    if (trial_reg)
        REG32(trial_reg) = old;
}

/* Fills gate_us (the stall after an enable at 0 us: the batch less the
 * baseline's) and whether every read was right. */
static void pll_adi_measure(uint32_t *gate_us, bool *all_right)
{
    uint32_t want = sleep_analog_id() >> 16, base_min = 0xffffffffu, base_max = 0, s, t, old;
    uint32_t wrong_raw = 0xffffffffu, gate = 0;
    int level = clock_level(), right, from = -1, wrong = 0;
    bool ok_at[PC_DELAYS];
    const char *tag = trial_tag;

    klog("sleep test: ADI after enable%s: id high half %04lx (SLEEP_DCXO_ID %04lx), %d reads of 1000, "
         "%d tries each", tag, (unsigned long)want, (unsigned long)(SLEEP_DCXO_ID >> 16), PC_READS,
         PC_TRIES);
    /* the baseline: 26 MHz, the PLL long up */
    for (int k = 0; k < PC_TRIES; k++) {
        s = arch_irq_save();
        timer_restart(T2_LONG);
        clock_set_level(0);
        t = pc_batch(want, &right, &wrong_raw);
        clock_set_level(level);
        timer_restart(timer_load());
        arch_irq_restore(s);
        if (t < base_min)
            base_min = t;
        if (t > base_max)
            base_max = t;
        if (right != PC_READS)
            klog("sleep test: ADI after enable: baseline try %d only %d right", k, right);
    }
    klog("sleep test: ADI after enable%s: baseline, PLL up, 26 MHz: %lu..%lu cycles per %d reads",
         tag, (unsigned long)base_min, (unsigned long)base_max, PC_READS);
    for (int d = 0; d < PC_DELAYS; d++) {
        uint32_t tmin = 0xffffffffu, tmax = 0;
        int good = 0, per[PC_TRIES];

        wrong_raw = 0xffffffffu;
        for (int k = 0; k < PC_TRIES; k++) {
            s = arch_irq_save();
            watchdog_arm();
            timer_restart(T2_LONG);
            old = trial_apply();
            clock_set_level(0);
            clock_pll_stop();
            timer_wait_us(100);
            REG32(PLL_CTL) |= 1u;       /* enabled: clock_pll_need would not wait now */
            timer_wait_us(pc_delay_us[d]);
            t = pc_batch(want, &right, &wrong_raw);
            timer_wait_us(clock_pll_lock_us());     /* before the CPU goes back on it */
            clock_set_level(level);
            trial_undo(old);
            timer_restart(timer_load());
            watchdog_stop();
            arch_irq_restore(s);
            per[k] = right;
            good += right;
            if (t < tmin)
                tmin = t;
            if (t > tmax)
                tmax = t;
            thread_sleep(10);           /* the tick and the UI between */
        }
        /* right, and no slower than the baseline's slowest and an eighth */
        ok_at[d] = good == PC_READS * PC_TRIES && tmax <= base_max + base_max / 8;
        wrong += PC_READS * PC_TRIES - good;
        if (d == 0 && tmin > base_max)
            gate = (tmin - base_max) / (TIMER_HZ / 1000000u);
        klog("sleep test: ADI after enable%s: %4u us: right %d/%d (%d %d %d %d), wrong %d, "
             "batch %lu..%lu cycles, first wrong raw %08lx", tag, (unsigned)pc_delay_us[d], good,
             PC_READS * PC_TRIES, per[0], per[1], per[2], per[3], PC_READS * PC_TRIES - good,
             (unsigned long)tmin, (unsigned long)tmax, (unsigned long)wrong_raw);
    }
    for (int d = PC_DELAYS - 1; d >= 0 && ok_at[d]; d--)
        from = pc_delay_us[d];
    klog("sleep test: ADI after enable%s: right and at the baseline's speed from %d us on (-1: not even "
         "at %u us; wrong raw ffffffff: none); gate after an enable about %lu us, %d wrong in all",
         tag, from, (unsigned)pc_delay_us[PC_DELAYS - 1], (unsigned long)gate, wrong);
    used += snprintf(report + used, sizeof(report) - (size_t)used, "ADI after PLL enable%s:\n right from %d us, "
                     "gate %lu us\n", tag, from, (unsigned long)gate);
    if (gate_us)
        *gate_us = gate;
    if (all_right)
        *all_right = !wrong;
}

/* The switch up onto the PLL after a restart as the OS does it
 * (clock_pll_need, then 104 MHz), and the frequency it runs at: a fixed loop
 * timed on timer 2 in PS_WINDOWS windows right after the switch, against the
 * same with the PLL long up. Each window is about 1.3 ms; a PLL not yet locked
 * would show as windows off the baseline. Interrupts off, watchdog armed, a
 * 3 s load on timer 2. Returns whether every try switched without a stall and
 * every window was within 0.3% of the baseline's range. */
#define PS_WINDOWS 8
#define PS_TRIES   4

__attribute__((noinline)) static void ps_loop(void)
{
    for (volatile int i = 0; i < 20000; i++)
        ;
}

static void ps_windows(uint32_t *wmin, uint32_t *wmax)
{
    for (int w = 0; w < PS_WINDOWS; w++) {
        uint32_t t0 = timer_count(), c;

        ps_loop();
        c = timer_elapsed(t0);
        if (c < *wmin)
            *wmin = c;
        if (c > *wmax)
            *wmax = c;
    }
}

static bool pll_switch_check(void)
{
    uint32_t bmin = 0xffffffffu, bmax = 0, wmin = 0xffffffffu, wmax = 0, nmin = 0xffffffffu, nmax = 0;
    uint32_t smin = 0xffffffffu, smax = 0, s, t0, c, old, lock = 0;
    int level = clock_level();
    bool ok;

    for (int k = 0; k < PS_TRIES; k++) {        /* the baseline */
        s = arch_irq_save();
        watchdog_arm();
        timer_restart(T2_LONG);
        clock_set_level(1);
        ps_loop();
        ps_windows(&bmin, &bmax);
        clock_set_level(level);
        timer_restart(timer_load());
        watchdog_stop();
        arch_irq_restore(s);
        thread_sleep(10);
    }
    for (int k = 0; k < PS_TRIES; k++) {
        s = arch_irq_save();
        watchdog_arm();
        timer_restart(T2_LONG);
        old = trial_apply();
        lock = clock_pll_lock_us();
        clock_set_level(0);
        clock_pll_stop();
        timer_wait_us(100);
        t0 = timer_count();
        clock_pll_need();
        c = timer_elapsed(t0);
        if (c < nmin)
            nmin = c;
        if (c > nmax)
            nmax = c;
        t0 = timer_count();
        clock_set_level(1);
        c = timer_elapsed(t0);
        if (c < smin)
            smin = c;
        if (c > smax)
            smax = c;
        ps_windows(&wmin, &wmax);
        clock_set_level(level);
        trial_undo(old);
        timer_restart(timer_load());
        watchdog_stop();
        arch_irq_restore(s);
        thread_sleep(10);
    }
    /* a stall shows as a switch of milliseconds: allow 50 us */
    ok = smax < 50u * (TIMER_HZ / 1000000u) && wmin >= bmin - bmin * 3 / 1000 && wmax <= bmax + bmax * 3 / 1000;
    klog("sleep test: CPU switch%s: %d tries: restart wait %lu..%lu us (clock_pll_lock_us %lu), switch to "
         "104 MHz %lu..%lu cycles, loop %lu..%lu cycles a window (PLL long up %lu..%lu): %s", trial_tag,
         PS_TRIES, (unsigned long)(nmin / (TIMER_HZ / 1000000u)), (unsigned long)(nmax / (TIMER_HZ / 1000000u)),
         (unsigned long)lock, (unsigned long)smin, (unsigned long)smax,
         (unsigned long)wmin, (unsigned long)wmax, (unsigned long)bmin, (unsigned long)bmax,
         ok ? "right" : "WRONG");
    return ok;
}

/* The kernel's time across restarts with the tick running, as in dark idle: a
 * restart with interrupts off spans many reloads of timer 2, and the interrupt
 * counts one (before timer_wait_us counted them: 8.9 ms lost a restart, phone,
 * 2026-11-18). Kernel ms against the 1 ms counter, first a run of plain sleeps
 * (the two clocks' ratio), then the same with a restart in each. */
#define PD_RUNS 200

static void pd_run(bool restart, uint32_t *k, uint32_t *m)
{
    int level = clock_level();
    uint32_t k0, m0;

    thread_sleep(1);                    /* start just after a tick */
    k0 = kernel_ticks();
    m0 = sleep_counter_ms();
    for (int i = 0; i < PD_RUNS; i++) {
        if (restart) {
            uint32_t s = arch_irq_save();

            clock_set_level(0);
            clock_pll_stop();
            timer_wait_us(100);
            clock_pll_need();
            clock_set_level(level);
            arch_irq_restore(s);
        }
        thread_sleep(3);
    }
    *k = kernel_ticks() - k0;
    *m = sleep_counter_ms() - m0;
}

static void pll_drift_measure(void)
{
    uint32_t k[2], m[2], starts = clock_pll_starts();
    int32_t lost_us;

    pd_run(false, &k[0], &m[0]);
    pd_run(true, &k[1], &m[1]);
    starts = clock_pll_starts() - starts;
    /* the kernel ms the counter's ms predict at the plain run's ratio, less
     * those counted, per restart */
    lost_us = m[0] ? (int32_t)(((int64_t)m[1] * k[0] / m[0] - k[1]) * 1000 / PD_RUNS) : 0;
    klog("sleep test: PLL drift: %d plain sleeps: kernel %lu ms, counter %lu ms; %d with a restart "
         "(%lu PLL starts): kernel %lu ms, counter %lu ms; kernel time lost %ld us a restart",
         PD_RUNS, (unsigned long)k[0], (unsigned long)m[0], PD_RUNS, (unsigned long)starts,
         (unsigned long)k[1], (unsigned long)m[1], (long)lost_us);
}

/* The log's lines to the card between the parts, the clock up and nothing
 * stopped: with the card held throughout, the log's 4 KB buffer filled and
 * lines were lost (a measurement C row on the phone, more in QEMU). */
static void pm_flush(void)
{
    sd_release();
    fs_sync();
    sd_hold();
}

/* Timer 2's count as the kernel reads it: back-to-back reads with a 3 s load
 * (no reload in them) must never go up, and the first read after a restart
 * must be at the load or just under it. A read that goes up looks like a
 * reload to into_period and timer_wait_us (a whole period counted), a stale
 * one after a restart like a period's worth gone. At 26 MHz with the PLL up,
 * at 26 MHz with it stopped (dark idle's case), and at 104 MHz. Once, dark
 * idle ran 123 ms ahead of the 1 ms counter and the PC in 281 s (phone,
 * 2026-11-18; not seen again in five runs). Every read here was right.
 * Interrupts off, the watchdog armed. */
#define T2_COUNT    0x81000044u
#define T2_READS    200000
#define T2_RESTARTS 1000

static void t2_read_check(void)
{
    static const char *const how[3] = { "26 MHz", "26 MHz, PLL stopped", "104 MHz" };
    int level = clock_level();

    for (int m = 0; m < 3; m++) {
        uint32_t s, prev, ups = 0, maxup = 0, above = 0, maxbelow = 0, t0, reads;

        s = arch_irq_save();
        watchdog_arm();
        timer_restart(T2_LONG);
        clock_set_level(m == 2 ? 1 : 0);
        if (m == 1)
            clock_pll_stop();
        t0 = timer_count();
        prev = REG32(T2_COUNT);
        for (int i = 0; i < T2_READS; i++) {
            uint32_t now = REG32(T2_COUNT);

            if (now > prev) {
                ups++;
                if (now - prev > maxup)
                    maxup = now - prev;
            }
            prev = now;
        }
        reads = timer_elapsed(t0);
        for (int i = 0; i < T2_RESTARTS; i++) {
            uint32_t r;

            timer_restart(T2_LONG);
            r = REG32(T2_COUNT);
            if (r > T2_LONG)
                above++;
            else if (T2_LONG - r > maxbelow)
                maxbelow = T2_LONG - r;
        }
        clock_set_level(level);         /* starts the PLL if stopped */
        timer_restart(timer_load());
        watchdog_stop();
        arch_irq_restore(s);
        klog("sleep test: timer 2 reads, %s: %d reads in %lu us, %lu went up (most %lu); %d restarts: first "
             "read above the load %lu, most under it %lu cycles", how[m], T2_READS,
             (unsigned long)(reads / (TIMER_HZ / 1000000u)), (unsigned long)ups, (unsigned long)maxup,
             T2_RESTARTS, (unsigned long)above, (unsigned long)maxbelow);
        thread_sleep(10);
    }
}

/* Dark idle's sleep, as the idle loop and the kernel do it, against timer 1's
 * slow ticks: a period on timer 2, 26 MHz, the PLL stopped, the system sleep,
 * timer 2's end waking it, the tick restarted as align() does, then a PLL
 * restart as the first analog read after a wake does it. Then the same
 * without the PLL stop, and without the sleep (a busy wait for the end): which
 * part gains time. Once, with chip sleep on, dark idle ran 123 ms ahead of
 * the 1 ms counter in 281 s (28 sleeps); off, 5 ms behind; later runs with it
 * on were within 10 ms, and every case here within 25 us a sleep (phone,
 * 2026-11-18). Interrupts off, the watchdog armed, only timer 2's line at the
 * INTC. */
static void idle_like_check(void)
{
    static const struct { const char *how; uint8_t n, sleep, stop; uint16_t ms; } v[4] = {
        { "sleep, PLL stopped", 16, 1, 1, 150 }, { "sleep, PLL stopped", 4, 1, 1, 1000 },
        { "sleep, PLL up", 16, 1, 0, 150 }, { "no sleep, PLL stopped", 16, 0, 1, 150 },
    };
    int level = clock_level();

    for (int m = 0; m < 4; m++) {
        uint32_t s, en, c0, c1, ms0, ms1, slow, starts = clock_pll_starts();
        int64_t gain, gain_ms;

        s = arch_irq_save();
        en = REG32(INTC_EN);
        REG32(INTC_DIS) = 0xffffffffu;
        watchdog_arm();
        timer_restart(timer_load());
        ms0 = sleep_counter_ms();
        c0 = timer_cycles();
        REG32(T1_CTL) = 0;              /* from its load, as in step() */
        REG32(T1_INT) = 8;
        REG32(T1_LOAD) = T1_TICKS;
        REG32(T1_CTL) = 0x80;
        for (int i = 0; i < v[m].n; i++) {
            timer_restart(v[m].ms * (TIMER_HZ / 1000u));
            REG32(INTC_EN) = 1u << IRQ_TIMER2;
            clock_set_level(0);
            if (v[m].stop)
                clock_pll_stop();
            if (v[m].sleep) {
                sleep_stop(0);
            } else {
                while (!(REG32(0x8100004cu) & 4))
                    ;
            }
            REG32(INTC_DIS) = 0xffffffffu;
            timer_restart(timer_load());        /* align(): the pending end counted */
            clock_pll_need();
            clock_set_level(level);
        }
        slow = T1_TICKS - t1_read();
        c1 = timer_cycles() - c0;
        ms1 = sleep_counter_ms() - ms0;
        REG32(T1_CTL) = 0;
        timer_restart(timer_load());
        watchdog_stop();
        REG32(INTC_EN) = en;
        arch_irq_restore(s);
        /* the kernel's cycles less those timer 1 and the 1 ms counter say passed */
        gain = (int64_t)c1 - (int64_t)slow * TIMER_HZ / 32768;
        gain_ms = (int64_t)c1 - (int64_t)ms1 * (TIMER_HZ / 1000u);
        klog("sleep test: idle-like, %s: %d x %d ms: kernel %lu us, timer 1 %lu slow ticks = %lu us, counter "
             "%lu ms; kernel ahead %ld us a sleep (by the counter %ld); %lu PLL starts", v[m].how, v[m].n,
             v[m].ms, (unsigned long)(c1 / (TIMER_HZ / 1000000u)), (unsigned long)slow,
             (unsigned long)((uint64_t)slow * 1000000u / 32768u), (unsigned long)ms1,
             (long)(gain / (TIMER_HZ / 1000000u) / v[m].n), (long)(gain_ms / (TIMER_HZ / 1000000u) / v[m].n),
             (unsigned long)(clock_pll_starts() - starts));
        thread_sleep(20);
    }
}

/* A shorter hardware gate: PLL_WAIT (0x8b000024 [9:0]) at 64 slow cycles
 * (1.95 ms, the SC6530 SDK's figure in commented-out code), then 40 (1.22
 * ms, what the SC6531 SDK writes) if 64 was clean; measurement C and the
 * switch check at each. If 64 does not move the gate, PLL_WAIT is not it:
 * XTL_WAIT (0x8b000000 [9:0], also 320) is tried the same way. Each value is
 * written only inside a try, interrupts off and the watchdog armed, and put
 * back before they come on, so the idle loop never sleeps with it. */
#define PLL_CTL1  0x8b000024u
#define XTL_CTL0  0x8b000000u
#define WAIT_MASK 0x3ffu

static bool pll_wait_try(uint32_t reg, const char *name, uint32_t val, uint32_t *gate)
{
    bool right, sw;

    trial_reg = reg;
    trial_mask = WAIT_MASK;
    trial_val = val;
    snprintf(trial_tag, sizeof(trial_tag), ", %s %lu", name, (unsigned long)val);
    pll_adi_measure(gate, &right);
    pm_flush();
    sw = pll_switch_check();
    klog("sleep test: trial %s %lu (%lu us): read back %08lx in the try; gate about %lu us, reads %s, CPU switch "
         "%s; register back to %08lx", name, (unsigned long)val, (unsigned long)SLOW_US(val),
         (unsigned long)trial_back, (unsigned long)*gate, right ? "all right" : "SOME WRONG",
         sw ? "right" : "WRONG", (unsigned long)REG32(reg));
    trial_reg = 0;
    trial_tag[0] = 0;
    pm_flush();
    return right && sw;
}

static void pll_wait_trials(void)
{
    uint32_t gate = 0, g40 = 0;
    bool ok64, ok40 = false;

    klog("sleep test: trials: 024 %08lx, 000 %08lx", (unsigned long)REG32(PLL_CTL1),
         (unsigned long)REG32(XTL_CTL0));
    ok64 = pll_wait_try(PLL_CTL1, "PLL_WAIT", 64, &gate);
    if (gate < 5000) {                  /* moved from about 9800 us */
        if (ok64)
            ok40 = pll_wait_try(PLL_CTL1, "PLL_WAIT", 40, &g40);
        klog("sleep test: trials: PLL_WAIT gates (64: %lu us, %s; 40: %s)", (unsigned long)gate,
             ok64 ? "clean" : "NOT clean", !ok64 ? "not tried" : ok40 ? "clean" : "NOT clean");
    } else {
        klog("sleep test: trials: PLL_WAIT 64 left the gate at %lu us: trying XTL_WAIT", (unsigned long)gate);
        pll_wait_try(XTL_CTL0, "XTL_WAIT", 64, &gate);
    }
    used += snprintf(report + used, sizeof(report) - (size_t)used, "PLL_WAIT 64: %s\n",
                     ok64 ? "clean" : "not clean");
}

/* About 250 PLL restarts, each stalling the CPU about 10 ms with interrupts
 * off: the phone's USB network was gone after the first run of these, and the
 * log file got garbled bytes where the clock dropped under the log thread's
 * card write (2026-11-18). So USB is off for them, as for the system steps,
 * and the card held. */
static void pll_measure(void)
{
    bool usb;

    if (clock_level() < 0) {
        klog("sleep test: PLL measure: the boot clock is unknown, not tried");
        return;
    }
    usb = usb_auto_pause();
    if (usb)
        klog("sleep test: USB off for the PLL measure");
    fs_sync();
    sd_hold();
    thread_sleep(200);                  /* the panel's last transfer */
    pll_regs_measure();
    pm_flush();
    t2_read_check();
    idle_like_check();
    pm_flush();
    pll_wait_trials();
    pm_flush();
    pll_adi_measure(NULL, NULL);
    pm_flush();
    pll_drift_measure();
    sd_release();
    fs_sync();
    if (usb)
        usb_auto_resume();
}

/* Layer 1 running, nothing received: idle frames, then its multi-frame sleep,
 * which is how stock's DSP rests between paging blocks. No radio, no TX. */
static void l1_steps(void)
{
    int rc = l1_start();

    if (rc == DSP_OK)
        rc = l1_run();
    klog("sleep test: layer 1: %s", dsp_strerror(rc));
    if (rc != DSP_OK)
        return;
    while (l1_fi_free() > 2 && l1_idle_post(1) == DSP_OK)
        ;
    states("layer 1, idle frames", 100, true);
    rc = l1_sleep_post(900);            /* about 4 s */
    klog("sleep test: multi-frame sleep posted: %s", dsp_strerror(rc));
    if (rc != DSP_OK)
        return;
    states("layer 1, frame sleep", 500, true);
    step("frame sleep, system", 0);
    step("frame sleep, gates, T2 off", S_GATES | S_T2_OFF);
    states("layer 1, frame sleep, after", 200, true);
}

/* The DSP running the phone's own program, as on stock: if it lets the chip
 * stop, the status words and the wake's lateness show it. Nothing is sent to
 * it; dsp_boot() starts the program, dsp_stop() holds it in reset again. */
static volatile int dsp_result;
static volatile bool dsp_done;

static void dsp_worker(void *arg)
{
    dsp_result = dsp_boot();
    dsp_done = true;
}

static void dsp_steps(void)
{
    dsp_done = false;
    if (!thread_create("dspboot", PRIO_NORMAL, 2048, dsp_worker, NULL))
        return;
    while (!dsp_done)
        thread_sleep(50);
    klog("sleep test: DSP program: boot %d, %lu ms, ready word %04lx", dsp_result,
         (unsigned long)dsp_info.boot_ms, (unsigned long)dsp_info.ready_word);
    if (dsp_result == DSP_OK) {
        thread_sleep(500);              /* let it settle */
        show_state("DSP program running");
        states("DSP program", 100, false);
        step("DSP running, system", 0);
        step("DSP running, gates, T2 on", S_GATES | S_KEEP_TIMERS);
        step("DSP running, gates, T2 off", S_GATES | S_T2_OFF);
        show_state("DSP program, after");
        l1_steps();
    }
    dsp_stop();
}

static void system_steps_run(void)
{
    uint32_t ms;

    sleep_chip_init();
    show_slow_clock("after the set-up");
    show_state("after the set-up");
    /* The slow clock must have survived the set-up: this wakes by timer 2
     * after 3 s if timer 0 no longer runs, and system sleep would not wake */
    ms = step("core stop, set up", S_CORE);
    if (ms < 900 || ms > 1100) {
        klog("sleep test: %lu ms: the slow clock is not right, system sleep not tried",
             (unsigned long)ms);
        used += snprintf(report + used, sizeof(report) - (size_t)used,
                         "slow clock wrong:\nsystem sleep not tried\n");
        ui_text_view("Sleep test", report);
        return;
    }
    states("DSP in reset", 100, false);
    step("system", 0);
    step("system, 26 MHz", S_LOW);
    step("gates, T2 on, 26 MHz", S_GATES | S_KEEP_TIMERS | S_LOW);
    pll_try();
    step("system, T2 off", S_T2_OFF);
    step("gates, T2 on", S_GATES | S_KEEP_TIMERS);
    step("gates, T2 off", S_GATES | S_T2_OFF);
    step("DSP out of reset, gates, T2 on", S_GATES | S_KEEP_TIMERS | S_DSP_RELEASED);
    REG32(0x8b000064) = DSP_RESET;
    states("DSP out of reset, no program", 100, false);
    REG32(0x8b000060) = DSP_RESET;
    dsp_steps();
    show_state("after the last");
    ui_text_view("Sleep test", report);
}

/* These close the USB clocks and drop to the crystal, which USB does not survive
 * (the data pipes die): the background network is off for them, and back after. */
static void system_steps(void)
{
    bool usb = usb_auto_pause();

    if (usb)
        klog("sleep test: USB off for the steps");
    system_steps_run();
    if (usb)
        usb_auto_resume();
}

void app_sleep_test(void)
{
    used = 0;
    klog("sleep test: analog id %08lx, DSP reset word %08lx, LDO 1180 %04lx 1184 %04lx 11a0 %04lx 11a4 %04lx, "
         "clocks 40 %08lx, MCU 1a0 %08lx, DSP 1c0 %08lx 1c4 %08lx",
         (unsigned long)sleep_analog_id(), (unsigned long)REG32(0x8b000060),
         (unsigned long)adi_read(0x82001180), (unsigned long)adi_read(0x82001184),
         (unsigned long)adi_read(0x820011a0), (unsigned long)adi_read(0x820011a4),
         (unsigned long)REG32(0x8b000040), (unsigned long)REG32(0x8b0001a0),
         (unsigned long)REG32(0x8b0001c0), (unsigned long)REG32(0x8b0001c4));
    show_slow_clock("before");
    show_state("before");
    pll_measure();                      /* no sleep: what dark idle does at each wake */
    step("core stop", S_CORE);
    ui_confirm("System sleep",
               "Now the real one. If the phone does not come back, take the battery out.",
               system_steps);
}
