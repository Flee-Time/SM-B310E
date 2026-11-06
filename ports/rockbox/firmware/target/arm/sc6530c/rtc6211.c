/* Richwave RTC6211S FM tuner driver (SM-B310E U101).
 * Copyright (C) 2026 B310E-OS project
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Behaviour from the e52q7a stock RTC6211S driver (NOR 0xa1578..0xa1ec6):
 * - Si470x-style block framing: writes start at register 0x02, reads at
 *   0x0A; both wrap 0x0F -> 0x00. The 8-bit address is 0xC8.
 * - The register bits are the Richwave RTC6213 layout, not the Si4702 one.
 * - Two variants. The S (reg01[15:10] = 3, stock 0xa1578..) streams six
 *   46-byte bank tables (NOR 0xa1990..0xa1aa0). The N (stock 0xae680..,
 *   probe 0xaf2be) reads 0xFFFF everywhere but reg00 until a "turnon"
 *   stream sets reg00 = 0x96AA; then reg01[15:10] = 4. It streams five
 *   bank tables (NOR 0xae9ec..0xaeaac). The SM-B310E tested has the N.
 * - Bank tables are read from this phone's own NOR at run time and used
 *   only when their CRC matches the stock image; the port does not carry
 *   the bytes.
 * Structure follows Rockbox's rda5802 driver. See docs/rockbox-fmradio.md.
 */
#include "config.h"
#include <stdbool.h>
#include <string.h>
#include "kernel.h"
#include "power.h"
#include "tuner.h"
#include "fmradio.h"
#include "fmradio_i2c.h"
#include "crc32.h"
#include "logf.h"

#define I2C_ADR 0xc8

#define RSSI_MIN 0
#define RSSI_MAX 80
#define SEEK_THRESHOLD 0x13             /* stock NV 0x1b5 default */

/* Registers and bits (stock usage) */
#define DEVICEID    0x0                 /* 0x1688 */
#define CHIPID      0x1                 /* bits 15:10 = 3 for RTC6211S */
#define POWERCFG    0x2
#define POWERCFG_DSMUTE   (1u << 15)
#define POWERCFG_DMUTE    (1u << 14)    /* 1 = audio on */
#define POWERCFG_VOLUME   0x000f
#define CHANNEL     0x3
#define CHANNEL_TUNE      (1u << 15)
#define CHANNEL_BAND      (3u << 12)
#define CHANNEL_SPACE     (3u << 10)
#define CHANNEL_CHAN      0x03ff
#define SEEKCFG     0x5
#define SEEKCFG_SEEK      (1u << 15)
#define SEEKCFG_SEEKUP    (1u << 14)
#define SEEKCFG_TH        0x00ff
#define POWER       0x6
#define POWER_ENABLE      (1u << 15)
#define POWER_DISABLE     (1u << 14)
#define BANKSEL     0x8
#define STATUS      0xA
#define STATUS_STC        (1u << 14)
#define STATUS_SF         (1u << 13)
#define STATUS_ST         (1u << 10)
#define STATUS_CHAN       0x03ff
#define RSSI        0xB

#define VOLUME_MAX 12                   /* stock clamps at 12 */

static const uint32_t nor_tables[6] = {
    0xa1990, 0xa19c0, 0xa19f0, 0xa1a20, 0xa1a50, 0xa1aa0,
};
#define NOR_TABLE_LEN 46
#define NOR_TABLE_CRC 0x871c1aa4u       /* e52q7a, Rockbox crc_32 */

/* RTC6211N: turnoff (30) and turnon (30) at 0xae9ac/0xae9cc, then the
 * swbk1/2/4/5/7 bank tables. The CRC covers all seven; the two 30-byte
 * streams are just reg02..0F = 0 then reg00, so they are built here. */
static const uint32_t n_tables[5] = {
    0xae9ec, 0xaea1c, 0xaea4c, 0xaea7c, 0xaeaac,
};
#define N_TABLES_CRC 0xa305ae11u
#define N_TURNOFF 0x16aa
#define N_TURNON  0x96aa

