/*
 * B310E-OS — drivers/led.c
 *
 * LED driver: LCD backlight + torch + keypad light via the SC6530 ADI
 * analog window (ANA_LED_CTRL @ 0x82001220 / 0x82001224), plus the
 * vibrator (ANA 0x82001240/0x82001244). Ported from fpdoom sys_brightness
 * (syscode.c:221-246, Unlicense) for the backlight; the keypad-light and
 * vibrator registers were extracted from dump_firmware.bin (the stock
 * firmware's _ANA_SetVibrator @ 0x01A2C4 and the keylight function @
 * 0x01A078) — fpdoom has zero code for them.
 *
 * Register facts (dump-mined, verified in stock code):
 *   - 0x82001220 backlight: bits 0-4 = 5-bit duty (fpdoom path, verified)
 *   - 0x82001224 keypad light: bits 4-7 = 4-bit level, bit5 = ON, bit4 =
 *     OFF (mask 0x30) — _ANA keylight function @ 0x01A078
 *   - 0x82001240 vibrator ON/OFF: bit 0
 *   - 0x82001244 vibrator intensity: bit 15 + bits 8-10 (value-1)<<8,
 *     mask 0xF0FF
 *   - 0x82001154 vibrator power gate: 0xA1B2 enable / 0 disable
 *
 * SAFETY: ADI mailbox access only (0x82000018/0x1c/0x20) — no 0x8c pinmux
 * writes. Bounded waits only; never freezes the scheduler.
 */

#include "os.h"
#include "../arch/chip.h"
#include "led.h"

/* ---- ADI analog mailbox (SC6530 path — same as lcd.c/keypad.c) --------- */

#define LED_ADI_RD_CMD   0x82000018u
#define LED_ADI_RD_DATA  0x8200001cu
#define LED_ADI_FIFO_STS 0x82000020u
#define LED_ADI_FIFO_FULL  (1u << 9)
#define LED_ADI_FIFO_EMPTY (1u << 8)

/* ANA_LED_CTRL (fpdoom syscode.c:221-246: ctrl = 0x82001000 + 0x220) */
#define LED_ANA_LED_CTRL  (0x82001000u + 0x220u)
#define LED_ANA_WHTLED_ST (0x82001000u + 0x320u)   /* is_whtled_on read */

/* Keypad light: 0x82001224, 4-bit level in bits 4-7, ON bit 5 / OFF bit 4.
 * Dump-mined from the stock keylight function @ 0x01A078 (mask 0x30). */
#define LED_ANA_KEYLIGHT  (0x82001000u + 0x224u)
#define LED_KL_MASK       0x30u
#define LED_KL_ON_BIT     0x20u
#define LED_KL_OFF_BIT    0x10u
#define LED_KL_LEVEL_SHIFT 4u
#define LED_KL_LEVEL_MASK 0xf0u

/* Vibrator: 0x82001240 bit 0 = ON/OFF, 0x82001244 = intensity (bit 15 +
 * bits 8-10), 0x82001154 = power gate. Dump-mined from _ANA_SetVibrator
 * @ 0x01A2C4 (masks 0xF0FF / 0xA1B2). */
#define LED_ANA_VIBRATOR  (0x82001000u + 0x240u)
#define LED_ANA_VIB_INT   (0x82001000u + 0x244u)
#define LED_ANA_VIB_PWR   (0x82001000u + 0x154u)
#define LED_VIB_ON_BIT    0x1u
#define LED_VIB_INT_EN    0x8000u
#define LED_VIB_INT_MASK  0xF0FFu
#define LED_VIB_INT_SHIFT 8u
#define LED_VIB_PWR_EN    0xA1B2u

#define LED_BL_MASK   0x5fu   /* duty bits 0-5 + enable bit 6 (fpdoom) */
#define LED_FLASH_OFF 0x1100u /* SC6530: bit 8 (flash) + bit 12 (pwr) */

#ifndef HOST_TEST
/* Bounded ADI mailbox access: every wait has a hard budget so a wedged
 * ADI bus can never freeze the cooperative scheduler (the waits complete
 * in microseconds on healthy hardware). On timeout the read returns the
 * last value and the write is abandoned — the caller's RMW then no-ops. */
#define LED_ADI_BUDGET 1000000u

static uint32_t led_adi_read(uint32_t addr)
{
    uint32_t a = 0, n = LED_ADI_BUDGET;

    MEM4(LED_ADI_RD_CMD) = addr & 0xfffu;
    while ((a = MEM4(LED_ADI_RD_DATA)) >> 31)   /* wait busy clear */
        if (--n == 0) break;
    return a & 0xffffu;
}

