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

#include "system-arm.h"
#include "cpucache-arm.h"

/* The SC6530C runs at a fixed 208 MHz (the B310E boot menu set the PLL).
 * There is no adjustable CPU frequency on this port (M1). */
#define CPUFREQ_DEFAULT 208000000
#define CPUFREQ_NORMAL  208000000
#define CPUFREQ_MAX     208000000

/* The crt0 identity MMU maps VA == PA everywhere and nothing is cached in
 * M1, so there is no separate uncached alias region. */
#define UNCACHED_ADDR(a) (a)

void system_prepare_fw_start(void);
void tick_stop(void);

/* Target-provided busy-wait (no SoC timer dependency — a plain calibrated
 * iteration loop at 208 MHz, see system-sc6530c.c). */
void udelay(unsigned int usecs);

#endif /* SYSTEM_TARGET_H */
