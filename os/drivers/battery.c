/* B310E OS — battery voltage (ADC channel 5), charger, time of day.
 *
 * The SC6530 ADC sits in the analog die (0x82001680); the conversion
 * order is the stock adc_read (0x20ef6) as the Rockbox port uses it,
 * verified on the phone: start, select, mode 0, enable, poll done, read.
 * A timed-out conversion runs the stock recovery block once and retries
 * (the first one after boot always times out). 4.39 mV per count
 * (calibrated on the phone: 4.15 V at 946).
 *
 * With a charger connected channel 5 reads the charging rail, not the
 * cell, so the last unplugged reading is kept.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/arch.h"
#include "os/drivers.h"

#define ADC         0x82001680u
#define ADC_ENABLE  (ADC + 0x00)
#define ADC_CHSEL   (ADC + 0x04)
#define ADC_RESULT  (ADC + 0x4c)
#define ADC_START   (ADC + 0x54)
#define ADC_STATUS  (ADC + 0x5c)
#define EIC_DATA    0x82001900u
#define EIC_CHARGER (1u << 2)
#define TIMEOUT     0xffffffffu

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
    udelay(1);
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
    set_bits(0x82001904, 1u << 3);
}

static uint32_t convert(unsigned ch)
{
    uint32_t n = 100000, raw;

    set_bits(ADC_START, 1);
    adi_write(ADC_CHSEL, adi_read(ADC_CHSEL) & ~0xfu);
    set_bits(ADC_CHSEL, ch & 0xf);
    set_bits(ADC_CHSEL, 0x10);          /* mode 0: both bits */
    set_bits(ADC_CHSEL, 0x20);
    set_bits(ADC_ENABLE, 2);
    while (!(adi_read(ADC_STATUS) & 1) && --n)
        ;
    raw = n ? adi_read(ADC_RESULT) & 0x3ff : TIMEOUT;
    set_bits(ADC_START, 0x3ff);
    return raw;
}

uint32_t adc_read(unsigned ch)
{
    uint32_t s = arch_irq_save(), raw = convert(ch);

    if (raw == TIMEOUT) {
        recovery();
        raw = convert(ch);
    }
    arch_irq_restore(s);
    return raw;
}

/* The charger's level (analog EIC channel 2) reads 0 until its mask bit
 * is set in 0x82001904, as the Rockbox port's power_init does. */
bool charger_present(void)
{
    static bool masked;

    if (!masked) {
        uint32_t s = arch_irq_save();

        adi_write(0x82001904, adi_read(0x82001904) | EIC_CHARGER);
        arch_irq_restore(s);
        masked = true;
    }
    return adi_read(EIC_DATA) & EIC_CHARGER;
}

/* The charger's status lines (stock GetCHGDoneStatus, NOR 0x66352; the
 * Rockbox port's charging_state): PGB is GPIO8, CHGSB GPIO9, inputs. PGB
 * high: no good input. PGB low, CHGSB low: charging; CHGSB high: done. */
#define GPIO_DATA     0x8a000000u
#define GPIO_DMSK     0x8a000004u
#define GPIO_DIR      0x8a000008u
#define CHARGER_PGB   (1u << 8)
#define CHARGER_CHGSB (1u << 9)

int charge_state(void)
{
    static bool configured;
    uint32_t d;

    if (!charger_present())
        return CHARGE_NONE;
    if (!configured) {
        uint32_t s = arch_irq_save();

        gpio_clock_on();
        REG32(GPIO_DIR) &= ~(CHARGER_PGB | CHARGER_CHGSB);
        REG32(GPIO_DMSK) |= CHARGER_PGB | CHARGER_CHGSB;
        arch_irq_restore(s);
        configured = true;
    }
    d = REG32(GPIO_DATA);
    if (d & CHARGER_PGB)
        return CHARGE_NONE;             /* no good input */
    return d & CHARGER_CHGSB ? CHARGE_DONE : CHARGE_CHARGING;
}

int battery_mv(void)
{
    static int last = -1;

    if (!charger_present()) {
        uint32_t raw = adc_read(5);

        if (raw != TIMEOUT)
            last = (int)((raw * 4390u + 500u) / 1000u);
    }
    return last;
}

/* 800 mAh Li-ion (AB463446BU): 0, 10 .. 100 % (the Rockbox port's table) */
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

/* RTC seconds, minutes, hours at 0x82001600/04/08 (the date register is a
 * day counter, not decoded yet). */
void rtc_time(int *h, int *m, int *s)
{
    *s = (int)(adi_read(0x82001600) & 0x3f);
    *m = (int)(adi_read(0x82001604) & 0x3f);
    *h = (int)(adi_read(0x82001608) & 0x1f);
}
