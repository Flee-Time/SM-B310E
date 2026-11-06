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

/*
 * B310E USB glue. The micro-USB socket is the only power input, so VBUS is
 * the charger-present EIC line (power-sc6530c.c). A wall charger and a PC
 * look the same until a host sends its first request: USB_DETECT_BY_REQUEST
 * keeps Rockbox playing on a charger and only takes the card over once a
 * host has spoken.
 */

#include "config.h"
#include "system.h"
#include "usb.h"
#include "usb_core.h"
#include "usb_drv.h"
#include "power.h"

void usb_drv_connect(void);   /* usb-drv-sc6530c.c */

void usb_init_device(void)
{
    usb_drv_startup();
}

int usb_detect(void)
{
    return power_input_status() ? USB_INSERTED : USB_EXTRACTED;
}

void usb_attach(void)
{
    usb_enable(true);
}

void usb_enable(bool on)
{
    if (on) {
        usb_core_init();
        /* attach only once the core and class drivers are set up */
        usb_drv_connect();
    } else {
        usb_core_exit();
    }
}
