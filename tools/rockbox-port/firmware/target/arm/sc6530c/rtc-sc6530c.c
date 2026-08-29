/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
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
#include "rtc.h"

/*
 * B310E-OS Rockbox port — b310e/target/arm/sc6530c/rtc-sc6530c.c
 * (GPLv2, Rockbox-derived; a port of the B310E-OS drivers/rtc.c — the
 * SC6530 on-die RTC, register map decoded from the stock rtc_phy_v5.c,
 * clean-room).
 *
 * Registers (ADI mailbox):
 *   read:  sec/min/hour @ 0x82001600/04/08 (mask 0x3f/0x3f/0x1f)
 *          date 16-bit @ 0x8200160C: day bits 0-4, month 5-8, year 9-15
 *          (0-127 = 2000-2127)
 *   write: clear ctrl bit 1 @ 0x82001630, write the four shadow regs
 *          @ 0x82001610/14/18/1C, set ctrl bit 1 to latch
 *          (year 0-127 = 2000-2127), then wait for the update flag
 *          @ 0x82001634 bit 0 to clear (bounded).
 */

#define REG32(a) (*(volatile uint32_t *)(a))

#define RTC_SEC       0x82001600u
#define RTC_MIN       0x82001604u
#define RTC_HOUR      0x82001608u
#define RTC_DATE      0x8200160Cu
#define RTC_W_SEC     0x82001610u
#define RTC_W_MIN     0x82001614u
#define RTC_W_HOUR    0x82001618u
#define RTC_W_DATE    0x8200161Cu
#define RTC_CTRL      0x82001630u
#define RTC_UPDATING  0x82001634u

/* Bounded ADI mailbox (fpdoom/B310E-OS pattern). */
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

/* Wait for the RTC to finish updating (bit 0 of 0x82001634), bounded. */
static int rtc_wait_idle(void)
{
    uint32_t n = ADI_BUDGET;

    while (adi_read(RTC_UPDATING) & 1u)
        if (--n == 0)
            return -1;
    return 0;
}

void rtc_init(void)
{
    /* Nothing to do — the on-die RTC is always running (backed by the
     * 32 kHz crystal). */
}

int rtc_read_datetime(struct tm *tm)
{
    uint32_t sec, min, hour, date;

    if (tm == NULL || rtc_wait_idle() != 0)
        return -1;

    sec  = adi_read(RTC_SEC)  & 0x3fu;
    min  = adi_read(RTC_MIN)  & 0x3fu;
    hour = adi_read(RTC_HOUR) & 0x1fu;
    date = adi_read(RTC_DATE) & 0xffffu;

    tm->tm_sec   = (int)sec;
    tm->tm_min   = (int)min;
    tm->tm_hour  = (int)hour;
    tm->tm_mday  = (int)(date & 0x1fu);
    tm->tm_mon   = (int)((date >> 5) & 0x1fu) - 1;   /* 0-11 */
    tm->tm_year  = (int)((date >> 9) & 0x7fu) + 100; /* 2000+ -> 1900-based */
    return 0;
}

int rtc_write_datetime(const struct tm *tm)
{
    uint32_t date;

    if (tm == NULL || rtc_wait_idle() != 0)
        return -1;

    /* Clear the latch bit, write the shadows, latch (stock set path). */
    adi_write(RTC_CTRL, adi_read(RTC_CTRL) & ~2u);

    date = ((uint32_t)(tm->tm_year - 100) & 0x7fu) << 9 |
           ((uint32_t)(tm->tm_mon + 1) & 0x1fu) << 5 |
           (uint32_t)tm->tm_mday & 0x1fu;

    adi_write(RTC_W_SEC,  (uint32_t)tm->tm_sec);
    adi_write(RTC_W_MIN,  (uint32_t)tm->tm_min);
    adi_write(RTC_W_HOUR, (uint32_t)tm->tm_hour);
    adi_write(RTC_W_DATE, date);

    adi_write(RTC_CTRL, adi_read(RTC_CTRL) | 2u);

    return rtc_wait_idle();
}
