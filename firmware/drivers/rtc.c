/*
 * B310E-OS — drivers/rtc.c
 *
 * Real-time clock: SC6530 on-die RTC in the ANA block (no external RTC
 * chip on the B310E). Register map decoded from rtc_phy_v5.c in the kern
 * region of dump_firmware.bin (@0x03614C):
 *
 *   - 0x82001600/04/08 = seconds/minutes/hours (binary, masked 0x3f/0x3f/0x1f)
 *   - 0x8200160C      = date, 16-bit (day+month packing EMPIRICAL)
 *   - 0x82001610/14/18/1C = year/month/day/time write shadows
 *   - 0x82001630      = control; OR 0xFF02 unlocks writes (stock path)
 *   - 0x82001634      = update flag (bit 0 set while the RTC is updating)
 *   - 0x82001638      = latch trigger after writing the shadow regs
 * The stock code waits for the update flag to clear (bounded) before
 * touching the time registers (RTC_CheckIfRtcUpdating @0x35e22, mask
 * 0xFF1F @0x361c4; the read path @0x360a2 reads the four time registers
 * with 6/6/5/16-bit masks).
 *
 * SAFETY: ADI mailbox access only (same primitives as led.c/keypad.c),
 * bounded waits — a wedged RTC degrades to a no-op, never hangs the
 * cooperative scheduler. HOST_TEST compiles the hardware path out.
 */

#include "rtc.h"

#include <stddef.h>         /* NULL */
#include "../arch/chip.h"   /* MEM4 */

#ifndef HOST_TEST
#define RTC_ADI_RD_CMD    0x82000018u
#define RTC_ADI_RD_DATA   0x8200001cu
#define RTC_ADI_FIFO_STS  0x82000020u
#define RTC_ADI_FIFO_FULL  (1u << 9)
#define RTC_ADI_FIFO_EMPTY (1u << 8)
#define RTC_ADI_BUDGET     1000000u

#define RTC_SEC           0x82001600u
#define RTC_MIN           0x82001604u
#define RTC_HOUR          0x82001608u
#define RTC_DATE          0x8200160cu   /* 16-bit date: (year<<9)|(month<<5)|day */
#define RTC_W_SEC         0x82001610u   /* write shadows (stock set path
                                           @0x36018-0x3603c: sec/min/hour/date16 —
                                           the old "year/month/day/time" naming
                                           was a misread of these registers)  */
#define RTC_W_MIN         0x82001614u
#define RTC_W_HOUR        0x82001618u
#define RTC_W_DATE        0x8200161cu
#define RTC_CTRL          0x82001630u   /* bit 1 = latch (set @0x3606c)       */
#define RTC_UPDATING      0x82001634u   /* bit 0 = update in progress        */

#define RTC_UPD_MASK      0x0000FF1Fu   /* stock mask @0x361c4           */
#define RTC_BUSY_POLLS    10u           /* stock retry count @0x35ec4    */

static uint32_t rtc_adi_read(uint32_t addr)
{
    uint32_t a = 0, n = RTC_ADI_BUDGET;

    MEM4(RTC_ADI_RD_CMD) = addr & 0xfffu;
    while ((a = MEM4(RTC_ADI_RD_DATA)) >> 31)   /* wait busy clear */
        if (--n == 0) break;
    return a & 0xffffu;
}

static void rtc_adi_write(uint32_t addr, uint32_t val)
{
    uint32_t n = RTC_ADI_BUDGET;

    while (MEM4(RTC_ADI_FIFO_STS) & RTC_ADI_FIFO_FULL)  /* FIFO full  */
        if (--n == 0) return;
    MEM4(addr) = val;
    n = RTC_ADI_BUDGET;
    while (!(MEM4(RTC_ADI_FIFO_STS) & RTC_ADI_FIFO_EMPTY))
        if (--n == 0) return;
}

/* Wait for the RTC to finish updating (stock RTC_CheckIfRtcUpdating
 * @0x35e1c-0x35ec8: read 0x82001634, while (val & ~mask) retry, bounded).
 * Returns 0 when idle, -1 on timeout. */
static int rtc_wait_idle(void)
{
    uint32_t n;

    for (n = 0; n < RTC_BUSY_POLLS; n++) {
        uint32_t v = rtc_adi_read(RTC_UPDATING);
        if ((v & ~RTC_UPD_MASK) == 0)
            return 0;
    }
    return -1;
}
#endif /* !HOST_TEST */

int rtc_get_time(rtc_time_t *t)
{
#if !defined(HOST_TEST)
    uint32_t sec, min, hour, date;

    if (t == NULL)
        return -1;
    if (rtc_wait_idle() != 0)
        return -1;

    /* The read path @0x360a2-0x360d4: sec/min 6-bit, hour 5-bit, date
     * 16-bit. Date packing (5 bits each, the only form that reaches years
     * past 2015): day = bits 0-4, month = bits 5-8, year = bits 9-15
     * (0-127 = 2000-2127). */
    sec  = rtc_adi_read(RTC_SEC)  & 0x3fu;
    min  = rtc_adi_read(RTC_MIN)  & 0x3fu;
    hour = rtc_adi_read(RTC_HOUR) & 0x1fu;
    date = rtc_adi_read(RTC_DATE) & 0xffffu;

    t->second  = (uint8_t)sec;
    t->minute  = (uint8_t)min;
    t->hour    = (uint8_t)hour;
    t->day     = (uint8_t)(date & 0x1fu);
    t->month   = (uint8_t)((date >> 5) & 0x1fu);
    t->year    = (uint16_t)((date >> 9) & 0x7fu);   /* 0-127 = 2000-2127 */
    return 0;
#else
    (void)t;
    return 0;
#endif
}

int rtc_set_time(const rtc_time_t *t)
{
#if !defined(HOST_TEST)
    uint32_t date;

    if (t == NULL)
        return -1;
    if (rtc_wait_idle() != 0)
        return -1;

    /* Stock set path @0x35faa-0x36078: clear ctrl bit 1, write the four
     * shadow registers as SEC/MIN/HOUR/DATE16 (NOT year/month/day/time —
     * the old register names were a misread), then set ctrl bit 1 to
     * latch. Date16 packing matches the read: (year<<9)|(month<<5)|day,
     * year 0-99 = 2000-2099. */
    rtc_adi_write(RTC_CTRL, rtc_adi_read(RTC_CTRL) & ~2u);

    date = ((uint32_t)(t->year & 0x7fu) << 9) |
           ((uint32_t)(t->month & 0x1fu) << 5) |
           (uint32_t)(t->day & 0x1fu);

    rtc_adi_write(RTC_W_SEC,  (uint32_t)t->second);
    rtc_adi_write(RTC_W_MIN,  (uint32_t)t->minute);
    rtc_adi_write(RTC_W_HOUR, (uint32_t)t->hour);
    rtc_adi_write(RTC_W_DATE, date);

    rtc_adi_write(RTC_CTRL, rtc_adi_read(RTC_CTRL) | 2u);

    return rtc_wait_idle();
#else
    (void)t;
    return 0;
#endif
}
