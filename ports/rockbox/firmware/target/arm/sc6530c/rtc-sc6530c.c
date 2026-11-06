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
#include "kernel.h"
#include "rtc.h"

/*
 * The SC6530 RTC in the analog die, from the stock firmware's rtc_phy_v5
 * driver (NOR 0x35dac..0x363d0) and time manager (0x19594..0x19b9e), as
 * the B310E OS's drivers/rtc.c decodes them (2026-11-07):
 *
 *   +0x00..0x0c  counters: seconds (6 bits), minutes (6), hours (5) and a
 *                16-bit day counter, day 0 = 1980-01-01 (a Tuesday; base
 *                date at NOR 0xc48b8), kept below 2099-12-31 by stock
 *   +0x10..0x1c  new seconds, minutes, hours, days (plain binary)
 *   +0x30 enable, +0x34 raw status, +0x38 clear (write ones). Raw bits
 *                0..3: a second/minute/hour/day passed (bit 0 is the
 *                seconds interrupt, not a busy flag); bits 8..11: the RTC
 *                took the new seconds/minutes/hours/days.
 *
 * Set (stock RTC_SetTime 0x35f9a): clear old acknowledgements, clear the
 * minute-interrupt enable (+0x30 bit 1), write the four new values, wait
 * for raw bits 8..11, clear them, restore the enable.
 *
 * Until 2026-11-07 this driver read the date as packed day/month/year
 * fields from 2000, took +0x30 bit 1 for a latch and +0x34 bit 0 for a
 * busy flag: Rockbox showed and set wrong dates.
 */

#define REG32(a) (*(volatile uint32_t *)(a))

#define RTC_BASE      0x82001600u
#define RTC_SEC       (RTC_BASE + 0x00)
#define RTC_MIN       (RTC_BASE + 0x04)
#define RTC_HOUR      (RTC_BASE + 0x08)
#define RTC_DAY       (RTC_BASE + 0x0c)
#define RTC_NEW_SEC   (RTC_BASE + 0x10)
#define RTC_NEW_MIN   (RTC_BASE + 0x14)
#define RTC_NEW_HOUR  (RTC_BASE + 0x18)
#define RTC_NEW_DAY   (RTC_BASE + 0x1c)
#define RTC_IE        (RTC_BASE + 0x30)
#define RTC_RAW       (RTC_BASE + 0x34)
#define RTC_CLEAR     (RTC_BASE + 0x38)
#define IE_MINUTE     (1u << 1)
#define ACK_TIME      (0xfu << 8)

#define YEAR_MIN      1980
#define YEAR_MAX      2099
#define DAY_MAX       43829u          /* 2099-12-31 */
#define ACK_WAIT_TICKS (HZ * 2)       /* the stock firmware's 100 x 20 ms */

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

/* ---- calendar ----------------------------------------------------------- */

static bool leap(int year)
{
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

static int month_days(int year, int month)      /* month 0..11 */
{
    static const unsigned char days[12] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
    };
    return days[month] + (month == 1 && leap(year));
}

static long days_from_date(int year, int month, int day)
{
    long days = 0;

    for (int y = YEAR_MIN; y < year; y++)
        days += 365 + leap(y);
    for (int m = 0; m < month; m++)
        days += month_days(year, m);
    return days + day - 1;
}

static void date_from_days(unsigned long days, struct tm *tm)
{
    int year = YEAR_MIN, month = 0;

    tm->tm_wday = (int)((days + 2) % 7);        /* day 0 was a Tuesday */
    while (days >= (unsigned long)(365 + leap(year))) {
        days -= 365 + leap(year);
        year++;
    }
    tm->tm_yday = (int)days;
    while (days >= (unsigned long)month_days(year, month)) {
        days -= month_days(year, month);
        month++;
    }
    tm->tm_year = year - 1900;
    tm->tm_mon = month;
    tm->tm_mday = (int)days + 1;
}

/* ---- the driver --------------------------------------------------------- */

void rtc_init(void)
{
    /* Always running from the 32 kHz crystal. */
}

int rtc_read_datetime(struct tm *tm)
{
    uint32_t sec = 0, min = 0, hour = 0, day = 0;

    if (tm == NULL)
        return -1;
    /* Read the seconds again last; a carry in between reads the set again.
     * Atomic against the button tick's ADI use. */
    for (int tries = 0; tries < 3; tries++) {
        int old = disable_irq_save();
        uint32_t first = adi_read(RTC_SEC) & 0x3fu;

        min = adi_read(RTC_MIN) & 0x3fu;
        hour = adi_read(RTC_HOUR) & 0x1fu;
        day = adi_read(RTC_DAY) & 0xffffu;
        sec = adi_read(RTC_SEC) & 0x3fu;
        restore_irq(old);
        if (sec == first)
            break;
    }
    tm->tm_sec = sec < 60 ? (int)sec : 59;
    tm->tm_min = min < 60 ? (int)min : 59;
    tm->tm_hour = hour < 24 ? (int)hour : 23;
    tm->tm_isdst = 0;
    if (day > DAY_MAX) {
        date_from_days(0, tm);
        return -1;
    }
    date_from_days(day, tm);
    return 0;
}

int rtc_write_datetime(const struct tm *tm)
{
    int year, old;
    long days;
    uint32_t ie, raw = 0;
    long end;

    if (tm == NULL)
        return -1;
    year = tm->tm_year + 1900;
    if (year < YEAR_MIN || year > YEAR_MAX || tm->tm_mon < 0 || tm->tm_mon > 11 ||
        tm->tm_mday < 1 || tm->tm_mday > month_days(year, tm->tm_mon) ||
        tm->tm_hour < 0 || tm->tm_hour > 23 || tm->tm_min < 0 || tm->tm_min > 59 ||
        tm->tm_sec < 0 || tm->tm_sec > 59)
        return -1;
    days = days_from_date(year, tm->tm_mon, tm->tm_mday);

    old = disable_irq_save();
    adi_write(RTC_CLEAR, ACK_TIME);     /* the last set's acknowledgements */
    ie = adi_read(RTC_IE);
    if (ie & IE_MINUTE)
        adi_write(RTC_IE, ie & ~IE_MINUTE);
    adi_write(RTC_NEW_SEC, (uint32_t)tm->tm_sec);
    adi_write(RTC_NEW_MIN, (uint32_t)tm->tm_min);
    adi_write(RTC_NEW_HOUR, (uint32_t)tm->tm_hour);
    adi_write(RTC_NEW_DAY, (uint32_t)days);
    restore_irq(old);

    end = current_tick + ACK_WAIT_TICKS;
    for (;;) {
        old = disable_irq_save();
        raw = adi_read(RTC_RAW);
        restore_irq(old);
        if ((raw & ACK_TIME) == ACK_TIME || TIME_AFTER(current_tick, end))
            break;
        sleep(1);
    }

    old = disable_irq_save();
    adi_write(RTC_CLEAR, ACK_TIME);
    if (ie & IE_MINUTE)
        adi_write(RTC_IE, ie);
    restore_irq(old);

    if ((raw & ACK_TIME) != ACK_TIME) {
        /* No acknowledgement: good only if the counters show the new day */
        struct tm now;

        if (rtc_read_datetime(&now) != 0 || now.tm_year != tm->tm_year ||
            now.tm_mon != tm->tm_mon || now.tm_mday != tm->tm_mday)
            return -1;
    }
    return 0;
}
