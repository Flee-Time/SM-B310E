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

/* Cycles into the current period; a period that ended but whose interrupt
 * has not run yet (bit 2 pending) counts whole. Interrupts off. */
static uint32_t into_period(void)
{
    uint32_t c = REG32(T2_COUNT);

    if (REG32(T2_INT) & 4)
        return cur + (cur - REG32(T2_COUNT));
    return cur - c;
}

static void timer2_isr(void)
{
    if (REG32(T2_INT) & 4) {
        REG32(T2_INT) = 9;
        base += cur;
    }
    tick_fn();
}

void timer_init(unsigned hz, void (*tick)(void))
{
    tick_fn = tick;
    period = cur = TIMER_HZ / hz;

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
void timer_restart(uint32_t cycles)
{
    if ((int32_t)cycles < 260)          /* 10 us (or late): not worth less */
        cycles = 260;
    base += into_period();
    REG32(T2_INT) = 9;                  /* a pending end is counted above */
    REG32(T2_CTL) = 0;
    REG32(T2_LOAD) = cycles;
    REG32(T2_CTL) = 0xc0;
    cur = cycles;
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
