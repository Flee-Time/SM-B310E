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
#include "config.h"
#include "cpu.h"
#include "system.h"
#include "backlight-target.h"
#include "backlight.h"
#include "lcd.h"

/*
 * B310E-OS Rockbox port — b310e/target/arm/sc6530c/backlight-sc6530c.c
 * (GPLv2, Rockbox-derived; modeled on Rockbox's mini2440/backlight-mini2440.c
 * and the B310E-OS drivers/led.c — led_backlight_set (fpdoom sys_brightness,
 * HW-verified) + led_keylight_set (dump-mined from the stock keylight
 * function @0x01A078)).
 *
 * TWO ANA channels, both dimmable:
 *   - 0x82001220  ANA_LED_CTRL — the REAL LCD backlight. fpdoom
 *     sys_brightness duty: bits 0-5 duty, bit 6 ON, and the 0x1100
 *     "flash off / pwr" invariant bits. The crt0 boot markers and os.bin
 *     both use this channel. 0..100 percent -> duty 1..32 (+ bit 6).
 *   - 0x82001224  ANA_KEYLIGHT — the 4-bit KEYPAD light. level (value-1)<<4
 *     in bits 4-7, bit 5 = ON, bit 4 = OFF. Mirrored at the same level so
 *     screen + keys glow together like the stock phone.
 *
 * Rockbox's brightness setting (MIN..MAX_BRIGHTNESS_SETTING = 1..15) scales
 * to percent for the LCD channel and maps directly onto the keypad level.
 */

#define REG32(a) (*(volatile uint32_t *)(a))

#define LED_ANA_LED_CTRL     0x82001220u   /* LCD backlight (fpdoom duty) */
#define LED_BL_MASK          0x5fu         /* duty bits 0-5 + enable bit 6 */
#define LED_FLASH_OFF        0x1100u       /* flash-off / pwr invariant   */

#define LED_ANA_KEYLIGHT     0x82001224u   /* keypad light (4-bit)       */
#define LED_KL_LEVEL_MASK    0xf0u
#define LED_KL_LEVEL_SHIFT   4u
#define LED_KL_ON_BIT        0x20u
#define LED_KL_OFF_BIT       0x10u
#define LED_KL_MASK          0x30u

/* Bounded ADI mailbox (fpdoom/B310E-OS drivers/led.c pattern). */
#define ADI_RD_CMD      0x82000018
#define ADI_RD_DATA     0x8200001C
#define ADI_FIFO_STS    0x82000020
#define ADI_FIFO_FULL   (1 << 9)
#define ADI_FIFO_EMPTY  (1 << 8)
#define ADI_BUDGET      1000000u

static uint32_t adi_read(uint32_t addr)
{
    uint32_t a = 0, n = ADI_BUDGET;

    REG32(ADI_RD_CMD) = addr & 0xfff;
    while ((a = REG32(ADI_RD_DATA)) >> 31)
        if (--n == 0) break;
    return a & 0xffffu;
}

static void adi_write(uint32_t addr, uint32_t val)
{
    uint32_t n = ADI_BUDGET;

    while (REG32(ADI_FIFO_STS) & ADI_FIFO_FULL)
        if (--n == 0) return;
    REG32(addr) = val;
    n = ADI_BUDGET;
    while (!(REG32(ADI_FIFO_STS) & ADI_FIFO_EMPTY))
        if (--n == 0) return;
}

/* LCD backlight, 0..100 percent — led_backlight_set VERBATIM (fpdoom
 * sys_brightness, syscode.c:225-238): 0..100 -> duty 1..32 (+ bit 6 ON);
 * level 0 -> duty 0, ON clear = off. */
static void backlight_set(int percent)
{
    uint32_t tmp, v;

    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;

    v = (uint32_t)((percent * 32 + 50) / 100);
    tmp = adi_read(LED_ANA_LED_CTRL) & ~(uint32_t)LED_BL_MASK;
    if (v)
        v += 0x40u - 1u;                  /* bit 6 (ON) + duty (v-1) */
    tmp |= LED_FLASH_OFF | v;
    adi_write(LED_ANA_LED_CTRL, tmp);
}

/* Keypad light, 0 = off, 1..15 = level (led_keylight_set verbatim logic).
 * NOT static: the pcm sink's diagnostic instrument blinks it per buffer. */
void keylight_set(int level)
{
    uint32_t tmp;

    if (level < 0) level = 0;
    if (level > MAX_BRIGHTNESS_SETTING) level = MAX_BRIGHTNESS_SETTING;

    tmp = adi_read(LED_ANA_KEYLIGHT);
    if (level == 0) {
        tmp &= ~(uint32_t)LED_KL_MASK;
        adi_write(LED_ANA_KEYLIGHT, tmp);
        tmp = adi_read(LED_ANA_KEYLIGHT) | LED_KL_OFF_BIT;
    } else {
        tmp = (tmp & ~(uint32_t)LED_KL_LEVEL_MASK) |
              (uint32_t)(level - 1) << LED_KL_LEVEL_SHIFT;
        adi_write(LED_ANA_KEYLIGHT, tmp);
        tmp = (adi_read(LED_ANA_KEYLIGHT) & ~(uint32_t)LED_KL_MASK) |
              LED_KL_ON_BIT;
    }
    adi_write(LED_ANA_KEYLIGHT, tmp);
}

void backlight_hw_brightness(int brightness)
{
    if (brightness < MIN_BRIGHTNESS_SETTING)
        brightness = MIN_BRIGHTNESS_SETTING;
    else if (brightness > MAX_BRIGHTNESS_SETTING)
        brightness = MAX_BRIGHTNESS_SETTING;

    /* Scale the 1..15 setting to percent for the LCD, mirror the keypad
     * at the same level (stock phone glows both together). */
    backlight_set(brightness * 100 / MAX_BRIGHTNESS_SETTING);
    keylight_set(brightness);
}

bool backlight_hw_init(void)
{
    backlight_set(backlight_brightness * 100 / MAX_BRIGHTNESS_SETTING);
    keylight_set(backlight_brightness);
    return true;
}

void backlight_hw_on(void)
{
    /* The navigation/wake path (backlight_update_state, the no-fade
     * #else branch) calls backlight_hw_on() ALONE — our config has no
     * CONFIG_BACKLIGHT_FADING, so BACKLIGHT_FADE_IN_THREAD is 0 and
     * backlight.c's "backlight_hw_brightness(backlight_brightness)"
     * re-apply never runs. Hardcoding DEFAULT here reverted the user's
     * brightness on every menu activity — apply the CURRENT setting
     * (backlight.h's extern, kept in sync by backlight_set_brightness). */
    backlight_set(backlight_brightness * 100 / MAX_BRIGHTNESS_SETTING);
    keylight_set(backlight_brightness);
}

void backlight_hw_off(void)
{
    backlight_set(0);
    keylight_set(0);
}