static void led_adi_write(uint32_t addr, uint32_t val)
{
    uint32_t n = LED_ADI_BUDGET;

    while (MEM4(LED_ADI_FIFO_STS) & LED_ADI_FIFO_FULL)  /* FIFO full  */
        if (--n == 0) return;
    MEM4(addr) = val;
    n = LED_ADI_BUDGET;
    /* fpdoom adi_write: wait until FIFO_EMPTY is SET (write drained).
     * The inverted `while (sts & EMPTY)` spins while the FIFO is already
     * empty — the normal post-drain state — burning the full budget per
     * write (the vibrator's 8-op sequence froze the scheduler ~0.5s). */
    while (!(MEM4(LED_ADI_FIFO_STS) & LED_ADI_FIFO_EMPTY))
        if (--n == 0) return;
}
#endif /* !HOST_TEST */

/* ---- public API --------------------------------------------------------- */

void led_backlight_set(int level)
{
#if !defined(HOST_TEST)
    uint32_t tmp, v;

    if (level < 0) level = 0;
    if (level > 100) level = 100;

    /* fpdoom sys_brightness (syscode.c:225-238): 0..100 -> 0..32 duty,
     * then bits 0-5 (duty-1) with bit 6 (enable) set; OR 0x1100 keeps the
     * flashlight channel OFF (fpdoom's "turn off flashlight" invariant). */
    v = (uint32_t)((level * 32 + 50) / 100);
    tmp = led_adi_read(LED_ANA_LED_CTRL) & ~(uint32_t)LED_BL_MASK;
    if (v)
        v += 0x40u - 1u;
    tmp |= LED_FLASH_OFF | v;
    led_adi_write(LED_ANA_LED_CTRL, tmp);
#else
    (void)level;
#endif
}

void led_keylight_set(int level)
{
#if !defined(HOST_TEST)
    uint32_t tmp;

    if (level < 0) level = 0;
    if (level > 15) level = 15;

    /* Stock keylight function @ 0x01A078 (dump-mined): the channel is
     * 0x82001224 with a 4-bit level in bits 4-7; bit 5 (0x20) = ON, bit 4
     * (0x10) = OFF. A zero level clears bits 4-5 and sets the OFF bit;
     * a non-zero level writes (level-1)<<4 and sets the ON bit. */
    tmp = led_adi_read(LED_ANA_KEYLIGHT);
    if (level == 0) {
        tmp &= ~(uint32_t)LED_KL_MASK;
        led_adi_write(LED_ANA_KEYLIGHT, tmp);
        tmp = led_adi_read(LED_ANA_KEYLIGHT) | LED_KL_OFF_BIT;
    } else {
        tmp = (tmp & ~(uint32_t)LED_KL_LEVEL_MASK) |
              (uint32_t)(level - 1) << LED_KL_LEVEL_SHIFT;
        led_adi_write(LED_ANA_KEYLIGHT, tmp);
        tmp = (led_adi_read(LED_ANA_KEYLIGHT) & ~(uint32_t)LED_KL_MASK) |
              LED_KL_ON_BIT;
    }
    led_adi_write(LED_ANA_KEYLIGHT, tmp);
#else
    (void)level;
#endif
}

void led_vibrator_set(int level)
{
#if !defined(HOST_TEST)
    uint32_t tmp;

    if (level < 0) level = 0;
    if (level > 7) level = 7;

    /* Stock _ANA_SetVibrator @ 0x01A2C4 (dump-mined): power gate
     * 0x82001154 = 0xA1B2; ON/OFF in 0x82001240 bit 0; intensity in
     * 0x82001244 bit 15 + bits 8-10 (value-1)<<8, mask 0xF0FF. */
    led_adi_write(LED_ANA_VIB_PWR, LED_VIB_PWR_EN);
    if (level == 0) {
        tmp = led_adi_read(LED_ANA_VIBRATOR) & ~LED_VIB_ON_BIT;
        led_adi_write(LED_ANA_VIBRATOR, tmp);
    } else {
        tmp = led_adi_read(LED_ANA_VIB_INT);
        led_adi_write(LED_ANA_VIB_INT, tmp | LED_VIB_INT_EN);
        tmp = (led_adi_read(LED_ANA_VIB_INT) & LED_VIB_INT_MASK) |
              (uint32_t)(level - 1) << LED_VIB_INT_SHIFT;
        led_adi_write(LED_ANA_VIB_INT, tmp);
        tmp = led_adi_read(LED_ANA_VIBRATOR) | LED_VIB_ON_BIT;
        led_adi_write(LED_ANA_VIBRATOR, tmp);
    }
    led_adi_write(LED_ANA_VIB_PWR, 0);
#else
    (void)level;
#endif
}

int led_backlight_is_on(void)
{
#if !defined(HOST_TEST)
    /* fpdoom is_whtled_on (syscode.c:457-462): bit 2 of the WHTLED status
     * reg is active-low (0 = on). */
    return (led_adi_read(LED_ANA_WHTLED_ST) & 4u) == 0;
#else
    return 0;
#endif
}

