/* Richwave RTC6211S FM tuner (SM-B310E U101).
 * Copyright (C) 2026 B310E-OS project
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef _RTC6211_H_
#define _RTC6211_H_

#define HAVE_RADIO_REGION
#define HAVE_RADIO_RSSI

struct rtc6211_dbg_info
{
    uint16_t regs[16];
    bool nor_tables;            /* stock init tables found in NOR */
    bool chip_n;                /* RTC6211N rather than S */
    unsigned i2c_errors;        /* failed I2C transfers since boot */
    unsigned start_retries;     /* power-ups that had to be retried */
};

bool rtc6211_detect(void);
void rtc6211_init(void);
int rtc6211_set(int setting, int value);
int rtc6211_get(int setting);
void rtc6211_dbg_info(struct rtc6211_dbg_info *nfo);
/* Analog FM volume: Rockbox tenths of a dB mapped to the tuner's
 * 0..12 volume steps (the codec gain covers positive values). */
void rtc6211_set_volume_db(int tenth_db);

#ifndef CONFIG_TUNER_MULTI
#define tuner_set rtc6211_set
#define tuner_get rtc6211_get
#endif

#endif /* _RTC6211_H_ */
