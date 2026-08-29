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
#ifndef _BUTTON_TARGET_H_
#define _BUTTON_TARGET_H_

/*
 * B310E-OS Rockbox port — b310e/target/arm/sc6530c/button-target.h
 * (GPLv2, Rockbox-derived; modeled on Rockbox's mini2440/button-target.h)
 *
 * B310E keypad (real matrix, dump_firmware.bin @0xc6e70 — see the B310E-OS
 * drivers/keypad.c s_keytrn):
 *
 *              col0    col1    col2    col3    col4
 *  row0        CENTER  DIAL    -       -       LEFT
 *  row1        1       2       3       LSOFT   DOWN
 *  row2        4       5       6       RSOFT   -
 *  row3        7       8       9       -       UP
 *  row4        *       0       #       -       RIGHT
 *
 * END is NOT in the matrix — it is the EIC power button (0x82001900 ch3).
 *
 * Rockbox button map (bits kept below 0x02000000 = BUTTON_REL and
 * 0x04000000 = BUTTON_REPEAT):
 *   d-pad = directions; CENTER = SELECT; DIAL = PLAY; LSOFT = MENU;
 *   RSOFT = BACK; END = POWER; 2 = VOL_UP; 8 = VOL_DOWN;
 *   4/6 = PREV/NEXT; STAR = HOME; HASH = HASH; remaining digits = 0/1/3/5/7/9.
 */

#define BUTTON_UP           0x00000001
#define BUTTON_DOWN         0x00000002
#define BUTTON_LEFT         0x00000004
#define BUTTON_RIGHT        0x00000008
#define BUTTON_SELECT       0x00000010
#define BUTTON_PLAY         0x00000020
#define BUTTON_MENU         0x00000040
#define BUTTON_BACK         0x00000080
#define BUTTON_POWER        0x00000100
#define BUTTON_VOL_UP       0x00000200
#define BUTTON_VOL_DOWN     0x00000400
#define BUTTON_HOME         0x00000800
#define BUTTON_NEXT         0x00001000
#define BUTTON_PREV         0x00002000
#define BUTTON_HASH         0x00004000
#define BUTTON_0            0x00008000
#define BUTTON_1            0x00010000
#define BUTTON_3            0x00020000
#define BUTTON_5            0x00040000
#define BUTTON_7            0x00080000
#define BUTTON_9            0x00100000

#define BUTTON_MAIN (BUTTON_UP|BUTTON_DOWN|BUTTON_LEFT|BUTTON_RIGHT  \
                    |BUTTON_SELECT|BUTTON_PLAY|BUTTON_MENU|BUTTON_BACK \
                    |BUTTON_POWER|BUTTON_VOL_UP|BUTTON_VOL_DOWN        \
                    |BUTTON_HOME|BUTTON_NEXT|BUTTON_PREV|BUTTON_HASH   \
                    |BUTTON_0|BUTTON_1|BUTTON_3|BUTTON_5|BUTTON_7|BUTTON_9)

/* Software power-off (END held) */
#define POWEROFF_BUTTON     BUTTON_POWER
#define POWEROFF_COUNT      10

#endif /* _BUTTON_TARGET_H_ */
