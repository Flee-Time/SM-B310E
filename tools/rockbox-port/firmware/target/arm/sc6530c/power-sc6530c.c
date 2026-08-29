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
 * There is no software power control on the B310E (the END key is the
 * physical power button). power_off() reboots via the SC6530 watchdog
 * (0.5 s reset) — the closest thing to a power cycle the software can do.
 */

#define REG32(a) (*(volatile uint32_t *)(a))

#define WDG_BASE  0x82001480

/* Bounded ADI mailbox (fpdoom pattern — the watchdog lives on the ANA
 * die and is reached through the FIFO bridge). */
#define ADI_RD_CMD      0x82000018
#define ADI_RD_DATA     0x8200001C
#define ADI_FIFO_STS    0x82000020
#define ADI_FIFO_FULL   (1 << 9)
#define ADI_FIFO_EMPTY  (1 << 8)
#define ADI_BUDGET      1000000u

static uint32_t adi_read(uint32_t addr)
{
    uint32_t a = 0, n = ADI_BUDGET;

    REG32(ADI_RD_CMD) = addr & 0xfff;
    while ((a = REG32(ADI_RD_DATA)) >> 31)
        if (--n == 0) break;
    return a & 0xffffu;
}

static void adi_write(uint32_t addr, uint32_t val)
{
    uint32_t n = ADI_BUDGET;

    while (REG32(ADI_FIFO_STS) & ADI_FIFO_FULL)
        if (--n == 0) return;
    REG32(addr) = val;
    n = ADI_BUDGET;
    while (!(REG32(ADI_FIFO_STS) & ADI_FIFO_EMPTY))
        if (--n == 0) return;
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
    /* No software power-down on the B310E; reboot via the watchdog
     * (B310E-OS menu_reboot: LOCK 0xe551, CTRL enable+start, LOAD 0x4000
     * = 0.5 s @ 32768 Hz). */
    adi_write(WDG_BASE + 0x20, 0xe551);
    {
        uint32_t ctrl = adi_read(WDG_BASE + 8);
        adi_write(WDG_BASE + 8, ctrl | 9);
    }
    adi_write(WDG_BASE, 0x4000);
    adi_write(WDG_BASE + 4, 0);
    {
        uint32_t ctrl = adi_read(WDG_BASE + 8);
        adi_write(WDG_BASE + 8, ctrl | 2);
    }
    adi_write(WDG_BASE + 0x20, ~0xe551u);
    while (1)
        ;
}
