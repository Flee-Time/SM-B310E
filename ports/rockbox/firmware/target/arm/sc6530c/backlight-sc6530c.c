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
#include "kernel.h"
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
 *     sys_brightness duty: bits 0-4 duty, bit 6 ON, and the 0x1100
 *     "flash off / pwr" invariant bits. Bit 5 is OFF: the stock on/off
 *     function (NOR 0x1a186) clears bits 5-6, then sets bit 6 (on) or
 *     bit 5 (off). The crt0 boot markers and os.bin both use this
 *     channel. 0..100 percent -> duty 1..32 (+ bit 6).
 *   - 0x82001224  ANA_KEYLIGHT — the 4-bit KEYPAD light. level (value-1)<<4
 *     in bits 4-7, bit 5 = ON, bit 4 = OFF. Driven as Rockbox's button
 *     light with its own timeout. The level field has no visible effect
 *     on hardware, so it is on/off only.
 *
 * Rockbox's brightness setting (MIN..MAX_BRIGHTNESS_SETTING = 1..15) scales
 * to percent for the LCD channel.
 *
 * Vibrator (stock _ANA_SetVibrator 0x1a2c4, HW-verified by B310E-OS):
 * power gate 0x82001154 = 0xA1B2 around the access, ON in 0x82001240
 * bit 0, strength 0x82001244 bit 15 + bits 8-10.
 */

#define REG32(a) (*(volatile uint32_t *)(a))

#define LED_ANA_LED_CTRL     0x82001220u   /* LCD backlight (fpdoom duty) */
#define LED_BL_MASK          0x7fu         /* duty 0-4, OFF bit 5, ON bit 6 */
#define LED_BL_OFF           0x20u
#define LED_BL_ON            0x40u
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

/* LCD backlight, 0..100 percent: fpdoom sys_brightness's duty
 * (syscode.c:225-238, 0..100 -> duty 1..32) with the stock on/off order
 * (NOR 0x1a186): clear ON and OFF, then set ON, or OFF for level 0.
 * fpdoom's off only cleared ON; with neither bit set the LED kept a faint
 * glow on the phone (seen in the B310E OS, 2026-11-07). */
static void backlight_set(int percent)
{
    uint32_t tmp, v;

    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;

    v = (uint32_t)((percent * 32 + 50) / 100);
    tmp = adi_read(LED_ANA_LED_CTRL) & ~(uint32_t)LED_BL_MASK;
    tmp |= LED_FLASH_OFF;
    if (v)
        tmp |= v - 1u;                    /* duty (v-1) */
    adi_write(LED_ANA_LED_CTRL, tmp);     /* neither on nor off */
    adi_write(LED_ANA_LED_CTRL, tmp | (v ? LED_BL_ON : LED_BL_OFF));
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
}

bool backlight_hw_init(void)
{
    backlight_set(backlight_brightness * 100 / MAX_BRIGHTNESS_SETTING);
    keylight_set(DEFAULT_BRIGHTNESS_SETTING);
    return true;
}

/* ---- quiet display during headset-mic recording -------------------------
 * The headset mic picks up the LCD bus: clicks stopped on the phone only
 * with the display off (2026-11-06). While recording from the headset
 * mic, panel transfers are limited to 2 Hz, run at full rate for 0.5 s
 * after any key, and stop after 5 s without keys, with the backlight LED
 * off; a key brings both back. The panel keeps its last picture, so the
 * frame is still copied to FRAME, only the transfer is skipped. Phone
 * mic and FM recordings are left alone. */
#define QUIET_REFRESH_TICKS  (HZ / 2)
#define QUIET_KEY_TICKS      (HZ / 2)
#define QUIET_AFTER_TICKS    (HZ * 5)
static bool bl_rockbox_on = true;   /* Rockbox's backlight state */
static bool bl_quiet;               /* LED off for a quiet recording */
static long quiet_last_kick;
static unsigned quiet_kicks, quiet_skipped, quiet_periods;