static bool tuner_present, tables_ok, chip_n;
static unsigned i2c_errors;              /* failed transfers, all time */
static unsigned start_retries;           /* power-ups that needed a retry */
static uint16_t cache[16];
static int band_base = 8750, band_space = 10;   /* 10 kHz units */
static uint16_t band_bits = 0, space_bits = 1u << 10;
static int volume = 9;                  /* stock default */
static bool muted;

static void rtc_read(int count)
{
    unsigned char buf[32];
    if (fmradio_i2c_read(I2C_ADR, buf, count * 2) < 0)
    {
        i2c_errors++;
        return;
    }
    /* Like stock, keep reads apart from the write shadow (reg02..09):
     * only status and ID registers (0A..0F, 00, 01) are taken. */
    for (int i = 0; i < count; i++)
    {
        int reg = (STATUS + i) & 15;
        if (reg >= STATUS || reg <= CHIPID)
            cache[reg] = buf[2 * i] << 8 | buf[2 * i + 1];
    }
}

static void rtc_write(int count)
{
    unsigned char buf[32];
    for (int i = 0; i < count; i++)
    {
        uint16_t value = cache[(POWERCFG + i) & 15];
        buf[2 * i] = value >> 8;
        buf[2 * i + 1] = value;
    }
    if (fmradio_i2c_write(I2C_ADR, buf, count * 2) < 0)
        i2c_errors++;
}

static bool check_nor_tables(void)
{
    uint32_t crc = 0xffffffff;
    if (chip_n)
    {
        crc = crc_32((const void *)0xae9ac, 30, crc);   /* turnoff */
        crc = crc_32((const void *)0xae9cc, 30, crc);   /* turnon */
        for (int i = 0; i < 5; i++)
            crc = crc_32((const void *)n_tables[i], NOR_TABLE_LEN, crc);
        return crc == N_TABLES_CRC;
    }
    for (int i = 0; i < 6; i++)
        crc = crc_32((const void *)nor_tables[i], NOR_TABLE_LEN, crc);
    return crc == NOR_TABLE_CRC;
}

static void write_nor_table(int index)
{
    unsigned char buf[NOR_TABLE_LEN];
    memcpy(buf, (const void *)(chip_n ? n_tables[index] : nor_tables[index]),
           sizeof(buf));
    if (!chip_n && index == 5)
        buf[0x16] = 0x10;               /* stock forces this byte */
    if (fmradio_i2c_write(I2C_ADR, buf, sizeof(buf)) < 0)
        i2c_errors++;
}

static void write_raw(const uint16_t *regs, int count)
{
    unsigned char buf[32];
    for (int i = 0; i < count; i++)
    {
        buf[2 * i] = regs[i] >> 8;
        buf[2 * i + 1] = regs[i];
    }
    if (fmradio_i2c_write(I2C_ADR, buf, count * 2) < 0)
        i2c_errors++;
}

/* N only: reg02..0F = 0, then reg00 (the stream wraps 0x0F -> 0x00). */
static void n_reg00(uint16_t value)
{
    uint16_t regs[15] = { [14] = value };
    write_raw(regs, 15);
}

static void apply_powercfg(void)
{
    int vol = muted ? 0 : volume;
    cache[POWERCFG] = POWERCFG_DSMUTE | (vol ? POWERCFG_DMUTE : 0) |
                      (vol & POWERCFG_VOLUME);
    rtc_write(1);
}

/* Stock probe 0xaf2be: the S answers at once; the N needs turnoff and
 * turnon before reg01 reads back. */
bool rtc6211_detect(void)
{
    chip_n = false;
    rtc_read(16);
    if (cache[DEVICEID] != 0x1688)
        return false;
    if ((cache[CHIPID] >> 10) == 3)
        return true;
    sleep(HZ / 5 + 1);
    n_reg00(N_TURNOFF);
    sleep(HZ * 3 / 100 + 1);
    n_reg00(N_TURNON);
    sleep(HZ / 20 + 1);
    rtc_read(16);
    chip_n = cache[DEVICEID] == 0x1688 && (cache[CHIPID] >> 10) == 4;
    return chip_n;
}

