/*
 * B310E-OS — drivers/led.h
 *
 * LED driver for the Samsung SM-B310E (SC6530C): LCD backlight, torch,
 * keypad light, and vibrator. The SC6530 drives these from the ADI analog
 * window via the ADI mailbox (same primitives the LCD backlight and EIC
 * keypad already use).
 *
 * Register facts:
 *   - 0x82001220 (ANA_LED_CTRL): backlight — bits 0-5 duty, bit 6 enable,
 *     bit 8 flash channel, bit 12 flash power. fpdoom syscode.c:221-246.
 *   - 0x82001224: keypad light — bits 4-7 = 4-bit level, bit 5 = ON,
 *     bit 4 = OFF. Dump-mined from stock firmware (keylight fn @ 0x01A078).
 *   - 0x82001240: vibrator ON/OFF — bit 0. 0x82001244: intensity — bit 15
 *     + bits 8-10. 0x82001154: power gate — 0xA1B2 enable / 0 disable.
 *     Dump-mined from stock _ANA_SetVibrator @ 0x01A2C4.
 *
 * SAFETY: same as the other drivers — no 0x8cxxxxxx pinmux writes, RAM
 * only, bounded ADI mailbox waits (never freeze the scheduler).
 *
 * HOST_TEST: hardware paths compile out; API links for host tests.
 */

#ifndef B310E_OS_DRIVERS_LED_H
#define B310E_OS_DRIVERS_LED_H

#include <stdint.h>

#include "kernel.h"     /* module_t */

#ifdef __cplusplus
extern "C" {
#endif

/* Kernel module (registered before lcd: led_init just arms the ADI path;
 * the actual backlight-on happens via led_backlight_set from lcd_init). */
extern const module_t led_module;

/* Set the LCD backlight level (0 = off, 100 = full). fpdoom sys_brightness
 * port (syscode.c:221-246, SC6530 path): scales to 0..32, writes the duty
 * into ANA_LED_CTRL bits 0-5 with bit 6 (enable) set, keeps the flash
 * channel OFF (0x1100). Safe to call any time after chip init. */
void led_backlight_set(int level);

/* Set the keypad light level (0 = off, 1-15). 0x82001224 bits 4-7 =
 * 4-bit level, bit 5 = ON, bit 4 = OFF. Dump-mined from the stock keylight
 * function @ 0x01A078 — fpdoom has zero code for this channel. */
void led_keylight_set(int level);

/* Set the vibrator level (0 = off, 1-7). 0x82001240 bit 0 = ON/OFF,
 * 0x82001244 bit 15 + bits 8-10 = intensity, power gate 0x82001154
 * (0xA1B2 / 0). Dump-mined from stock _ANA_SetVibrator @ 0x01A2C4. */
void led_vibrator_set(int level);

/* 1 while the backlight is reported on, 0 otherwise. Reads the WHTLED
 * status bit (ANA reg 0x82001320 bit 2, active-low — fpdoom
 * is_whtled_on, syscode.c:457-462). */
int led_backlight_is_on(void);

/* Torch / flashlight toggle. ANA_LED_CTRL (0x82001220) bits 10/11 +
 * 14/15 — dump-mined from the stock torch function @ 0x01A1CE. NOTE: the
 * schematic shows the B310E torch is an LD6816 EN-only GPIO, so this ANA
 * path is likely NOT the real torch; kept until the GPIO pin is found via
 * led_torch_probe_next(). */
void led_keypad_set(int on);

/* Torch GPIO sweep probe. The B310E torch is an LD6816CX4C33P LED driver
 * with a single TORCH_EN line (EN-only — no serial protocol). This walks
 * EVERY SC6530 GPIO (0-159) driving each HIGH, auto-advancing per call.
 * Call every ~1.5s; the user watches which pin lights the torch and
 * reports the id, which we then hardcode as led_keypad_set(). */
int led_torch_probe_next(void);

#ifdef __cplusplus
}
#endif

#endif /* B310E_OS_DRIVERS_LED_H */
