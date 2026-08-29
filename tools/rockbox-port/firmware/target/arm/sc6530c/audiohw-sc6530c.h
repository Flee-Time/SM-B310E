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
 * The SC6530C on-die codec has NO ARM-controllable data path (it is
 * DSP/NV-driven; the ARM register chain produces no sound — see the
 * B310E-OS audio notes). M1 declares a MONO volume setting so the sound
 * menu exists; every audiohw_set_* is a documented no-op. The volume
 * scale is centibels (sound.c's requirement for VOLUME).
 *
 * This header is processed TWICE per TU class:
 *  - in firmware/sound.c (AUDIOHW_IS_SOUND_C defined) the AUDIOHW_SETTING
 *    call below generates the _audiohw_setting_VOLUME struct;
 *  - everywhere else config.h's empty AUDIOHW_SETTING makes it a no-op.
 */

#include "config.h"

/* Mono volume only — the sink's software volume (PCM_SINK_SWVOL) does the
 * real scaling; this entry just keeps the settings table honest. */
#define AUDIOHW_CAPS (MONO_VOL_CAP)

AUDIOHW_SETTING(VOLUME, "dB", 0, 1, -100, 0, -30)

#endif /* AUDIOHW_SC6530C_H */
