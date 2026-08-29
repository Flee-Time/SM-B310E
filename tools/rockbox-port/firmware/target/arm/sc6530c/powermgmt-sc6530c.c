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
#include "system.h"
#include "adc.h"
#include "power.h"
#include "powermgmt.h"

/*
 * B310E-OS Rockbox port — b310e/target/arm/sc6530c/powermgmt-sc6530c.c
 * (GPLv2, Rockbox-derived; modeled on Rockbox's mini2440/powermgmt-mini2440.c
 * and the B310E-OS drivers/battery.c — the SC6530 internal-ADC fuel gauge,
 * adc_phy_v5.c disassembly, VERBATIM).
 *
 * VBAT is read by the SC6530 internal ADC on channel 5 (HW-verified:
 * ch5 is the only channel that moves with the battery). The conversion is
 * stock-verbatim: START -> SELECT -> MODE 0 (both bits 4 AND 5!) ->
 * ENABLE -> poll done (bounded) -> result; a done-bit timeout runs the
 * ANA recovery block then retries ONCE (the first conversion after boot
 * always times out). All waits are iteration-budgeted (the free-running
 * 0x8100300c counter glitches on this phone — see the B310E-OS learnings).
 *
 * Rockbox's powermgmt.c wraps this: it implements the digital filter and
 * the voltage-to-percent mapping; we only provide _battery_voltage() and
 * the percent_to_volt tables.
 */

#define REG32(a) (*(volatile uint32_t *)(a))

/* Bounded ADI mailbox (fpdoom/B310E-OS drivers/battery.c pattern). */
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

/* ---- ADC registers (adc_phy_v5.c; base 0x82001680) --------------------- */
#define BAT_ADC_BASE      0x82001680u
#define BAT_ADC_ENABLE    (BAT_ADC_BASE + 0x00u)
#define BAT_ADC_CHSEL     (BAT_ADC_BASE + 0x04u)
#define BAT_ADC_START     (BAT_ADC_BASE + 0x54u)
#define BAT_ADC_STATUS    (BAT_ADC_BASE + 0x5cu)
#define BAT_ADC_RESULT    (BAT_ADC_BASE + 0x4cu)

#define BAT_ANA_BASE      0x82001040u
#define BAT_ANA_E0        0x820010E0u
#define BAT_ANA_E4        0x820010E4u
#define BAT_ANA_A0        0x820010A0u
#define BAT_ANA_B0        0x820010B0u

#define BAT_CH_VBAT       5u
#define BAT_CONV_BUDGET   1000000u
#define BAT_CONV_TIMEOUT  0xFFFFFFFFu

/* 4.39 mV/count (HW-calibrated 2026-08-22: 4.15V at raw 946; full-scale
 * 1023 -> 4491 mV — the stock 3.54 mV/count only fits the ch6 rail). */
#define BAT_CONV_NUM      4390u
#define BAT_CONV_DEN      1000u

static void bat_adc_select(uint32_t ch)
{
    uint32_t v = adi_read(BAT_ADC_CHSEL);

    adi_write(BAT_ADC_CHSEL, v & ~0xFu);
    adi_write(BAT_ADC_CHSEL, adi_read(BAT_ADC_CHSEL) | (ch & 0xFu));
}

static void bat_adc_mode0(void)
{
    adi_write(BAT_ADC_CHSEL, adi_read(BAT_ADC_CHSEL) | 0x10u);
    adi_write(BAT_ADC_CHSEL, adi_read(BAT_ADC_CHSEL) | 0x20u);
}

static void bat_adc_mux(void)
{
    adi_write(BAT_ADC_BASE + 0x2cu,
              (adi_read(BAT_ADC_BASE + 0x2cu) & ~0xFu) | 2u);
    adi_write(BAT_ADC_BASE + 0x34u,
              (adi_read(BAT_ADC_BASE + 0x34u) & ~0xFu) | 3u);
}

/* Re-assert the keypad EIC channel 3 (END key) after the recovery clobbers
 * 0x820010E0/E4 — same sequence as button-sc6530c.c button_init_device. */
