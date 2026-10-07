/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
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
#include "adc.h"
#include "system.h"

/*
 * B310E-OS Rockbox port — b310e/target/arm/sc6530c/adc-sc6530c.c
 * (GPLv2, Rockbox-derived; modeled on Rockbox's s3c2440/adc-s3c2440.c)
 *
 * Rockbox's adc.h (included by apps/debug_menu.c etc.) needs adc_init()
 * and adc_read(). The B310E's battery is read by the SC6530 INTERNAL ADC
 * directly in powermgmt-sc6530c.c (the stock adc_phy_v5.c conversion) —
 * Rockbox's adc subsystem is not wired, so adc_read() returns 0. The
 * debug menu's battery line comes from battery_read_info() (powermgmt),
 * not from adc_read().
 */

void adc_init(void)
{
    /* Nothing to do — no Rockbox-ADC channels. */
}

unsigned short adc_read(int channel)
{
    (void)channel;
    return 0;
}
