/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2009 by Bob Cousins, Lyre Project
 * Copyright (C) 2009 by Jorge Pinto, Lyre Project
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
 * B310E-OS Rockbox port — b310e/config/b310e.h
 * (GPLv2, Rockbox-derived; modeled on Rockbox's config/mini2440.h)
 *
 * Target config for the Samsung SM-B310E "Guru Music 2" (Spreadtrum SC6530C,
 * ARM926EJ-S @ 208 MHz, 4 MB PSRAM @ 0x04000000, 128x160 16bpp ST7735).
 * Rockbox is linked FIXED at 0x04000000 (DRAMORIG in app.lds) and launched
 * by the B310E boot menu like an fpdoom game.
 */

/* For Rolo and boot loader */
#define MODEL_NUMBER 126
#define MODEL_NAME "B310E"

/***************************************************************************/
/* Hardware Config */

#define CONFIG_SDRAM_START 0x04000000

/* Flash storage */
#define HAVE_FLASH_STORAGE
/* define the storage type */
#define CONFIG_STORAGE STORAGE_SD

#define HAVE_MULTIVOLUME
#define HAVE_HOTSWAP
#define HAVE_HOTSWAP_STORAGE_AS_MAIN
#define INCLUDE_TIMEOUT_API

/* Display */
#define HAVE_LCD_COLOR
/* ST7735S BOE 128x160, RGB565, refreshed by the SC6530 LCDC DMA from a
 * fixed physical framebuffer (see firmware/export/sc6530c.h FRAME). */
#define CONFIG_LCD LCD_B310E
#define LCD_WIDTH  128
#define LCD_HEIGHT 160
/* sqrt(128^2 + 160^2) / 1.8" screen = 112 dpi */
#define LCD_DPI 112
#define LCD_DEPTH  16          /* 65536 colours */
#define LCD_PIXELFORMAT RGB565 /* rgb565 */

/* The B310E keypad light is 4-bit dimmable (ANA 0x82001224, see the
 * B310E-OS drivers/led.c led_keylight_set). Rockbox backlight = keylight. */
#define HAVE_BACKLIGHT
#define HAVE_BACKLIGHT_BRIGHTNESS
/* Main LCD backlight brightness range and defaults (1..15, 4-bit) */
#define MIN_BRIGHTNESS_SETTING          1
#define MAX_BRIGHTNESS_SETTING          15
#define DEFAULT_BRIGHTNESS_SETTING      12

/* Keypad */
#define CONFIG_KEYPAD B310E_PAD
#define HAVE_BUTTON_DATA

/* I2C: not used on this SoC */
#define CONFIG_I2C I2C_NONE

/* On-die SC6530C codec (data path is DSP/VBC driven; the ARM codec chain
 * produces no sound — see firmware/target/arm/sc6530c/audiohw-sc6530c.h) */
#define HAVE_SC6530_CODEC

/* The only honest hardware rate is the 8 kHz VBC path. SAMPR_CAP_44 is
 * added ONLY because pcm_sampr.h's HW_FREQ_DEFAULT chain #errors without
 * 44 or 48 kHz ("Neither 48 or 44KHz supported?") — a shared-file
 * constraint, not a hardware lie. The sink's own caps table (pcm-sc6530c.c)
 * stays {8000}; pcm_set_frequency() collapses every request to 8 kHz. */
#define HW_SAMPR_CAPS (SAMPR_CAP_8 | SAMPR_CAP_44)

/* Battery: SM-B310E packs an 800 mAh Li-ion (BATTERY_CAPACITY_DEFAULT).
 * VBAT is read by the SC6530 internal ADC, channel 5. */
#define BATTERY_CAPACITY_DEFAULT 800   /* default battery capacity */
#define BATTERY_CAPACITY_MIN      800  /* min. capacity selectable */
#define BATTERY_CAPACITY_MAX      800  /* max. capacity selectable */
#define BATTERY_CAPACITY_INC       50  /* capacity increment */

#define CONFIG_BATTERY_MEASURE VOLTAGE_MEASURE

/***************************************************************************/
/* Application Config */

#define HAVE_ALBUMART
/* define this to enable bitmap scaling */
#define HAVE_BMP_SCALING
/* define this to enable JPEG decoding */
#define HAVE_JPEG

#define HAVE_QUICKSCREEN

/* define this if you have a real-time clock (SC6530 on-die RTC) */
#define CONFIG_RTC RTC_SC6530

/* The number of bytes reserved for loadable codecs */
#define CODEC_SIZE 0x80000

/* The number of bytes reserved for loadable plugins */
#define PLUGIN_BUFFER_SIZE 0x10000

#define CONFIG_CPU SC6530C

/* Define this to the CPU frequency */
#define CPU_FREQ      208000000
#define SLOW_CLOCK        32768

/* USB */
#define USB_NONE

/* No hardware volume control — the mixer applies software volume and feeds
 * the sink already-scaled 16-bit samples (volume_type = PCM_SINK_SWVOL). */
#define HAVE_SW_VOLUME_CONTROL

#define BOOTFILE_EXT "b310e"
#define BOOTFILE "rockbox.bin"
#define BOOTDIR "/.rockbox"

/* Define this if a programmable hotkey is mapped */
#define HAVE_HOTKEY
