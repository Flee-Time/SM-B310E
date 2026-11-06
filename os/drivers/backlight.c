/* B310E OS — LCD backlight and keypad light, both in the analog die.
 *
 * 0x82001220: LCD backlight, bits 0-5 duty, bit 6 on, 0x1100 kept set
 * (fpdoom sys_brightness, verified on the phone in Rockbox).
 * 0x82001224: keypad light, bit 5 on, bit 4 off (on/off only in practice).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/arch.h"
#include "os/drivers.h"

#define LED_CTRL   0x82001220u
#define KEYLIGHT   0x82001224u

void backlight_set(int percent)
{
    uint32_t v, tmp;

    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    v = (uint32_t)((percent * 32 + 50) / 100);
    tmp = adi_read(LED_CTRL) & ~0x5fu;
    if (v)
        v += 0x40u - 1u;
    adi_write(LED_CTRL, tmp | 0x1100u | v);
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