/* Stock N Initial 0xaed62 + PowerOn(1) 0xae758. */
static void rtc6211n_start(void)
{
    static const uint16_t swbk0[8] = { 0, 0, 0, 0, 0x8000, 0, 0, 0x4050 };
    static const uint16_t pwron[6] = { 0xc00f, 0x0400, 0, 0x000c, 0x8000, 0x7000 };
    rtc_read(16);
    memset(cache, 0, sizeof(cache));
    cache[7] = 0x7000;
    cache[9] = 0x4000;
    sleep(HZ * 3 / 100 + 1);
    n_reg00(N_TURNOFF);
    sleep(HZ * 3 / 100 + 1);
    n_reg00(N_TURNON);
    sleep(HZ * 3 / 100 + 1);
    if (tables_ok)
        for (int i = 0; i < 5; i++)
        {
            write_nor_table(i);
            sleep(HZ * 3 / 100 + 1);
        }
    write_raw(swbk0, 8);
    sleep(HZ * 3 / 100 + 1);
    write_raw(pwron, 6);
    sleep(HZ * 3 / 100 + 1);
    cache[POWER] = (cache[POWER] | POWER_ENABLE) & ~POWER_DISABLE;
    rtc_write(5);
    sleep(HZ * 6 / 100 + 1);
    cache[CHANNEL] = band_bits | space_bits;
    rtc_write(2);
    cache[SEEKCFG] = (cache[SEEKCFG] & ~SEEKCFG_TH) | SEEK_THRESHOLD;
    rtc_write(4);
    rtc_read(16);
    cache[SEEKCFG] &= ~SEEKCFG_SEEK;
    rtc_write(4);
    muted = true;
    apply_powercfg();
}

static void rtc6211_start(void)
{
    if (chip_n)
    {
        rtc6211n_start();
        return;
    }
    /* Stock 0xa1836 + 0xa1650, RTC6211S. */
    memset(cache, 0, sizeof(cache));
    cache[7] = 0x7000;
    cache[9] = 0x4000;
    sleep(HZ / 5 + 1);
    if (tables_ok)
        for (int i = 0; i < 5; i++)
        {
            write_nor_table(i);
            sleep(HZ * 3 / 100 + 1);
        }
    cache[POWER] = POWER_ENABLE;
    cache[9] = 0x4050;
    rtc_write(8);
    sleep(HZ * 15 / 100 + 1);
    cache[BANKSEL] = 0x4000;
    rtc_write(7);
    rtc_read(8);
    if (tables_ok)
    {
        write_nor_table(5);
        sleep(HZ * 3 / 100 + 1);
    }
    cache[POWER] = POWER_ENABLE;
    rtc_write(5);
    sleep(HZ / 100 + 1);
    cache[CHANNEL] = band_bits | space_bits;
    rtc_write(2);
    cache[SEEKCFG] = (cache[SEEKCFG] & ~(SEEKCFG_TH | SEEKCFG_SEEK)) | SEEK_THRESHOLD;
    rtc_write(4);
    muted = true;
    apply_powercfg();
}

/* Power up and run the start sequence. A start that hit an I2C error,
 * or after which the chip does not read back awake, leaves it silent
 * until the next power cycle, so retry from reset (up to 3 attempts). */
static void power_up(void)
{
    for (int attempt = 0; attempt < 3; attempt++)
    {
        unsigned errors = i2c_errors;
        tuner_power(true);
        rtc6211_start();
        rtc_read(16);
        bool awake = cache[DEVICEID] == 0x1688 &&
                     (cache[CHIPID] >> 10) == (chip_n ? 4 : 3);
        if (awake && i2c_errors == errors)
            return;
        start_retries++;
        logf("rtc6211: start attempt %d failed (errors %u)", attempt,
             i2c_errors - errors);
        tuner_power(false);
        sleep(HZ / 20);
    }
}

void rtc6211_init(void)
{
    tuner_power(true);
    tuner_present = rtc6211_detect();
    tables_ok = check_nor_tables();
    logf("rtc6211: id %04x %04x present %d N %d tables %d", cache[DEVICEID],
         cache[CHIPID], tuner_present, chip_n, tables_ok);
    tuner_power(false);
}

