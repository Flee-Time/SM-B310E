/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2025 by Sho Tanimoto
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
#ifndef AUDIOHW_SC6530C_H
#define AUDIOHW_SC6530C_H

/*
 * B310E-OS Rockbox port — b310e/target/arm/sc6530c/audiohw-sc6530c.h
 * (GPLv2, Rockbox-derived; modeled on Rockbox's export/as3514.h — the
 * "codec header" pulled in by the AUDIOHW_SETTING chain in audiohw.h
 * via the HAVE_SC6530_CODEC define).
 *
 * Playback uses the stock ARM-owned DMA/VBC/on-die DAC route. Rockbox
 * applies software volume before the DMA sink; analog gain stays at
 * the captured stock value. The settings table exposes one master
 * volume control for both channels, with no unverified input/amp controls.
 *
 * This header is processed TWICE per TU class:
 *  - in firmware/sound.c (AUDIOHW_IS_SOUND_C defined) the AUDIOHW_SETTING
 *    call below generates the _audiohw_setting_VOLUME struct;
 *  - everywhere else config.h's empty AUDIOHW_SETTING makes it a no-op.
 */

#include "config.h"

/* One master volume; audiohw_set_volume forwards tenths of a dB to the
 * core PCM scaler before the stereo samples reach the DMA sink. */
#define AUDIOHW_CAPS (MONO_VOL_CAP)

AUDIOHW_SETTING(VOLUME, "dB", 0, 1, -100, 0, -30)

#endif /* AUDIOHW_SC6530C_H */
