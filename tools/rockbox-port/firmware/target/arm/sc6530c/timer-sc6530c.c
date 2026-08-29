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
#include "cpu.h"
#include "system.h"
#include "timer.h"
#include "logf.h"

/*
 * B310E-OS Rockbox port — b310e/target/arm/sc6530c/timer-sc6530c.c
 * (GPLv2, Rockbox-derived; modeled on Rockbox's s3c2440/gigabeat-fx/
 * timer-meg-fx.c — the SC6530C user-timer API).
 *
 * Rockbox's firmware/timer.c calls timer_set()/timer_start()/timer_stop()
 * for user timers (WPS scroll, battery gauge, …). We use SC6530 TIMER 0
 * @ 0x81000000 (26 MHz, IRQ line 4) — TIMER 2 (line 23) is reserved for
 * the Rockbox TICK (kernel-sc6530c.c). Register layout is identical to
 * timer 2 (load @+0, ctl @+8, int @+0xc); the timer clock gate
 * (0x8b0000a0, shared by timers 0-2) is already enabled by tick_start.
 *
 * The ISR calls Rockbox's pfn_timer() then ACKs the timer's OWN source
 * (write 9 to +0xc) — never touch the INTC (0x8000000C is INT_DISABLE).
 */

#define REG32(a) (*(volatile uint32_t *)(a))

#define TIMER0_REG  0x81000000
#define TIMER0_LOAD (TIMER0_REG + 0x00)
#define TIMER0_CTL  (TIMER0_REG + 0x08)
#define TIMER0_INT  (TIMER0_REG + 0x0C)
#define TIMER0_FREQ 26000000u

#define INT_ENABLE_REG 0x80000008
#define TIMER0_IRQ_MASK (1 << 4)

void TIMER0(void)
{
    if (pfn_timer != NULL)
        pfn_timer();

    if (REG32(TIMER0_INT) & 4)
        REG32(TIMER0_INT) = 9;
}

static void stop_timer(void)
{
    REG32(INT_ENABLE_REG) &= ~TIMER0_IRQ_MASK;
    REG32(TIMER0_CTL) = 0;
    REG32(TIMER0_INT) = 9;   /* clear pending, keep disabled */
}

bool timer_set(long cycles, bool start)
{
    bool retval = false;

    if (cycles > 0 && cycles <= (long)0xFFFFFFFFu)
    {
        int oldlevel;

        if (start && pfn_unregister != NULL)
        {
            pfn_unregister();
            pfn_unregister = NULL;
        }

        oldlevel = disable_irq_save();

        stop_timer();
        REG32(TIMER0_LOAD) = (uint32_t)cycles;
        REG32(TIMER0_INT)  = 1;             /* int enable */
        REG32(TIMER0_CTL)  = 0xc0;          /* ctl: enable */

        restore_irq(oldlevel);

        retval = true;
    }

    return retval;
}

bool timer_start(void)
{
    int oldstatus = disable_interrupt_save(IRQ_FIQ_STATUS);

    stop_timer();

    /* (Re)arm: reload + start + enable the IRQ line. */
    REG32(TIMER0_INT)  = 1;
    REG32(TIMER0_CTL)  = 0xc0;
    REG32(INT_ENABLE_REG) |= TIMER0_IRQ_MASK;

    restore_interrupt(oldstatus);
    return true;
}

void timer_stop(void)
{
    int oldlevel = disable_irq_save();

    stop_timer();

    restore_irq(oldlevel);
}
