/* B310E OS — the kernel tick on system timer 2.
 *
 * Timer 2 (0x81000040, 26 MHz, IRQ 23): load +0, current count +4,
 * control +8 (0xc0 = enable, periodic), interrupt +0xc (1 = enable; the
 * handler acknowledges with 9 when bit 2 is pending). Clock gate and
 * reset as fpdoom and the Rockbox port do it (kernel-sc6530c.c).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/arch.h"

#define T2_LOAD   0x81000040u
#define T2_COUNT  0x81000044u
#define T2_CTL    0x81000048u
#define T2_INT    0x8100004cu

static void (*tick_fn)(void);
static uint32_t period;                 /* the tick: 26000 cycles */
static uint32_t cur;                    /* the load running now */
static uint32_t base;                   /* cycles before the current period */
static uint32_t low;                    /* the lowest count read this period */
static bool started;                    /* timer_wait_us: udelay before */

/* Cycles into the current period; a period that ended but whose interrupt
 * has not run yet counts whole. It ended when bit 2 is pending, or when
 * the count is above one already read this period: the countdown can
 * reload before the pending bit shows (QEMU's timer does; the chip is not
 * known not to), and reading that as early in the period put the time
 * back by a period, which the kernel's unsigned tick count turned into a
 * jump of 2^32 cycles (165 s): a playing track was then "taken".
 * Interrupts off. */
static uint32_t into_period(void)
{
    uint32_t c = REG32(T2_COUNT);

    if (REG32(T2_INT) & 4)
        return cur + (cur - REG32(T2_COUNT));
    if (c > low)
        return cur + (cur - c);         /* reloaded, the bit not yet set */
    low = c;
    return cur - c;
}

static void timer2_isr(void)
{
    if (REG32(T2_INT) & 4) {
        REG32(T2_INT) = 9;
        base += cur;
        low = cur;                      /* a new period */
    }
    tick_fn();
}

void timer_init(unsigned hz, void (*tick)(void))
{
    tick_fn = tick;
    period = cur = low = TIMER_HZ / hz;

    REG32(0x8b0000a0) = 0x410000u;      /* timer clock gate */
    REG32(0x8b000060) = 0x2000u;        /* reset pulse */
    udelay(1000);
    REG32(0x8b000064) = 0x2000u;
    udelay(1000);

    REG32(T2_CTL) = 0;
    REG32(T2_LOAD) = cur;
    REG32(T2_INT) = 1;
    REG32(T2_CTL) = 0xc0;
    irq_register(IRQ_TIMER2, timer2_isr);
    irq_enable_line(IRQ_TIMER2);
    started = true;
}

uint32_t timer_count(void)
{
    return REG32(T2_COUNT);
}

/* 26 MHz cycles since timer_init (wraps after 165 s; use differences).
 * Interrupts off. Until 2026-11-07 the kernel measured time as the
 * countdown's difference modulo one period, which lost every whole period
 * the idle thread slept through: the phone showed 10% load and 17000
 * timer interrupts a second at rest. Since the tickless idle the period
 * changes, so the cycles before the running period are kept in base. */
uint32_t timer_cycles(void)
{
    return base + into_period();
}

/* Start a new period of cycles now (tickless idle: a long one; after it,
 * the tick again). Stop, load, start, as at init: the count restarts from
 * the load on the chip. Interrupts off. */
static bool long_bad;                   /* a load did not read back as written */
static uint32_t long_max;               /* the longest load read back right */

void timer_restart(uint32_t cycles)
{
    uint32_t back;

    if ((int32_t)cycles < 260)          /* 10 us (or late): not worth less */
        cycles = 260;
    base += into_period();
    REG32(T2_INT) = 9;                  /* a pending end is counted above */
    REG32(T2_CTL) = 0;
    REG32(T2_LOAD) = cycles;
    REG32(T2_CTL) = 0xc0;
    /* Since 2026-11-18 a tickless sleep loads up to 60 s (1.56e9 cycles, 31
     * bits); the 1 s before needed 25, and the sleep test's 3 s load 27. The
     * register's width is not documented: a long load is read back, and one
     * that differs (cut to fewer bits) is what the count runs from, so the
     * time stays right; the kernel then keeps its sleeps to a second
     * (timer_long_ok). An unreadable 0 changes nothing. */
    if (cycles > TIMER_HZ) {
        back = REG32(T2_LOAD);
        if (back == cycles) {
            if (cycles > long_max)
                long_max = cycles;
        } else if (back) {
            long_bad = true;
            cycles = back;
        }
    }
    cur = low = cycles;
}

bool timer_long_ok(void)
{
    return !long_bad;
}

uint32_t timer_long_max(void)
{
    return long_max;
}

void timer_skip(uint32_t cycles)
{
    base += cycles;
}

uint32_t timer_load(void)
{
    return period;
}

/* Cycles since start; correct while less than one tick period passed or
 * the caller accounts for the ticks itself. */
uint32_t timer_elapsed(uint32_t start)
{
    uint32_t now = REG32(T2_COUNT);

    return start >= now ? start - now : start + cur - now;
}

/* A busy wait on the down-counter, for waits that must not run long with the
 * clock (udelay's 2 ms are about 2.46 ms at 26 MHz). Interrupts off for it:
 * reads come far less than a period apart, so a count above the last read is
 * one reload of the load running, the 1 ms tick or a tickless sleep's up to
 * 60 s. Before timer_init, or with the count standing (the sleep test stops
 * timer 2 or closes the timers' bus clock), udelay, which is never short.
 *
 * The interrupt after it counts one reload (into_period too), however many
 * passed: a 2 ms wait on the 1 ms tick, a PLL restart, lost a period from
 * the kernel's time. The others it saw go into base here. */
void timer_wait_us(unsigned us)
{
    uint32_t need = us * (TIMER_HZ / 1000000u), sum = 0, same = 0, reloads, mark, s;

    if (!started) {
        udelay(us);
        return;
    }
    s = arch_irq_save();
    mark = REG32(T2_COUNT);
    /* a reload not yet counted: then every one in the wait is extra */
    reloads = (REG32(T2_INT) & 4) || mark > low ? 1 : 0;
    while (sum < need) {
        uint32_t now = REG32(T2_COUNT);

        if (now == mark) {
            if (++same > 10000) {
                udelay(us);
                break;
            }
            continue;
        }
        same = 0;
        if (now > mark)
            reloads++;
        sum += mark > now ? mark - now : mark + cur - now;
        mark = now;
    }
    if (reloads > 1)
        base += (reloads - 1) * cur;
    arch_irq_restore(s);
}
