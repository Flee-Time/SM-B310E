/* B310E boot loader — battery level and charger, for the status bar.
 *
 * B310E OS drivers/battery.c (verified on the phone): the battery on ADC
 * channel 5 in the analog die (0x82001680), 4.39 mV a count; the first
 * conversion after boot times out and the stock recovery block runs once.
 * The charger is analog EIC channel 2 (unmasked in 0x82001904); its status
 * lines PGB (GPIO 8) and CHGSB (GPIO 9) tell charging from done. On the
 * charger channel 5 reads the charging rail, so the bar shows "chg".
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "boot.h"

#define ADC         0x82001680u
#define ADC_ENABLE  (ADC + 0x00)
#define ADC_CHSEL   (ADC + 0x04)
#define ADC_RESULT  (ADC + 0x4c)
#define ADC_START   (ADC + 0x54)
#define ADC_STATUS  (ADC + 0x5c)
#define EIC_DATA    0x82001900u
#define EIC_DMSK    0x82001904u
#define EIC_CHARGER (1u << 2)
#define TIMEOUT     0xffffffffu

#define GPIO_DATA     0x8a000000u
#define GPIO_DMSK     0x8a000004u
#define GPIO_DIR      0x8a000008u
#define CHARGER_PGB   (1u << 8)
#define CHARGER_CHGSB (1u << 9)

static void set_bits(uint32_t reg, uint32_t bits)
{
    adi_write(reg, adi_read(reg) | bits);
}

static void recovery(void)
{
    adi_write(0x82001040, 1);
    adi_write(0x82001040, 2);
    adi_write(0x820010e0, 0x20);
    adi_write(0x820010e0, 0x10);
    adi_write(0x820010e4, 0x10);
    adi_write(0x820010a0, 8);
    delay(100);
    adi_write(0x820010b0, 8);
    set_bits(ADC_ENABLE, 1);
    adi_write(ADC + 0x48, 0xe0);
    set_bits(ADC + 0x34, 0x40);
    set_bits(ADC + 0x2c, 0x40);
    set_bits(ADC + 0x30, 0x40);
    set_bits(ADC + 0x28, 0x40);
    adi_write(ADC + 0x2c, (adi_read(ADC + 0x2c) & ~0xfu) | 2);
    adi_write(ADC + 0x34, (adi_read(ADC + 0x34) & ~0xfu) | 3);
    adi_write(0x820010e4, 0x20);         /* the END key's EIC channel again */
    adi_write(0x820010e0, 0x80);
    set_bits(EIC_DMSK, 1u << 3);
}

static uint32_t convert(unsigned ch)
{
    uint32_t n = 100000, raw;

    set_bits(ADC_START, 1);
    adi_write(ADC_CHSEL, adi_read(ADC_CHSEL) & ~0xfu);
    set_bits(ADC_CHSEL, ch & 0xf);
    set_bits(ADC_CHSEL, 0x10);
    set_bits(ADC_CHSEL, 0x20);
    set_bits(ADC_ENABLE, 2);
    while (!(adi_read(ADC_STATUS) & 1) && --n)
        ;
    raw = n ? adi_read(ADC_RESULT) & 0x3ff : TIMEOUT;
    set_bits(ADC_START, 0x3ff);
    return raw;
}

static bool charger_present(void)
{
    static bool masked;

    if (!masked) {
        set_bits(EIC_DMSK, EIC_CHARGER);
        masked = true;
    }
    return adi_read(EIC_DATA) & EIC_CHARGER;
}

int charge_state(void)
{
    static bool configured;
    uint32_t d;

    if (!charger_present())
        return CHG_NONE;
    if (!configured) {
        REG32(0x8b0000a0) = 0x800080u;   /* GPIO clocks */
        REG32(GPIO_DIR) &= ~(CHARGER_PGB | CHARGER_CHGSB);
        REG32(GPIO_DMSK) |= CHARGER_PGB | CHARGER_CHGSB;
        configured = true;
    }
    d = REG32(GPIO_DATA);
    if (d & CHARGER_PGB)
        return CHG_NONE;                 /* no good input */
    return d & CHARGER_CHGSB ? CHG_DONE : CHG_CHARGING;
}

int battery_mv(void)
{
    static int last = -1;

    if (!charger_present() || last < 0) {
        uint32_t raw = convert(5);

        if (raw == TIMEOUT) {
            recovery();
            raw = convert(5);
        }
        if (raw != TIMEOUT)
            last = (int)((raw * 4390u + 500u) / 1000u);
    }
    return last;
}

/* 800 mAh Li-ion (AB463446BU): 0, 10 .. 100 % (B310E OS, the Rockbox port) */
int battery_percent(int mv)
{
    static const uint16_t curve[11] = {
        3400, 3520, 3570, 3600, 3630, 3660, 3720, 3780, 3860, 3940, 4100
    };

    if (mv < 0)
        return -1;
    if (mv <= curve[0])
        return 0;
    for (int i = 1; i < 11; i++)
        if (mv < curve[i])
            return (i - 1) * 10 + (mv - curve[i - 1]) * 10 / (curve[i] - curve[i - 1]);
    return 100;
}