static void set_frequency(int freq)
{
    int chan = (freq / 10000 - band_base) / band_space;
    if (chan < 0)
        chan = 0;
    cache[SEEKCFG] &= ~SEEKCFG_SEEK;
    rtc_write(4);
    cache[CHANNEL] = CHANNEL_TUNE | band_bits | space_bits | (chan & CHANNEL_CHAN);
    rtc_write(2);
    for (int i = 0; i < 20; i++)        /* stock: 20 x 50 ms */
    {
        rtc_read(2);
        if (cache[STATUS] & STATUS_STC)
            break;
        sleep(HZ / 20);
    }
    cache[CHANNEL] &= ~CHANNEL_TUNE;
    rtc_write(2);
}

static void set_region(int region)
{
    const struct fm_region_data *rd = &fm_region_data[region];
    if (rd->freq_min >= 87500000)
        band_bits = 0, band_base = 8750;
    else if (rd->freq_max > 91000000)
        band_bits = 1u << 12, band_base = 7600;
    else
        band_bits = 2u << 12, band_base = 7600;
    if (rd->freq_step >= 200000)
        space_bits = 0, band_space = 20;
    else if (rd->freq_step >= 100000)
        space_bits = 1u << 10, band_space = 10;
    else
        space_bits = 2u << 10, band_space = 5;
    if (tuner_powered())
    {
        cache[CHANNEL] = (cache[CHANNEL] & CHANNEL_CHAN) | band_bits | space_bits;
        rtc_write(2);
    }
}

static int rssi(void)
{
    rtc_read(2);
    return cache[RSSI] & 0xff;
}

void rtc6211_set_volume_db(int tenth_db)
{
    /* 0 dB and above: full tuner volume, codec gain does the rest. Below,
     * about 2 dB per tuner step (unmeasured), muting at the bottom. */
    int v = VOLUME_MAX + tenth_db / 20;
    volume = v < 0 ? 0 : v > VOLUME_MAX ? VOLUME_MAX : v;
    if (tuner_present && tuner_powered())
        apply_powercfg();
}

int rtc6211_set(int setting, int value)
{
    if (!tuner_present && setting != RADIO_REGION)
        return -1;
    switch (setting)
    {
    case RADIO_SLEEP:
        if (value)
        {
            if (tuner_powered())
            {
                muted = true;
                apply_powercfg();
                if (chip_n)
                {
                    /* Stock N close 0xaeeb6: rPwrOff, then turnoff. */
                    static const uint16_t pwroff[5] =
                        { 0xc00f, 0x0400, 0, 0x000c, 0xc000 };
                    write_raw(pwroff, 5);
                    sleep(HZ * 3 / 100 + 1);
                    n_reg00(N_TURNOFF);
                    sleep(HZ * 3 / 100 + 1);
                }
                cache[POWER] &= ~(POWER_ENABLE | POWER_DISABLE);
                rtc_write(5);
            }
            tuner_power(false);
        }
        else if (!tuner_powered())
            power_up();
        break;
    case RADIO_FREQUENCY:
        set_frequency(value);
        break;
    case RADIO_SCAN_FREQUENCY:
        set_frequency(value);
        return rssi() >= SEEK_THRESHOLD;
    case RADIO_MUTE:
        muted = value;
        apply_powercfg();
        break;
    case RADIO_REGION:
        set_region(value);
        break;
    default:
        return -1;
    }
    return 1;
}

int rtc6211_get(int setting)
{
    switch (setting)
    {
    case RADIO_PRESENT:
        return tuner_present;
    case RADIO_TUNED:
        return tuner_powered() && rssi() >= SEEK_THRESHOLD;
    case RADIO_STEREO:
        if (!tuner_powered())
            return 0;
        rtc_read(1);
        return !!(cache[STATUS] & STATUS_ST);
    case RADIO_RSSI:
        return tuner_powered() ? rssi() : RSSI_MIN;
    case RADIO_RSSI_MIN:
        return RSSI_MIN;
    case RADIO_RSSI_MAX:
        return RSSI_MAX;
    }
    return -1;
}

void rtc6211_dbg_info(struct rtc6211_dbg_info *nfo)
{
    if (tuner_powered())
        rtc_read(16);
    memcpy(nfo->regs, cache, sizeof(nfo->regs));
    nfo->nor_tables = tables_ok;
    nfo->i2c_errors = i2c_errors;
    nfo->start_retries = start_retries;
    nfo->chip_n = chip_n;
}
