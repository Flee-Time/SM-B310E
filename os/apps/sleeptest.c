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
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/arch.h"
#include "os/drivers.h"
#include "os/dsp.h"
#include "os/fs.h"
#include "os/kernel.h"
#include "os/lib.h"
#include "os/ui.h"

#define INTC_EN     0x80000008u
#define INTC_DIS    0x8000000cu
#define IRQ_T0      4
#define WDG         0x82001480u
#define T1_LOAD     0x81000020u
#define T1_COUNT    0x81000024u
#define T1_CTL      0x81000028u
#define T1_INT      0x8100002cu
#define T1_TICKS    0x7fffffu
#define T2_CTL      0x81000048u
#define T2_LONG     (3u * TIMER_HZ)
#define APB_GATES   0xba01efffu
#define APB_TIMERS  (1u << 16)          /* the timers' bus clock */
#define DSP_RESET   0x10000u            /* 0x8b000060 set, 0x64 clear; held since
                                         * dsp_hold_reset() at boot */

enum {
    S_CORE = 1,                         /* the core only, not the system */
    S_GATES = 2,                        /* close the clocks stock closes */
    S_KEEP_TIMERS = 4,                  /* ... but the timers' bus clock */
    S_T2_OFF = 8,                       /* timer 2 stopped */
    S_DSP_RELEASED = 16,                /* the DSP's boot ROM running, as it does
                                         * from power-on if nobody holds it */
};

static char report[1200];
static int used;

static void watchdog_arm(void)
{
    adi_write(0x820010e0, 4);
    adi_write(0x820010e4, 2);
    adi_write(WDG + 0x20, 0xe551);
    adi_write(WDG + 8, adi_read(WDG + 8) | 9);
    adi_write(WDG, 0);                  /* 8 s at 32768 Hz */
    adi_write(WDG + 4, 4);
    adi_write(WDG + 8, adi_read(WDG + 8) | 2);
    adi_write(WDG + 0x20, (uint16_t)~0xe551u);
}

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
    int late;

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
    if (flags & S_GATES) {
        apb = REG32(0x8b0000a8) & mask;
        apb_slp = REG32(0x8b000100) & 4u;
        ahb = REG32(0x20500080) & 0x1fc7fu;
        REG32(0x8b000100) &= ~4u;
        REG32(0x8b0000a8) &= ~mask;
        REG32(0x20500080) &= ~0x1fc7fu;
    }
    sleep_stop(flags & S_CORE ? 1 : 0);
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
    klog("sleep test: %s: %lu ms by the counter, timer 2 %lu ms, woke %d slow ticks late, T0 %02lx, sts %08lx %08lx",
         name, (unsigned long)(ms1 - ms0), (unsigned long)(t2 / (TIMER_HZ / 1000u)), late,
         (unsigned long)t0, (unsigned long)sts_a, (unsigned long)sts_b);
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
        step("DSP running, system", 0);
        step("DSP running, gates, T2 on", S_GATES | S_KEEP_TIMERS);
        step("DSP running, gates, T2 off", S_GATES | S_T2_OFF);
        show_state("DSP program, after");
    }
    dsp_stop();
}

static void system_steps(void)
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
    step("system", 0);
    step("system, T2 off", S_T2_OFF);
    step("gates, T2 on", S_GATES | S_KEEP_TIMERS);
    step("gates, T2 off", S_GATES | S_T2_OFF);
    step("DSP out of reset, gates, T2 on", S_GATES | S_KEEP_TIMERS | S_DSP_RELEASED);
    dsp_steps();
    show_state("after the last");
    ui_text_view("Sleep test", report);
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
