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
#ifndef BACKLIGHT_TARGET_H
#define BACKLIGHT_TARGET_H

/*
 * B310E-OS Rockbox port — b310e/target/arm/sc6530c/backlight-target.h
 * (GPLv2, Rockbox-derived; modeled on Rockbox's mini2440/backlight-target.h)
 *
 * The B310E keypad light (ANA 0x82001224) is the backlight; 4-bit dimmable
 * (MIN..MAX_BRIGHTNESS_SETTING = 1..15, see config/b310e.h).
 */

bool backlight_hw_init(void);
void backlight_hw_on(void);
void backlight_hw_off(void);
void backlight_hw_brightness(int brightness);

#endif /* BACKLIGHT_TARGET_H */
