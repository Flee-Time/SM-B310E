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
#ifndef ADC_TARGET_H
#define ADC_TARGET_H

/*
 * B310E-OS Rockbox port — b310e/target/arm/sc6530c/adc-target.h
 * (GPLv2, Rockbox-derived; modeled on Rockbox's as3525/adc-target.h)
 *
 * Rockbox's adc.h needs a channel enum. The B310E's battery is read by
 * the SC6530 INTERNAL ADC directly from powermgmt-sc6530c.c (the stock
 * adc_phy_v5.c conversion), NOT through Rockbox's adc subsystem — so
 * there are no Rockbox-ADC channels and adc_read() always returns 0.
 */

enum adc_channel
{
    ADC_BATTERY = 0,   /* kept for clarity; adc_read() returns 0 anyway */
    NUM_ADC_CHANNELS
};

#endif /* ADC_TARGET_H */
