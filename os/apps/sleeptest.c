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
 * system sleep, 8 and 9 the oscillator's and the PLL's wait counts done. The
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
    uint32_t s, en, ms0, ms1, t2, t0, slow, apb = 0, apb_slp = 0, ahb = 0, sts_a, sts_b, lost;
    uint32_t mask = flags & S_KEEP_TIMERS ? APB_GATES & ~APB_TIMERS : APB_GATES;
    uint32_t wake_or = 0, wake_and = 0xffffu;
    int late, level = clock_level();

    klog("sleep test: %s: start", name);
    fs_sync();
    thread_sleep(200);                  /* the panel's last transfer */
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

    late = (int)slow - (int)(1000u * 32768u / 1000u);
    klog("sleep test: %s: %lu ms by the counter, timer 2 %lu ms, woke %d slow ticks late, T0 %02lx, sts %08lx %08lx, "
         "at wake any %04lx all %04lx",
         name, (unsigned long)(ms1 - ms0), (unsigned long)(t2 / (TIMER_HZ / 1000u)), late,
         (unsigned long)t0, (unsigned long)sts_a, (unsigned long)sts_b,
         (unsigned long)wake_or, (unsigned long)wake_and);
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
    uint32_t any[2], all[2], ctl, id, right, s, t0, us = 0;
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
    t0 = timer_cycles();
    REG32(PLL_CTL) = ctl | 1u;          /* no wait: adi_read sees it enabled */
    do {
        id = sleep_analog_id();
        us = (timer_cycles() - t0) / (TIMER_HZ / 1000000u);
    } while (id != right && us < 5000);
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
    step("core stop", S_CORE);
    ui_confirm("System sleep",
               "Now the real one. If the phone does not come back, take the battery out.",
               system_steps);
}
