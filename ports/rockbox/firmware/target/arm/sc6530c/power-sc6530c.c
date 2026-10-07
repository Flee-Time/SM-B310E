/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2009 by Bob Cousins
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
#include <stdbool.h>
#include "kernel.h"
#include "system.h"
#include "power.h"

/*
 * B310E-OS Rockbox port — b310e/target/arm/sc6530c/power-sc6530c.c
 * (GPLv2, Rockbox-derived; modeled on Rockbox's mini2440/power-mini2440.c
 * and the B310E-OS menu_reboot watchdog sequence, arch/diag_menu_main.c).
 *
 * Stock e52q7a NOR 0x1a42e powers off by disabling IRQ/FIQ and writing
 * the two LDO power-down SET registers. This is separate from reboot.
 */

#define REG32(a) (*(volatile uint32_t *)(a))

#define ANA_LDO_PD_SET0  0x82001180
#define ANA_LDO_PD_SET1  0x82001184

/* Bounded ADI mailbox (fpdoom pattern — the power controls live on the ANA
 * die and is reached through the FIFO bridge). */
#define ADI_FIFO_STS    0x82000020
#define ADI_FIFO_FULL   (1 << 9)
#define ADI_FIFO_EMPTY  (1 << 8)
#define ADI_BUDGET      1000000u

static bool adi_write(uint32_t addr, uint32_t val)
{
    uint32_t n = ADI_BUDGET;

    while (REG32(ADI_FIFO_STS) & ADI_FIFO_FULL)
        if (--n == 0) return false;
    REG32(addr) = val;
    n = ADI_BUDGET;
    while (!(REG32(ADI_FIFO_STS) & ADI_FIFO_EMPTY))
        if (--n == 0) return false;
    return true;
}

void power_init(void)
{
    /* Nothing to do — the B310E boot menu already did chip init. */
}

unsigned int power_input_status(void)
{
    /* No charger detection implemented (M1). */
    return 0;
}

bool charging_state(void)
{
    /* No charging support (M1). */
    return false;
}

void power_off(void)
{
    /* Rockbox has already stopped audio and flushed the filesystem.
     * Serialize the ADI bridge against button/timer interrupt handlers.
     * Stock 0x1a448..52: SET1=0x1f, then SET0=0x3fff. The final store
     * removes the CPU/RAM supplies, so do not wait on ADI afterwards. */
    disable_interrupt(IRQ_FIQ_STATUS);
    REG32(0x8000000c) = 0xffffffff;
    if (!adi_write(ANA_LDO_PD_SET1, 0x1f))
        system_reboot();
    uint32_t budget = ADI_BUDGET;
    while (REG32(ADI_FIFO_STS) & ADI_FIFO_FULL)
        if (--budget == 0)
            system_reboot();
    REG32(ANA_LDO_PD_SET0) = 0x3fff;
    while (1)
        asm volatile ("mcr p15, 0, %0, c7, c0, 4" :: "r" (0) : "memory");
}
