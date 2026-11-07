/* B310E OS — the SC6530 RTC as a calendar (drivers/rtc.c).
 *
 * The RTC counts seconds, minutes, hours and days. The day counter is 16
 * bits wide and counts days since 1980-01-01 (day 0, a Tuesday), the
 * stock firmware's epoch; its calendar stops at 2099-12-31 (day 43829),
 * so does ours, and a date set here reads the same in the stock firmware.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef OS_RTC_H
#define OS_RTC_H

#include <stdbool.h>
#include <stdint.h>

#define RTC_YEAR_MIN   1980         /* day 0 */
#define RTC_YEAR_MAX   2099
#define RTC_DAY_MAX    43829u       /* 2099-12-31 */

/* Error codes of rtc_get / rtc_set */
#define RTC_EINVAL     (-1)         /* a field out of range */
#define RTC_EBADDATE   (-2)         /* the day counter is past 2099-12-31 */
#define RTC_EBUSY      (-3)         /* another rtc_set is running */
#define RTC_ETIMEOUT   (-4)         /* no update acknowledgement, read back differs */
#define RTC_EIO        (-5)         /* an ADI write failed */

typedef struct {
    uint16_t year;                  /* 1980 .. 2099 */
    uint8_t month;                  /* 1 .. 12 */
    uint8_t day;                    /* 1 .. 31 */
    uint8_t hour;                   /* 0 .. 23 */
    uint8_t min;                    /* 0 .. 59 */
    uint8_t sec;                    /* 0 .. 59 */
    uint8_t wday;                   /* 0 Sunday .. 6 Saturday; rtc_set ignores it */
} rtc_datetime_t;

/* Read the clock. Returns 0; RTC_EBADDATE when the day counter is past
 * 2099-12-31 (the time of day is filled in, the date is 1980-01-01). */
int rtc_get(rtc_datetime_t *t);

/* Set the clock (all fields but wday). Waits until the RTC acknowledges
 * all four counters, at most 2 s; 0 or a negative RTC_E* code. Not from
 * interrupt handlers. */
int rtc_set(const rtc_datetime_t *t);

/* calendar helpers (pure) */
bool rtc_leap_year(int year);
int rtc_days_in_month(int year, int month);         /* 0 for a bad month */
bool rtc_valid(const rtc_datetime_t *t);            /* every field in range */
/* Days since 1980-01-01 (the RTC day counter); -1 for a bad date. */
int32_t rtc_days_from_date(int year, int month, int day);
/* The date of a day count (year, month, day, wday); false past RTC_DAY_MAX. */
bool rtc_date_from_days(uint32_t days, rtc_datetime_t *t);
int rtc_weekday(int year, int month, int day);      /* 0 Sunday .. 6; -1 bad */
const char *rtc_weekday_name(int wday);             /* "Sun" .. "Sat" */

/* The time packed for FatFs' get_fattime(): year - 1980 in bits 31:25,
 * month 24:21, day 20:16, hour 15:11, minute 10:5, seconds / 2 in 4:0.
 * 2026-01-01 00:00 when the RTC cannot be read as a date. */
uint32_t rtc_fattime(void);

/* The clock kept on the card (/b310e-os/clock.txt): saved at power-off,
 * before a restart and every 10 minutes; at boot a clock behind the saved
 * one was cleared (a reset, the stock firmware's 2015-01-01) and gets the
 * saved time back. The time it was off is lost, not the date. */
void rtc_save(void);
/* Set the clock from Unix time (UTC) and the zone in minutes; 0 if set. */
int rtc_set_unix(uint32_t unix_s, int tz_min);
uint32_t rtc_unix(int tz_min);              /* the clock as Unix time (UTC) */
void rtc_restore(void);

#endif
