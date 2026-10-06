/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2007 by Michael Sevakis
 * Copyright (C) 2026 by B310E-OS project
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "timer.h"
#include "thread.h"

/*
 * B310E-OS Rockbox port — b310e/target/arm/sc6530c/kernel-sc6530c.c
 * (GPLv2, Rockbox-derived; modeled on Rockbox's s3c2440/kernel-s3c2440.c)
 *
 * Rockbox's tick (firmware/kernel/tick.c): the target implements
 * `tick_start(unsigned int interval_in_ms)` and calls `call_tick_tasks()`
 * from the timer ISR. On the SC6530C we use the SAME 1 ms system timer 2
 * as B310E-OS (kernel/irq.c sys_timer_start + kernel/sched.c sys_tick_isr,
 * fpdoom lcd_mono.h verbatim):
 *
 *   timer 2 @ 0x81000040, 26 MHz, IRQ line 23
 *     load @ +0   = 26000 * interval_ms
 *     ctl  @ +8   = 0xc0 (enable)
 *     int  @ +0xc = 1 (enable); the ISR ACKs by writing 9 (bit3 clr + bit0
 *                   en) when bit 2 is pending — the INTC itself has no
 *                   acknowledge register (0x8000000C is INT_DISABLE and
 *                   MUST NOT be written in the ISR, or the tick stops).
 */

#define REG32(a) (*(volatile uint32_t *)(a))

#define TIMER2_REG     0x81000040
#define TIMER2_LOAD    (TIMER2_REG + 0x00)
#define TIMER2_CTL     (TIMER2_REG + 0x08)
#define TIMER2_INT     (TIMER2_REG + 0x0C)
#define TIMER2_LOAD_1MS 26000   /* 26 MHz -> 1 ms */

#define TIMER_CLK_GATE 0x8b0000a0   /* clock enable (WRITE) */
#define TIMER_RST_SET  0x8b000060
#define TIMER_RST_CLR  0x8b000064

#define INT_ENABLE_REG 0x80000008   /* set-ones-to-enable */
#define TIMER_IRQ_MASK (1 << 23)

void tick_start(unsigned int interval_in_ms)
{
    /* Clock enable + reset (fpdoom lcd_mono.h:296-308 verbatim; the clock
     * gate is a WRITE — 0x410000 for timer 2 — and the reset goes to
     * 0x8b000060 then the +4 half 0x8b000064). */
    REG32(TIMER_CLK_GATE) = 0x410000u;
    REG32(TIMER_RST_SET)  = 0x2000u;
    udelay(1000);
    REG32(TIMER_RST_CLR)  = 0x2000u;
    udelay(1000);

    /* Enable IRQ line 23 (a WRITE, per fpdoom). */
    REG32(INT_ENABLE_REG) = TIMER_IRQ_MASK;

    REG32(TIMER2_CTL)  = 0;                     /* ctl off while configuring */
    REG32(TIMER2_LOAD) = TIMER2_LOAD_1MS * interval_in_ms;
    REG32(TIMER2_INT)  = 1;                     /* int enable */
    REG32(TIMER2_CTL)  = 0xc0;                  /* ctl: enable */
}

#ifdef BOOTLOADER
void tick_stop(void)
{
    REG32(INT_ENABLE_REG) = 0;
    REG32(TIMER2_CTL) = 0;
    REG32(TIMER2_INT) = 9;   /* clear pending, keep disabled */
}
#endif /* BOOTLOADER */

/* 1 ms tick ISR (system-sc6530c.c irq_handler dispatches line 23 here).
 * Runs through Rockbox's tick task list, then clears the timer's OWN
 * pending bit — never touch the INTC (see file header). */
void TIMER23(void)
{
    call_tick_tasks();

    if (REG32(TIMER2_INT) & 4)
        REG32(TIMER2_INT) = 9;
}
