/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2007 by Greg White
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
#ifndef SYSTEM_TARGET_H
#define SYSTEM_TARGET_H

/*
 * B310E-OS Rockbox port — b310e/target/arm/sc6530c/system-target.h
 * (GPLv2, Rockbox-derived; modeled on Rockbox's s3c2440/system-target.h)
 */

#include <stdbool.h>
#include <stdint.h>
#include "system-arm.h"
#include "cpucache-arm.h"

/* The SC6530C runs at a fixed 208 MHz (the B310E boot menu set the PLL).
 * There is no adjustable CPU frequency on this port (M1). */
#define CPUFREQ_DEFAULT 208000000
#define CPUFREQ_NORMAL  208000000
#define CPUFREQ_MAX     208000000

/* PSRAM is identity-mapped and cached. There is no uncached alias;
 * drivers must commit DMA source ranges explicitly. */
#define UNCACHED_ADDR(a) (a)

void system_prepare_fw_start(void);
void tick_stop(void);

/* Target-provided busy-wait (no SoC timer dependency — a plain calibrated
 * iteration loop at 208 MHz, see system-sc6530c.c). */
void udelay(unsigned int usecs);

/* GPIO block (0x8a000000) APB clock. fpdoom gpio_init: APB_PWR_ON
 * (0x800080) "GPIO_D" on SC6530. Bit 19 is half of the keypad clock
 * (0x80040), not GPIO: with only that set, the hardware drops every GPIO
 * write and reads 0 (torch test dump, 2026-11-05). QEMU does not model
 * this gate. */
static inline void sc6530_gpio_clock_on(void)
{
    *(volatile unsigned long *)0x8b0000a0 = 0x800080;  /* APB enable SET */
}

/* Torch LED (power-sc6530c.c): GPIO7 enables the U202 LD6816 LDO that
 * feeds LED200. Long-press 0 toggles it (button-sc6530c.c). */
void torch_set(bool on);
bool torch_get(void);

/* Analog die through the ADI mailbox (system-sc6530c.c), the one copy all
 * drivers use. Safe from interrupt context: each access masks IRQs. A read
 * checks the echoed register index and retries; _checked returns false and
 * leaves *value alone on failure, the plain read returns 0 then. A write
 * returns false if the FIFO stayed full (nothing written) or never drained.
 * A read-modify-write that an interrupt handler may race on the same
 * register needs its own disable_irq_save() around both calls. */
extern uint32_t sc6530_adi_read_errors;
bool sc6530_adi_read_checked(uint32_t addr, uint32_t *value);
uint32_t sc6530_adi_read(uint32_t addr);
bool sc6530_adi_write(uint32_t addr, uint32_t val);

#endif /* SYSTEM_TARGET_H */
