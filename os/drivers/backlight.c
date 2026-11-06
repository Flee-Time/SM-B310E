/* B310E OS — LCD backlight, keypad light and vibrator, all in the analog die.
 *
 * 0x82001220: LCD backlight. Bits 0-4 current (duty), bit 5 off, bit 6
 * on, 0x1100 kept set (fpdoom sys_brightness, verified on the phone in
 * Rockbox). The stock on/off function (NOR 0x1a186) clears bits 5 and 6,
 * then sets bit 6 to switch on or bit 5 to switch off; its level function
 * (0x1a154) writes the current to bits 0-4. fpdoom's "off" (and ours until
 * 2026-11-07) only cleared bit 6: with neither bit set the LED kept a
 * faint glow on the phone.
 * 0x82001224: keypad light, bit 5 on, bit 4 off (on/off only in practice).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/arch.h"
#include "os/drivers.h"

#define LED_CTRL   0x82001220u
#define LED_KEEP   0x1100u
#define LED_DUTY   0x1fu
#define LED_OFF    0x20u
#define LED_ON     0x40u

#define KEYLIGHT   0x82001224u

void backlight_set(int percent)
{
    uint32_t v, tmp;

    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    v = (uint32_t)((percent * 32 + 50) / 100);      /* 0, or current 1..32 */
    tmp = adi_read(LED_CTRL) & ~(LED_DUTY | LED_OFF | LED_ON);
    tmp |= LED_KEEP;
    if (v)
        tmp |= v - 1u;
    adi_write(LED_CTRL, tmp);                       /* neither on nor off */
    adi_write(LED_CTRL, tmp | (v ? LED_ON : LED_OFF));
}

void keylight_set(bool on)
{
    uint32_t tmp = adi_read(KEYLIGHT);

    if (on) {                           /* level 15, then on */
        adi_write(KEYLIGHT, (tmp & ~0xf0u) | 0xe0u);
        adi_write(KEYLIGHT, (adi_read(KEYLIGHT) & ~0x30u) | 0x20u);
    } else {
        adi_write(KEYLIGHT, tmp & ~0x30u);
        adi_write(KEYLIGHT, adi_read(KEYLIGHT) | 0x10u);
    }
}

/* Vibrator (stock _ANA_SetVibrator, NOR 0x1a2c4; Rockbox backlight-sc6530c.c,
 * verified on the phone): power gate 0x82001154 = 0xa1b2 around the
 * access, on/off 0x82001240 bit 0, strength 0x82001244 bit 15 + bits 8-10
 * (level - 1). Level 0 off, 1..7. */
#define VIB_PWR       0x82001154u
#define VIB_PWR_EN    0xa1b2u
#define VIB_CTRL      0x82001240u
#define VIB_ON        0x0001u
#define VIB_STRENGTH  0x82001244u
#define VIB_STR_EN    0x8000u
#define VIB_STR_MASK  0xf0ffu

void vibrator_set(int level)
{
    uint32_t s, tmp;

    if (level < 0) level = 0;
    if (level > 7) level = 7;
    s = arch_irq_save();
    adi_write(VIB_PWR, VIB_PWR_EN);
    if (level == 0) {
        adi_write(VIB_CTRL, adi_read(VIB_CTRL) & ~VIB_ON);
    } else {
        adi_write(VIB_STRENGTH, adi_read(VIB_STRENGTH) | VIB_STR_EN);
        tmp = (adi_read(VIB_STRENGTH) & VIB_STR_MASK) | (uint32_t)(level - 1) << 8;
        adi_write(VIB_STRENGTH, tmp);
        adi_write(VIB_CTRL, adi_read(VIB_CTRL) | VIB_ON);
    }
    adi_write(VIB_PWR, 0);
    arch_irq_restore(s);
}