static bool quiet_gated(void)
{
#ifdef HAVE_RECORDING
    return sc6530_headset_mic_recording();
#else
    return false;
#endif
}

void sc6530_display_quiet_tick(void)
{
    bool quiet = quiet_gated() &&
                 TIME_AFTER(current_tick, sc6530_key_tick + QUIET_AFTER_TICKS);

    if (quiet == bl_quiet)
        return;
    bl_quiet = quiet;
    if (quiet)
        quiet_periods++;
    if (bl_rockbox_on)
        backlight_set(quiet ? 0 :
                      backlight_brightness * 100 / MAX_BRIGHTNESS_SETTING);
}

bool sc6530_lcd_kick_allowed(void)
{
    bool ok = true;

    if (quiet_gated())
    {
        if (bl_quiet)
            ok = false;
        else if (TIME_BEFORE(current_tick, sc6530_key_tick + QUIET_KEY_TICKS))
            ok = true;
        else
            ok = current_tick - quiet_last_kick >= QUIET_REFRESH_TICKS;
    }
    if (ok)
    {
        quiet_last_kick = current_tick;
        quiet_kicks++;
    }
    else
        quiet_skipped++;
    return ok;
}

void sc6530_display_stats(unsigned *kicks, unsigned *skipped,
                          unsigned *quiet_periods_out)
{
    *kicks = quiet_kicks;
    *skipped = quiet_skipped;
    *quiet_periods_out = quiet_periods;
}

void backlight_hw_on(void)
{
    /* Restore the panel and current framebuffer before illuminating it. */
    lcd_awake();
    /* The navigation/wake path (backlight_update_state, the no-fade
     * #else branch) calls backlight_hw_on() ALONE — our config has no
     * CONFIG_BACKLIGHT_FADING, so BACKLIGHT_FADE_IN_THREAD is 0 and
     * backlight.c's "backlight_hw_brightness(backlight_brightness)"
     * re-apply never runs. Hardcoding DEFAULT here reverted the user's
     * brightness on every menu activity — apply the CURRENT setting
     * (backlight.h's extern, kept in sync by backlight_set_brightness). */
    bl_rockbox_on = true;
    if (!bl_quiet)
        backlight_set(backlight_brightness * 100 / MAX_BRIGHTNESS_SETTING);
}

void backlight_hw_off(void)
{
    bl_rockbox_on = false;
    backlight_set(0);
}

/* ---- keypad light as Rockbox's button light (on/off only) --------------- */
void buttonlight_hw_on(void)
{
    keylight_set(MAX_BRIGHTNESS_SETTING);
}

void buttonlight_hw_off(void)
{
    keylight_set(0);
}

/* ---- vibrator ----------------------------------------------------------- */
#define VIB_PWR       0x82001154u
#define VIB_PWR_EN    0xa1b2u
#define VIB_CTRL      0x82001240u
#define VIB_ON        0x0001u
#define VIB_STRENGTH  0x82001244u
#define VIB_STR_EN    0x8000u
#define VIB_STR_MASK  0xf0ffu

void vibrator_set(int level)
{
    uint32_t tmp;
    int old;

    if (level < 0) level = 0;
    if (level > 7) level = 7;
    old = disable_irq_save();
    adi_write(VIB_PWR, VIB_PWR_EN);
    if (level == 0)
        adi_write(VIB_CTRL, adi_read(VIB_CTRL) & ~VIB_ON);
    else
    {
        adi_write(VIB_STRENGTH, adi_read(VIB_STRENGTH) | VIB_STR_EN);
        tmp = (adi_read(VIB_STRENGTH) & VIB_STR_MASK) | (uint32_t)(level - 1) << 8;
        adi_write(VIB_STRENGTH, tmp);
        adi_write(VIB_CTRL, adi_read(VIB_CTRL) | VIB_ON);
    }
    adi_write(VIB_PWR, 0);
    restore_irq(old);
}
