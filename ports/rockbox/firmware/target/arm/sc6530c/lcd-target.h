/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2009 by Bob Cousins, Lyre Project
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
#ifndef LCD_TARGET_H
#define LCD_TARGET_H

/*
 * B310E-OS Rockbox port — b310e/target/arm/sc6530c/lcd-target.h
 * (GPLv2, Rockbox-derived; modeled on Rockbox's mini2440/lcd-target.h)
 *
 * Rockbox's framebuffer (lcd-memframe.c) is copied to FRAME — the fixed
 * physical LCDC DMA source (firmware/export/sc6530c.h).
 */

#define LCD_FRAMEBUF_ADDR(col, row) ((fb_data *)FRAME + (row)*LCD_WIDTH + (col))

#endif /* LCD_TARGET_H */
