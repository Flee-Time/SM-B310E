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

#define ANA_EIC_DATA    0x82001900u
#define ANA_EIC_DMSK    0x82001904u
#define CHARGER_EIC_BIT (1u << 2)
#define GPIO_DATA       0x8a000000u
#define GPIO_DMSK       0x8a000004u
#define GPIO_DIR        0x8a000008u
#define CHARGER_DISABLE (1u << 6)
#define CHARGER_PGB     (1u << 8)
#define CHARGER_CHGSB   (1u << 9)

static bool charger_present;

static bool adi_read(uint32_t addr, uint32_t *value)
{
    int old = disable_irq_save();
    uint32_t n = ADI_BUDGET, data;
    bool ok = false;
    while (!(REG32(ADI_FIFO_STS) & ADI_FIFO_EMPTY))
        if (--n == 0) goto done;
    REG32(0x82000018) = addr & 0xfff;
    n = ADI_BUDGET;
    do {
        data = REG32(0x8200001c);
        if (!(data & (1u << 31))) {
            ok = ((data >> 16) & 0xfff) == (addr & 0xfff);
            if (ok) *value = data & 0xffff;
            break;
        }
    } while (--n);
done:
    restore_irq(old);
    return ok;
}

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
    uint32_t mask;
    int old = disable_irq_save();
    /* e52q7a CHG_PHY_IsChargerPresent at NOR 0x663d4 reads logical
     * EIC18. The table at 0xca92c maps that to analog channel2.
     * Preserve END's channel3 and the existing charger enable (GPIO6). */
    adi_write(0x820010e0, 0x80);
    adi_write(0x820010e4, 0x20);
    if (adi_read(ANA_EIC_DMSK, &mask))
        adi_write(ANA_EIC_DMSK, mask | CHARGER_EIC_BIT);
    REG32(0x8b0000a0) = 1u << 19; /* GPIO clock SET */
    REG32(GPIO_DIR) &= ~(CHARGER_PGB | CHARGER_CHGSB);
    REG32(GPIO_DMSK) |= CHARGER_PGB | CHARGER_CHGSB;
    restore_irq(old);
}

unsigned int power_input_status(void)
{
    uint32_t data;
    if (adi_read(ANA_EIC_DATA, &data))
        charger_present = (data & CHARGER_EIC_BIT) != 0;
    return charger_present ? POWER_INPUT_MAIN_CHARGER : POWER_INPUT_NONE;
}

bool charging_state(void)
{
    if (!power_input_status())
        return false;
    /* NOR 0x66352 / its GetCHGDoneStatus string: PGB is raw GPIO8,
     * CHGSB is GPIO9; PGB low + CHGSB high means complete. PGB high
     * means the charger has no good input. A configured GPIO6 high
     * explicitly disables charging (0x6626c); do not drive it here. */
    uint32_t data = REG32(GPIO_DATA);
    if ((REG32(GPIO_DIR) & REG32(GPIO_DMSK) & CHARGER_DISABLE) &&
        (data & CHARGER_DISABLE))
        return false;
    return (data & (CHARGER_PGB | CHARGER_CHGSB)) == 0;
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
