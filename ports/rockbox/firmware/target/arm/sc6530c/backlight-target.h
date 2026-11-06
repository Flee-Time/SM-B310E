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

void buttonlight_hw_on(void);
void buttonlight_hw_off(void);
void keylight_set(int level);          /* raw keypad light, 0..15 */
/* Quiet display for headset-mic recordings (backlight-sc6530c.c): the LCD
 * bus couples into the headset mic line. 2 Hz refresh, full rate for
 * 0.5 s after a key, and after 5 s without keys no refresh and the
 * backlight LED off. sc6530_display_quiet_tick runs from the button
 * tick; the LCD driver asks sc6530_lcd_kick_allowed before each panel
 * transfer. */
extern volatile long sc6530_key_tick;
bool sc6530_headset_mic_recording(void);
void sc6530_display_quiet_tick(void);
bool sc6530_lcd_kick_allowed(void);
void sc6530_display_stats(unsigned *kicks, unsigned *skipped,
                          unsigned *quiet_periods);

/* Vibrator motor (stock _ANA_SetVibrator 0x1a2c4): 0 = off, 1..7 = strength. */
void vibrator_set(int level);

#endif /* BACKLIGHT_TARGET_H */
