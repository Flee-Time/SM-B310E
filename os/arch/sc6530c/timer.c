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
static uint32_t load;

static void timer2_isr(void)
{
    if (REG32(T2_INT) & 4)
        REG32(T2_INT) = 9;
    tick_fn();
}

void timer_init(unsigned hz, void (*tick)(void))
{
    tick_fn = tick;
    load = TIMER_HZ / hz;

    REG32(0x8b0000a0) = 0x410000u;      /* timer clock gate */
    REG32(0x8b000060) = 0x2000u;        /* reset pulse */
    udelay(1000);
    REG32(0x8b000064) = 0x2000u;
    udelay(1000);

    REG32(T2_CTL) = 0;
    REG32(T2_LOAD) = load;
    REG32(T2_INT) = 1;
    REG32(T2_CTL) = 0xc0;
    irq_register(IRQ_TIMER2, timer2_isr);
    irq_enable_line(IRQ_TIMER2);
}

uint32_t timer_count(void)
{
    return REG32(T2_COUNT);
}

uint32_t timer_load(void)
{
    return load;
}

/* Cycles since start; correct while less than one tick period passed or
 * the caller accounts for the ticks itself. */
uint32_t timer_elapsed(uint32_t start)
{
    uint32_t now = REG32(T2_COUNT);

    return start >= now ? start - now : start + load - now;
}
