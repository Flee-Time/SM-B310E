/* B310E OS — the SC6530 RTC: date and time, read and set.
 *
 * The RTC sits in the analog die (ADI). Its registers, from the stock
 * firmware's rtc_phy_v5.c driver (NOR 0x35dac .. 0x363d0) and its time
 * manager (NOR 0x19594 .. 0x19b9e):
 *
 *   +0x00 .. +0x0c  counters: seconds (6 bits), minutes (6), hours (5),
 *                   days (16 bits, all of the ADI word); read directly
 *                   (stock RTC_GetTime 0x36070)
 *   +0x10 .. +0x1c  new seconds, minutes, hours, days
 *   +0x20 .. +0x2c  alarm seconds .. days
 *   +0x30           interrupt enable; +0x34 raw status; +0x38 clear
 *                   (write ones). Raw bits 0..3: second, minute, hour and
 *                   day passed, bit 4 the alarm, bits 8..11 the RTC took
 *                   the new seconds, minutes, hours, days, bits 12..15 the
 *                   alarm fields. Stock init (0x35e4a) clears 0x38 = 0xff1f
 *                   and enables 0xff02 (the minute and every acknowledge).
 *
 * The day counter counts days since 1980-01-01: the time manager turns it
 * into a date from the base date 1980-01-01, a Tuesday (NOR 0xc48b8:
 * 01 01 bc07 02), with the month tables at 0xc48c4 (0x19594), back again
 * by counting the years from 1980 (literal 0x7bc at 0x199d8, 0x19762),
 * and keeps it below the last date at 0xc48be: 2099-12-31 (0x19a84).
 * The set path (0x35f9a) takes a counter value up to 0xfffe.
 *
 * Setting (stock RTC_SetTime 0x35f9a): wait until the previous update
 * has been acknowledged; clear the minute interrupt enable (+0x30 bit 1);
 * write the new seconds, minutes, hours and days to +0x10 .. +0x1c, plain
 * binary, nothing to latch; enable the minute interrupt again. The RTC
 * raises raw bits 8..11 as it takes each field; the stock interrupt
 * handler (0x35dac) clears them through +0x38 and polls raw until they
 * drop, and until then RTC_GetTime returns the values just written.
 * We poll raw bits 8..11 instead of taking the interrupt, clear them, and
 * readers get the values being set (plus the time since) meanwhile.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/arch.h"
#include "os/fs.h"
#include "os/kernel.h"
#include "os/lib.h"
#include "os/rtc.h"
#include "build_info.h"

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
#define ACK_TIME      (0xfu << 8)   /* seconds, minutes, hours, days taken */

#define ACK_WAIT_MS   2000          /* the stock firmware's: 100 x 20 ms */
#define SLACK_S       3             /* read-back drift accepted without ack */

static const uint8_t month_days[12] = {
    31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
};

/* ---- calendar --------------------------------------------------------------- */