static void eic_reapply(void)
{
    adi_write(0x820010e4, 0x20);
    adi_write(0x820010e0, 0x80);
    adi_write(0x82001904, adi_read(0x82001904) | (1u << 3));
}

static void bat_adc_recovery(void)
{
    adi_write(BAT_ANA_BASE, 1u);
    adi_write(BAT_ANA_BASE, 2u);
    adi_write(BAT_ANA_E0, 0x20u);
    adi_write(BAT_ANA_E0, 0x10u);
    adi_write(BAT_ANA_E4, 0x10u);
    adi_write(BAT_ANA_A0, 8u);
    {
        volatile unsigned int d = 10;

        while (d--) ;
    }
    adi_write(BAT_ANA_B0, 8u);

    adi_write(BAT_ADC_ENABLE, adi_read(BAT_ADC_ENABLE) | 1u);
    adi_write(BAT_ADC_BASE + 0x48u, 0xE0u);
    adi_write(BAT_ADC_BASE + 0x34u, adi_read(BAT_ADC_BASE + 0x34u) | 0x40u);
    adi_write(BAT_ADC_BASE + 0x2cu, adi_read(BAT_ADC_BASE + 0x2cu) | 0x40u);
    adi_write(BAT_ADC_BASE + 0x30u, adi_read(BAT_ADC_BASE + 0x30u) | 0x40u);
    adi_write(BAT_ADC_BASE + 0x28u, adi_read(BAT_ADC_BASE + 0x28u) | 0x40u);

    bat_adc_mux();

    eic_reapply();
}

static int bat_adc_wait_done(void)
{
    uint32_t n = BAT_CONV_BUDGET;

    while (!(adi_read(BAT_ADC_STATUS) & 1u))
        if (--n == 0)
            return 0;
    return 1;
}

/* One conversion attempt (stock adc_read @0x20ef6 verbatim order). */
static uint32_t bat_adc_convert(void)
{
    uint32_t raw;

    adi_write(BAT_ADC_START, adi_read(BAT_ADC_START) | 1u);
    bat_adc_select(BAT_CH_VBAT);
    bat_adc_mode0();
    adi_write(BAT_ADC_ENABLE, adi_read(BAT_ADC_ENABLE) | 2u);

    if (!bat_adc_wait_done()) {
        adi_write(BAT_ADC_START, adi_read(BAT_ADC_START) | 0x3ffu);
        return BAT_CONV_TIMEOUT;
    }

    raw = adi_read(BAT_ADC_RESULT) & 0x3ffu;
    adi_write(BAT_ADC_START, adi_read(BAT_ADC_START) | 0x3ffu);
    return raw;
}

/* Returns battery voltage in millivolts (0 if wedged). */
static uint32_t battery_read_mv(void)
{
    uint32_t raw = bat_adc_convert();

    if (raw == BAT_CONV_TIMEOUT) {
        /* Stock @0x20f34: timeout -> recovery -> retry once. */
        bat_adc_recovery();
        raw = bat_adc_convert();
        if (raw == BAT_CONV_TIMEOUT)
            return 0;
    }
    return (raw * BAT_CONV_NUM + BAT_CONV_DEN / 2u) / BAT_CONV_DEN;
}

/* The following constants are for the 800 mAh SM-B310E Li-ion cell
 * (AB463446BU). */
unsigned short battery_level_disksafe = 3450;

unsigned short battery_level_shutoff = 3400;

/* voltages (millivolt) of 0%, 10%, ... 100% when charging disabled */
unsigned short percent_to_volt_discharge[11] =
{
    /* Typical Li Ion 800mAH */
    3400, 3520, 3570, 3600, 3630, 3660, 3720, 3780, 3860, 3940, 4100
};

/* voltages (millivolt) of 0%, 10%, ... 100% when charging enabled */
unsigned short percent_to_volt_charge[11] =
{
    /* Typical Li Ion 800mAH */
    3400, 3520, 3570, 3600, 3630, 3660, 3720, 3780, 3860, 3940, 4100
};

/* Returns battery voltage from the SC6530 ADC [millivolts] */
int _battery_voltage(void)
{
    return (int)battery_read_mv();
}