/* ANA_LED_CTRL torch/flash channel — dump-mined from the stock torch
 * function @ 0x01A1CE (sibling of _ANA_SetVibrator / _ANA_SetLCMBrightness
 * in the same 0x01A0C0-0x01A340 driver block):
 *   ON  = OR  0x8800 (bit 11 + bit 15), then AND ~0x4400 (0xBBFF)
 *   OFF = OR  0x4400 (bit 10 + bit 14), then AND ~0x8800 (0x77FF)
 * The bits are the LED controller's channel-select pair (10/11) and
 * channel-enable pair (14/15). User-verified: this is the torch the stock
 * OS's "Torch light" app drives. */
#define LED_TORCH_ON_OR   0x8800u
#define LED_TORCH_ON_MASK 0xBBFFu
#define LED_TORCH_OFF_OR  0x4400u
#define LED_TORCH_OFF_MASK 0x77FFu

void led_keypad_set(int on)
{
#if !defined(HOST_TEST)
    uint32_t tmp = led_adi_read(LED_ANA_LED_CTRL);

    if (on) {
        tmp |= LED_TORCH_ON_OR;
        tmp &= LED_TORCH_ON_MASK;
    } else {
        tmp |= LED_TORCH_OFF_OR;
        tmp &= LED_TORCH_OFF_MASK;
    }
    led_adi_write(LED_ANA_LED_CTRL, tmp);
#else
    (void)on;
#endif
}

/* ---- torch GPIO sweep ------------------------------------------------------
 *
 * Schematic fact (user-provided): the B310E torch is an LD6816CX4C33P LED
 * driver with a single TORCH_EN control line from the SC6530 (EN-only, no
 * serial protocol; LED current is hardwired). So the torch = one GPIO
 * driven HIGH. The exact pin is not in the firmware strings; this sweep
 * walks EVERY SC6530 GPIO (ids 0-159, fpdoom max n=0xa0) driving each HIGH
 * as output, auto-advancing so the user can watch which pin lights the LED.
 *
 * SC6530 GPIO layout (fpdoom syscode.c:133-150): ids 0-127 ->
 * 0x8a000000 + ((id>>4)<<7), bit (id&0xf), offsets DATA=0, DMSK=4, DIR=8,
 * IE=0x18; ids 128-159 -> ANA 0x82001580 + ((id>>4)<<6). */

/* SC6530 GPIO register access (fpdoom gpio_set, syscode.c:133-150). */
static void led_gpio_set(unsigned id, unsigned off, int state)
{
#if !defined(HOST_TEST)
    uint32_t addr, tmp, shl = id & 0xfu;

    addr = (id >= 128)
        ? (0x82001580u) + ((id >> 4) << 6)
        : 0x8a000000u + ((id >> 4) << 7);
    addr += off;
    if (id >= 128) tmp = led_adi_read(addr);
    else           tmp = MEM4(addr);
    tmp &= ~(1u << shl);
    tmp |= (uint32_t)state << shl;
    if (id >= 128) led_adi_write(addr, tmp);
    else           MEM4(addr) = tmp;
#else
    (void)id; (void)off; (void)state;
#endif
}

/* BANNED GPIO ids — driving these HANGS the B310E (HW-verified):
 *   125  (0x8a000380 bit 13) — the torch sweep logged "gpio 125" on
 *        2026-08-21 and the phone died silently (not a fault: no
 *        exception line; the pin drives a power/memory-critical line).
 *        Never drive it again; skip it in any GPIO sweep. */
#define LED_TORCH_BANNED_PIN 125u

/* Advance to the next GPIO and drive it HIGH as output; previous pin is
 * released (input, no drive). Returns the current pin id (0-159, banned
 * pins skipped). Call every ~1.5s from a task; the user watches which pin
 * lights the torch. MARKED EMPIRICAL: report the pin id that lights the
 * LED, we hardcode it. */
int led_torch_probe_next(void)
{
#if !defined(HOST_TEST)
    static int s_id = -1;
    unsigned id;

    if (s_id >= 0)
        led_gpio_set((unsigned)s_id, 8, 0);     /* previous: DIR = input */

    do {
        s_id++;
        if (s_id >= 160)
            s_id = 0;
    } while ((unsigned)s_id == LED_TORCH_BANNED_PIN);

    id = (unsigned)s_id;
    led_gpio_set(id, 4, 1);             /* DMSK */
    led_gpio_set(id, 8, 1);             /* DIR = output */
    led_gpio_set(id, 0x18, 0);          /* IE off */
    led_gpio_set(id, 0, 1);             /* DATA = high (torch on) */
    return s_id;
#else
    return 0;
#endif
}

/* ---- kernel module ------------------------------------------------------- */

int led_init(void)
{
    /* Nothing to power on at module time: the ADI path is always alive
     * (chip init enabled the analog block). lcd_init() calls
     * led_backlight_set() to light the panel after the LCDC comes up. */
    return 0;
}

const module_t led_module = { "led", led_init };