bool rtc_leap_year(int year)
{
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

int rtc_days_in_month(int year, int month)
{
    if (month < 1 || month > 12)
        return 0;
    return month_days[month - 1] + (month == 2 && rtc_leap_year(year));
}

bool rtc_valid(const rtc_datetime_t *t)
{
    return t && t->year >= RTC_YEAR_MIN && t->year <= RTC_YEAR_MAX &&
           t->day >= 1 && t->day <= rtc_days_in_month(t->year, t->month) &&
           t->hour < 24 && t->min < 60 && t->sec < 60;
}

static int32_t leaps_before(int year)       /* leap years in [1, year) */
{
    int y = year - 1;

    return y / 4 - y / 100 + y / 400;
}

int32_t rtc_days_from_date(int year, int month, int day)
{
    int32_t days;

    if (year < RTC_YEAR_MIN || year > RTC_YEAR_MAX || day < 1 ||
        day > rtc_days_in_month(year, month))
        return -1;
    days = 365 * (int32_t)(year - RTC_YEAR_MIN) +
           leaps_before(year) - leaps_before(RTC_YEAR_MIN);
    for (int m = 1; m < month; m++)
        days += rtc_days_in_month(year, m);
    return days + day - 1;
}

bool rtc_date_from_days(uint32_t days, rtc_datetime_t *t)
{
    int year = RTC_YEAR_MIN, month = 1;

    if (days > RTC_DAY_MAX)
        return false;
    t->wday = (uint8_t)((days + 2) % 7);    /* day 0 was a Tuesday */
    while (days >= (uint32_t)(365 + rtc_leap_year(year))) {
        days -= (uint32_t)(365 + rtc_leap_year(year));
        year++;
    }
    while (days >= (uint32_t)rtc_days_in_month(year, month)) {
        days -= (uint32_t)rtc_days_in_month(year, month);
        month++;
    }
    t->year = (uint16_t)year;
    t->month = (uint8_t)month;
    t->day = (uint8_t)(days + 1);
    return true;
}

int rtc_weekday(int year, int month, int day)
{
    int32_t days = rtc_days_from_date(year, month, day);

    return days < 0 ? -1 : (int)((days + 2) % 7);
}

const char *rtc_weekday_name(int wday)
{
    static const char names[7][4] = {
        "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
    };

    return wday >= 0 && wday < 7 ? names[wday] : "---";
}

/* ---- the counters ----------------------------------------------------------- */

/* While rtc_set runs, readers get the time being set plus the time since. */
static volatile bool setting;
static uint32_t set_seconds;            /* days * 86400 + time of day */
static uint32_t set_at;                 /* kernel_ticks() then */

static uint32_t to_seconds(uint32_t days, unsigned h, unsigned m, unsigned s)
{
    return days * 86400u + h * 3600u + m * 60u + s;
}

/* Seconds, minutes, hours, days, read as one: the seconds are read again
 * last, and a change in between (a carry) reads the set again. */
static uint32_t read_counters(void)
{
    uint32_t sec = 0, min = 0, hour = 0, day = 0;

    for (int tries = 0; tries < 3; tries++) {
        uint32_t s = arch_irq_save(), first = adi_read(RTC_SEC) & 0x3f;

        min = adi_read(RTC_MIN) & 0x3f;
        hour = adi_read(RTC_HOUR) & 0x1f;
        day = adi_read(RTC_DAY) & 0xffff;
        sec = adi_read(RTC_SEC) & 0x3f;
        arch_irq_restore(s);
        if (sec == first)
            break;
    }
    /* a field out of range (never seen) counts as its maximum */
    return to_seconds(day, hour < 24 ? hour : 23, min < 60 ? min : 59,
                      sec < 60 ? sec : 59);
}

static uint32_t now_seconds(void)
{
    uint32_t s = arch_irq_save();

    if (setting) {
        uint32_t at = set_seconds + (kernel_ticks() - set_at) / 1000u;

        arch_irq_restore(s);
        return at;
    }
    arch_irq_restore(s);
    return read_counters();
}

int rtc_get(rtc_datetime_t *t)
{
    uint32_t total = now_seconds(), tod = total % 86400u;

    t->hour = (uint8_t)(tod / 3600u);
    t->min = (uint8_t)(tod / 60u % 60u);
    t->sec = (uint8_t)(tod % 60u);
    if (rtc_date_from_days(total / 86400u, t))
        return 0;
    rtc_date_from_days(0, t);
    return RTC_EBADDATE;
}

static void wait_1ms(void)
{
    if (kernel_running() && !arch_in_irq())
        thread_sleep(1);
    else
        udelay(1000);
}

/* Clear raw bits and wait until they read clear (the stock handler polls
 * up to 10 reads). */
static bool clear_raw(uint32_t bits)
{
    if (!adi_write(RTC_CLEAR, bits))
        return false;
    for (int n = 0; n < 100; n++)
        if (!(adi_read(RTC_RAW) & bits))
            return true;
    return false;
}

int rtc_set(const rtc_datetime_t *t)
{
    uint32_t s, ie, raw = 0, want, got, days;
    int ms, result = 0;
    bool ok;

    if (!rtc_valid(t))
        return RTC_EINVAL;
    days = (uint32_t)rtc_days_from_date(t->year, t->month, t->day);
    want = to_seconds(days, t->hour, t->min, t->sec);

    s = arch_irq_save();
    if (setting) {
        arch_irq_restore(s);
        return RTC_EBUSY;
    }
    set_seconds = want;
    set_at = kernel_ticks();
    setting = true;
    arch_irq_restore(s);

    /* Nobody takes the RTC interrupt, so the last set's acknowledgements
     * are still raised: clear them first. */
    ok = clear_raw(ACK_TIME);
    ie = adi_read(RTC_IE);
    if (ie & IE_MINUTE)                 /* as the stock set path does */
        ok = adi_write(RTC_IE, ie & ~IE_MINUTE) && ok;
    s = arch_irq_save();
    ok = adi_write(RTC_NEW_SEC, t->sec) && ok;
    ok = adi_write(RTC_NEW_MIN, t->min) && ok;
    ok = adi_write(RTC_NEW_HOUR, t->hour) && ok;
    ok = adi_write(RTC_NEW_DAY, days) && ok;
    arch_irq_restore(s);

    for (ms = 0; ms < ACK_WAIT_MS; ms++) {
        raw = adi_read(RTC_RAW);
        if ((raw & ACK_TIME) == ACK_TIME)
            break;
        wait_1ms();
    }
    clear_raw(ACK_TIME);
    if (ie & IE_MINUTE)
        adi_write(RTC_IE, ie);

    s = arch_irq_save();
    setting = false;
    arch_irq_restore(s);

    got = read_counters();
    if (!ok) {
        result = RTC_EIO;
    } else if ((raw & ACK_TIME) != ACK_TIME) {
        /* No acknowledgement: keep the set only if the counters show it. */
        uint32_t since = want + (uint32_t)ms / 1000u;

        if (got < want || got > since + SLACK_S)
            result = RTC_ETIMEOUT;
    }
    klog("rtc: set %04u-%02u-%02u %02u:%02u:%02u (day %lu): %s, ack 0x%03lx "
         "after %d ms, reads day %lu %02lu:%02lu:%02lu",
         t->year, t->month, t->day, t->hour, t->min, t->sec,
         (unsigned long)days, result == 0 ? "ok" : "failed",
         (unsigned long)(raw & ACK_TIME), ms, (unsigned long)(got / 86400u),
         (unsigned long)(got % 86400u / 3600u),
         (unsigned long)(got % 3600u / 60u), (unsigned long)(got % 60u));
    return result;
}

#define UNIX_1980  315532800u              /* 1980-01-01 00:00:00 UTC */

int rtc_set_unix(uint32_t unix_s, int tz_min)
{
    int64_t local = (int64_t)unix_s + (int64_t)tz_min * 60 - UNIX_1980;
    rtc_datetime_t t;

    if (local < 0 || !rtc_date_from_days((uint32_t)(local / 86400), &t))
        return RTC_EINVAL;
    t.hour = (uint8_t)(local % 86400 / 3600);
    t.min = (uint8_t)(local % 3600 / 60);
    t.sec = (uint8_t)(local % 60);
    return rtc_set(&t);
}

uint32_t rtc_unix(int tz_min)
{
    return (uint32_t)((int64_t)now_seconds() + UNIX_1980 - (int64_t)tz_min * 60);
}

#define CLOCK_FILE OS_DIR "/clock.txt"
#define PLAUSIBLE_DAYS (30u * 366u)     /* past this build's date: not a clock */
#define SAVED_DAYS     (3u * 366u)      /* past the saved time: off that long? no */

/* BUILD_DATE, "YYYY-MM-DD HH:MM": the latest thing known to be true. */
static uint32_t build_seconds(void)
{
    const char *b = BUILD_DATE;
#define TWO(i) ((b[i] - '0') * 10 + (b[(i) + 1] - '0'))
    int32_t days = rtc_days_from_date(TWO(0) * 100 + TWO(2), TWO(5), TWO(8));

    return days < 0 ? 0 : to_seconds((uint32_t)days, (unsigned)TWO(11), (unsigned)TWO(14), 0);
#undef TWO
}

void rtc_save(void)
{
    char buf[16];
    FIL f;
    UINT n;
    int len = snprintf(buf, sizeof(buf), "%lu\n", (unsigned long)now_seconds());

    if (f_open(&f, CLOCK_FILE, FA_WRITE | FA_CREATE_ALWAYS) == FR_OK) {
        f_write(&f, buf, (UINT)len, &n);
        f_close(&f);
    }
}

void rtc_restore(void)
{
    char buf[16];
    FIL f;
    UINT n = 0;
    uint32_t saved = 0, now = now_seconds(), to, built = build_seconds();
    uint32_t day = adi_read(RTC_DAY) & 0xffff, last_day = built / 86400u + PLAUSIBLE_DAYS;
    bool time_bad = (adi_read(RTC_HOUR) & 0x1f) > 23 || (adi_read(RTC_MIN) & 0x3f) > 59 ||
                    (adi_read(RTC_SEC) & 0x3f) > 59;
    rtc_datetime_t t;

    if (f_open(&f, CLOCK_FILE, FA_READ) == FR_OK) {
        f_read(&f, buf, sizeof(buf) - 1, &n);
        f_close(&f);
        buf[n] = 0;
        for (char *p = buf; *p >= '0' && *p <= '9'; p++)
            saved = saved * 10 + (uint32_t)(*p - '0');
    }
    if (saved / 86400u > last_day)      /* saved from a clock that had gone wrong */
        saved = 0;
    /* the clock is saved all the time it runs, and runs while the phone is off:
     * far past the saved day is as wrong (a year past the build is always fine) */
    if (saved) {
        last_day = saved / 86400u + SAVED_DAYS;
        if (last_day < built / 86400u + 366u)
            last_day = built / 86400u + 366u;
    }
    /* With the battery out the counters can come back with anything: the day
     * counter read 0xaaaa, 2099-08-14, on 2026-11-09 (the time of day changed too),
     * ahead of the saved time so nothing below caught it. */
    if (day > last_day || time_bad) {
        to = saved ? saved : built;
        klog("rtc: day %lu at boot cannot be right: set to the %s time", (unsigned long)day,
             saved ? "saved" : "build's");
    } else if (saved && now + 60 < saved) {
        to = saved;
        klog("rtc: day %lu at boot is behind the saved day %lu: the clock was cleared, "
             "set back to the saved time", (unsigned long)(now / 86400u),
             (unsigned long)(saved / 86400u));
    } else {
        return;                         /* the clock kept going */
    }
    if (!rtc_date_from_days(to / 86400u, &t))
        return;
    t.hour = (uint8_t)(to % 86400u / 3600u);
    t.min = (uint8_t)(to % 3600u / 60u);
    t.sec = (uint8_t)(to % 60u);
    rtc_set(&t);
}

uint32_t rtc_fattime(void)
{
    rtc_datetime_t t;

    if (rtc_get(&t) != 0)
        return (uint32_t)(2026 - 1980) << 25 | 1u << 21 | 1u << 16;
    return (uint32_t)(t.year - 1980) << 25 | (uint32_t)t.month << 21 |
           (uint32_t)t.day << 16 | (uint32_t)t.hour << 11 |
           (uint32_t)t.min << 5 | t.sec / 2u;
}
