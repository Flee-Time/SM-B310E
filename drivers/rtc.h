/*
 * B310E-OS — drivers/rtc.h
 *
 * Real-time clock for the SM-B310E (SC6530C). The SC6530 has the RTC
 * on-die in the ANA block — there is NO external RTC chip on the B310E
 * (docs' part list has only U100 RT9532GQW charger, U201 FSUSB42UMX USB
 * switch, U200 SIP32408DNP, U202 LD6816CX4C33P torch — no RTC IC).
 * OSC101 is the 26 MHz main crystal; the RTC runs on the internal 32k
 * (no 32k crystal on the board).
 *
 * Register map (dump-mined from rtc_phy_v5.c in the kern region of
 * dump_firmware.bin, @0x03614C; read path @0x360a2, write @0x36008):
 *   - 0x82001600  seconds (0-59, 6-bit)
 *   - 0x82001604  minutes (0-59, 6-bit)
 *   - 0x82001608  hours   (0-23, 5-bit)
 *   - 0x8200160C  date    (16-bit — day + month packing EMPIRICAL)
 *   - 0x82001610  year    (write shadow)
 *   - 0x82001614  month   (write shadow)
 *   - 0x82001618  day     (write shadow)
 *   - 0x8200161C  time    (write shadow: hour/min/sec)
 *   - 0x82001630  control (write-enable unlock, OR 0xFF02 to enable)
 *   - 0x82001634  update flag (bit 0 set while RTC is updating)
 *   - 0x82001638  latch trigger (write after the shadow regs)
 * The stock code waits for the update flag to clear (bounded, ~10 polls)
 * before/after touching the time registers (RTC_CheckIfRtcUpdating @0x35e22).
 *
 * SAFETY: ADI mailbox access only, bounded waits — a wedged RTC degrades
 * to a no-op. HOST_TEST compiles the hardware path out.
 */

#ifndef B310E_OS_DRIVERS_RTC_H
#define B310E_OS_DRIVERS_RTC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t year;      /* 0-99 (shadow write) / 16-bit read        */
    uint8_t  month;     /* 1-12                                     */
    uint8_t  day;       /* 1-31                                     */
    uint8_t  hour;      /* 0-23                                     */
    uint8_t  minute;    /* 0-59                                     */
    uint8_t  second;    /* 0-59                                     */
} rtc_time_t;

/* Read the current RTC time. Returns 0 on success, -1 if the RTC stayed
 * busy (update flag never cleared — bounded wait). The date fields are
 * MARKED EMPIRICAL (the 0x8200160C packing needs HW verification). */
int rtc_get_time(rtc_time_t *t);

/* Write the RTC time (shadow registers + latch trigger). Returns 0 on
 * success, -1 if the RTC stayed busy. MARKED EMPIRICAL until HW-verified
 * (the stock write path's unlock pattern on 0x82001630 is replicated
 * verbatim; the exact latch semantics need a hardware test). */
int rtc_set_time(const rtc_time_t *t);

#ifdef __cplusplus
}
#endif

#endif /* B310E_OS_DRIVERS_RTC_H */
