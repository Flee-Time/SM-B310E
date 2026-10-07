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
#include "i2c.h"

/*
 * B310E-OS Rockbox port — b310e/target/arm/sc6530c/i2c-sc6530c.c
 * (GPLv2, Rockbox-derived)
 *
 * apps/main.c calls i2c_init() unconditionally; the SC6530C has no I2C
 * peripherals on this board (CONFIG_I2C I2C_NONE), so the init is a
 * no-op and the rest of the i2c API is never referenced (i2c.h only
 * declares i2c_init without CONFIG_I2C guards).
 */

void i2c_init(void)
{
    /* No I2C hardware on the B310E. */
}
