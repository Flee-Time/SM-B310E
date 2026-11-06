/* B310E OS — FM radio: the Richwave RTC6211N tuner (U101) on I2C 1.
 *
 * Ported from the Rockbox port's fmradio-i2c-sc6530c.c and rtc6211.c
 * (GPLv2-or-later, verified on the phone 2026-11-06), which follow the
 * stock e52q7a driver:
 * - I2C controller 1 at 0x86001000 (stock i2c_phy_v5: init 0xa6ba4, start
 *   0xa6c2a, write 0xa6cc4, read 0xa6d7e, stop 0xa6e06), 100 kHz.
 * - Tuner power: FM_RST is GPIO31 (bank 1 bit 15, low = off); the 32 kHz
 *   reference leaves on the IISMCK pad in function 1 (stock FM_ConfigClk
 *   0xa1484), the one pinmux register written here, restored on off.
 * - Si470x-style framing at address 0xc8: writes start at register 2,
 *   reads at 0x0a, both wrap 0x0f -> 0x00.
 * - The N variant (this phone's) reads 0xffff but for reg00 until a
 *   turn-on stream sets reg00 = 0x96aa; then reg01[15:10] = 4. Its five
 *   46-byte bank tables are read from this phone's own NOR at run time
 *   (0xae9ec..) and used only when their CRC matches the stock image, so
 *   no stock bytes are carried. The S variant is handled the same way.
 *
 * The audio itself takes the analog line-in bypass (audio_fm_route); the
 * headset cable is the antenna.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/arch.h"
#include "os/audio.h"
#include "os/drivers.h"
#include "os/fs.h"
#include "os/kernel.h"
#include "os/lib.h"
#include "drivers/audio_internal.h"

/* ---- I2C 1 -------------------------------------------------------------- */
#define I2C_CTL     REG32(0x86001000u)  /* b0 INT, b3 IE, b4 EN */
#define I2C_CMD     REG32(0x86001004u)  /* data 15:8 */
#define I2C_CLKD0   REG32(0x86001008u)
#define I2C_CLKD1   REG32(0x8600100cu)
#define I2C_RST     REG32(0x86001010u)
#define CMD_INT_ACK 0x01
#define CMD_NACK    0x02
#define CMD_WRITE   0x04
#define CMD_READ    0x08
#define CMD_STOP    0x10
#define CMD_START   0x20
#define CMD_RXNACK  0x40
#define CMD_BUSY    0x80
#define I2C_BUDGET  200000u

#define FM_RST_BIT  (1u << 15)
#define GPIO1_DATA  REG32(0x8a000080u)
#define GPIO1_DMSK  REG32(0x8a000084u)
#define GPIO1_DIR   REG32(0x8a000088u)
#define PIN_FM_32K  REG32(0x8c00017cu)
#define PIN_FM_32K_ON 0x111u

static mutex_t lock;
static bool lock_ready, powered;
static uint32_t pin_saved;
static unsigned i2c_errors;

static void i2c_hw_init(void)
{
    REG32(0x8b0000a0) = 0x80000000u;    /* I2C1 APB clock */
    REG32(0x8b000060) = 1u << 25;       /* reset pulse */
    udelay(10);
    REG32(0x8b000064) = 1u << 25;
    I2C_RST = 1;
    I2C_CTL &= ~0x38u;
    I2C_CLKD0 = 26000000 / (4 * 100000) - 1;    /* 100 kHz */
    I2C_CLKD1 = 0;
    REG32(0x8b0001e0) |= 2;             /* APB_PIN_CTL0: I2C1 pad select */
    I2C_CTL |= 0x18;
    I2C_CMD |= CMD_INT_ACK;
}

/* One command, wait, acknowledge: the data byte, or -1. */
static int i2c_cmd(uint32_t cmd, bool check_ack)
{
    unsigned n = I2C_BUDGET;
    uint32_t value;

    I2C_CMD = cmd;
    while (!(I2C_CTL & 1))
        if (!--n)
            return -1;
    n = I2C_BUDGET;
    while ((value = I2C_CMD) & CMD_BUSY)
        if (!--n)
            return -1;
    I2C_CMD = value | CMD_INT_ACK;
    if (check_ack && (I2C_CMD & CMD_RXNACK))
        return -1;
    return (int)((value >> 8) & 0xff);
}

static int i2c_write(uint8_t addr, const uint8_t *buf, int count)
{
    int ret = 0;

    if (i2c_cmd((uint32_t)(addr & 0xfe) << 8 | CMD_START | CMD_WRITE, true) < 0)
        ret = -1;
    for (int i = 0; !ret && i < count; i++) {
        uint32_t cmd = (uint32_t)buf[i] << 8 | CMD_WRITE;

        if (i == count - 1)
            cmd |= CMD_STOP;
        if (i2c_cmd(cmd, true) < 0)
            ret = -1;
    }
    if (ret) {
        i2c_cmd(CMD_STOP, false);
        i2c_errors++;
    }
    return ret;
}

static int i2c_read(uint8_t addr, uint8_t *buf, int count)
{
    int ret = 0;

    if (i2c_cmd((uint32_t)(addr | 1) << 8 | CMD_START | CMD_WRITE, true) < 0)
        ret = -1;
    for (int i = 0; !ret && i < count; i++) {
        uint32_t cmd = i == count - 1 ? CMD_READ | CMD_STOP | CMD_NACK : CMD_READ;
        int v = i2c_cmd(cmd, false);

        if (v < 0)
            ret = -1;
        else
            buf[i] = (uint8_t)v;
    }
    if (ret) {
        i2c_cmd(CMD_STOP, false);
        i2c_errors++;
    }
    return ret;
}

/* Reset low >= 10 ms with the 32 kHz clock running, then >= 100 ms awake. */
static void tuner_power(bool on)
{
    uint32_t s;

    if (on == powered)
        return;
    if (on) {
        gpio_clock_on();
        i2c_hw_init();
        pin_saved = PIN_FM_32K;
        PIN_FM_32K = PIN_FM_32K_ON;
        s = arch_irq_save();
        GPIO1_DMSK |= FM_RST_BIT;
        GPIO1_DATA &= ~FM_RST_BIT;
        GPIO1_DIR |= FM_RST_BIT;
        arch_irq_restore(s);
        thread_sleep(11);
        s = arch_irq_save();
        GPIO1_DATA |= FM_RST_BIT;
        arch_irq_restore(s);
        thread_sleep(101);
    } else {
        s = arch_irq_save();
        GPIO1_DATA &= ~FM_RST_BIT;
        arch_irq_restore(s);
        PIN_FM_32K = pin_saved;
    }
    powered = on;
}

/* ---- RTC6211 ------------------------------------------------------------ */
#define ADDR        0xc8
#define DEVICEID    0x0                 /* 0x1688 */
#define CHIPID      0x1                 /* [15:10]: 3 S, 4 N */
#define POWERCFG    0x2
#define DSMUTE      (1u << 15)
#define DMUTE       (1u << 14)          /* 1 = audio on */
#define CHANNEL     0x3
#define TUNE        (1u << 15)
#define CHAN_MASK   0x03ff
#define SEEKCFG     0x5
#define SEEK        (1u << 15)
#define SEEK_TH     0x00ff
#define POWER       0x6
#define POWER_EN    (1u << 15)
#define POWER_DIS   (1u << 14)
#define BANKSEL     0x8
#define STATUS      0xa
#define STC         (1u << 14)
#define ST          (1u << 10)
#define RSSI        0xb
#define SEEK_THRESHOLD 0x13             /* stock NV 0x1b5 default */
#define VOLUME_MAX  12
#define BAND_BASE   8750                /* 10 kHz units, Europe/US band */
#define BAND_SPACE  10                  /* 100 kHz */
#define SPACE_BITS  (1u << 10)
#define N_TURNOFF   0x16aa
#define N_TURNON    0x96aa
#define TABLE_LEN   46

static const uint32_t s_tables[6] = { 0xa1990, 0xa19c0, 0xa19f0, 0xa1a20, 0xa1a50, 0xa1aa0 };
static const uint32_t n_tables[5] = { 0xae9ec, 0xaea1c, 0xaea4c, 0xaea7c, 0xaeaac };
#define S_TABLES_CRC 0x871c1aa4u
#define N_TABLES_CRC 0xa305ae11u

static int probed = -1;                 /* -1 not yet, 0 absent, 1 present */
static bool chip_n, tables_ok, is_on, muted;
static uint16_t cache[16];
static int volume = 9, freq_khz = 100000;

/* Rockbox's crc_32: polynomial 0x04c11db7, MSB first, no final xor */
static uint32_t crc32_msb(const uint8_t *p, uint32_t len, uint32_t crc)
{
    while (len--) {
        crc ^= (uint32_t)*p++ << 24;
        for (int b = 0; b < 8; b++)
            crc = crc & 0x80000000u ? crc << 1 ^ 0x04c11db7u : crc << 1;
    }
    return crc;
}

static void rd(int count)
{
    uint8_t buf[32];

    if (i2c_read(ADDR, buf, count * 2) < 0)
        return;
    for (int i = 0; i < count; i++) {   /* status and ID registers only */
        int reg = (STATUS + i) & 15;

        if (reg >= STATUS || reg <= CHIPID)
            cache[reg] = (uint16_t)(buf[2 * i] << 8 | buf[2 * i + 1]);
    }
}

static void wr(int count)
{
    uint8_t buf[32];

    for (int i = 0; i < count; i++) {
        uint16_t v = cache[(POWERCFG + i) & 15];

        buf[2 * i] = (uint8_t)(v >> 8);
        buf[2 * i + 1] = (uint8_t)v;
    }
    i2c_write(ADDR, buf, count * 2);
}

static void write_raw(const uint16_t *regs, int count)
{
    uint8_t buf[32];

    for (int i = 0; i < count; i++) {
        buf[2 * i] = (uint8_t)(regs[i] >> 8);
        buf[2 * i + 1] = (uint8_t)regs[i];
    }
    i2c_write(ADDR, buf, count * 2);
}

static void n_reg00(uint16_t value)     /* reg02..0f = 0, then reg00 */
{
    uint16_t regs[15] = { [14] = value };

    write_raw(regs, 15);
}

static bool check_tables(void)
{
    uint32_t crc = 0xffffffffu;

    if (chip_n) {
        crc = crc32_msb((const uint8_t *)0xae9ac, 30, crc);   /* turnoff */
        crc = crc32_msb((const uint8_t *)0xae9cc, 30, crc);   /* turnon */
        for (int i = 0; i < 5; i++)
            crc = crc32_msb((const uint8_t *)n_tables[i], TABLE_LEN, crc);
        return crc == N_TABLES_CRC;
    }
    for (int i = 0; i < 6; i++)
        crc = crc32_msb((const uint8_t *)s_tables[i], TABLE_LEN, crc);
    return crc == S_TABLES_CRC;
}

static void write_table(int index)
{
    uint8_t buf[TABLE_LEN];

    memcpy(buf, (const void *)(chip_n ? n_tables[index] : s_tables[index]), sizeof(buf));
    if (!chip_n && index == 5)
        buf[0x16] = 0x10;               /* stock forces this byte */
    i2c_write(ADDR, buf, sizeof(buf));
}

static void apply_powercfg(void)
{
    int v = muted ? 0 : volume;

    cache[POWERCFG] = (uint16_t)(DSMUTE | (v ? DMUTE : 0) | (v & 0xf));
    wr(1);
}

/* Stock probe 0xaf2be: the S answers at once, the N needs off and on. */
static bool detect(void)
{
    chip_n = false;
    rd(16);
    if (cache[DEVICEID] != 0x1688)
        return false;
    if ((cache[CHIPID] >> 10) == 3)
        return true;
    thread_sleep(201);
    n_reg00(N_TURNOFF);
    thread_sleep(31);
    n_reg00(N_TURNON);
    thread_sleep(51);
    rd(16);
    chip_n = cache[DEVICEID] == 0x1688 && (cache[CHIPID] >> 10) == 4;
    return chip_n;
}

/* Stock N Initial 0xaed62 + PowerOn(1) 0xae758 */
static void start_n(void)
{
    static const uint16_t swbk0[8] = { 0, 0, 0, 0, 0x8000, 0, 0, 0x4050 };
    static const uint16_t pwron[6] = { 0xc00f, 0x0400, 0, 0x000c, 0x8000, 0x7000 };

    rd(16);
    memset(cache, 0, sizeof(cache));
    cache[7] = 0x7000;
    cache[9] = 0x4000;
    thread_sleep(31);
    n_reg00(N_TURNOFF);
    thread_sleep(31);
    n_reg00(N_TURNON);
    thread_sleep(31);
    if (tables_ok)
        for (int i = 0; i < 5; i++) {
            write_table(i);
            thread_sleep(31);
        }
    write_raw(swbk0, 8);
    thread_sleep(31);
    write_raw(pwron, 6);
    thread_sleep(31);
    cache[POWER] = (uint16_t)((cache[POWER] | POWER_EN) & ~POWER_DIS);
    wr(5);
    thread_sleep(61);
    cache[CHANNEL] = SPACE_BITS;
    wr(2);
    cache[SEEKCFG] = (uint16_t)((cache[SEEKCFG] & ~SEEK_TH) | SEEK_THRESHOLD);
    wr(4);
    rd(16);
    cache[SEEKCFG] &= (uint16_t)~SEEK;
    wr(4);
    muted = true;
    apply_powercfg();
}

/* Stock 0xa1836 + 0xa1650, the S */
static void start_s(void)
{
    memset(cache, 0, sizeof(cache));
    cache[7] = 0x7000;
    cache[9] = 0x4000;
    thread_sleep(201);
    if (tables_ok)
        for (int i = 0; i < 5; i++) {
            write_table(i);
            thread_sleep(31);
        }
    cache[POWER] = POWER_EN;
    cache[9] = 0x4050;
    wr(8);
    thread_sleep(151);
    cache[BANKSEL] = 0x4000;
    wr(7);
    rd(8);
    if (tables_ok) {
        write_table(5);
        thread_sleep(31);
    }
    cache[POWER] = POWER_EN;
    wr(5);
    thread_sleep(11);
    cache[CHANNEL] = SPACE_BITS;
    wr(2);
    cache[SEEKCFG] = (uint16_t)((cache[SEEKCFG] & ~(SEEK_TH | SEEK)) | SEEK_THRESHOLD);
    wr(4);
    muted = true;
    apply_powercfg();
}

/* A start that hit an I2C error, or after which the chip does not read
 * back awake, stays silent until the next power cycle: retry from reset. */
static bool power_up(void)
{
    for (int attempt = 0; attempt < 3; attempt++) {
        unsigned errors = i2c_errors;

        tuner_power(true);
        if (chip_n)
            start_n();
        else
            start_s();
        rd(16);
        if (cache[DEVICEID] == 0x1688 && (cache[CHIPID] >> 10) == (chip_n ? 4 : 3) &&
            i2c_errors == errors)
            return true;
        klog("fm: start attempt %d failed", attempt);
        tuner_power(false);
        thread_sleep(50);
    }
    return false;
}

static void set_frequency(int khz)
{
    int chan = (khz / 10 - BAND_BASE) / BAND_SPACE;

    if (chan < 0)
        chan = 0;
    cache[SEEKCFG] &= (uint16_t)~SEEK;
    wr(4);
    cache[CHANNEL] = (uint16_t)(TUNE | SPACE_BITS | (chan & CHAN_MASK));
    wr(2);
    for (int i = 0; i < 20; i++) {      /* stock: 20 x 50 ms */
        rd(2);
        if (cache[STATUS] & STC)
            break;
        thread_sleep(50);
    }
    cache[CHANNEL] &= (uint16_t)~TUNE;
    wr(2);
    freq_khz = (BAND_BASE + chan * BAND_SPACE) * 10;
}

static void lock_init(void)
{
    if (!lock_ready) {
        mutex_init(&lock);
        lock_ready = true;
    }
}

/* ---- the public side ---------------------------------------------------- */

bool fm_present(void)
{
    lock_init();
    mutex_lock(&lock, WAIT_FOREVER);
    if (probed < 0) {
        tuner_power(true);
        probed = detect();
        tables_ok = check_tables();
        klog("fm: id %04x %04x, present %d, N %d, NOR tables %s", cache[DEVICEID],
             cache[CHIPID], probed, chip_n, tables_ok ? "ok" : "not matching");
        tuner_power(false);
    }
    mutex_unlock(&lock);
    return probed > 0;
}

int fm_on(void)
{
    int rc = 0;

    if (!fm_present())
        return -1;
    mutex_lock(&lock, WAIT_FOREVER);
    if (!is_on) {
        if (power_up()) {
            set_frequency(freq_khz);
            muted = false;
            apply_powercfg();
            is_on = true;
        } else {
            rc = -1;
        }
    }
    mutex_unlock(&lock);
    if (!rc)
        audio_fm_route(true);
    return rc;
}

void fm_off(void)
{
    lock_init();
    if (is_on)
        audio_fm_route(false);
    mutex_lock(&lock, WAIT_FOREVER);
    if (is_on) {
        muted = true;
        apply_powercfg();
        if (chip_n) {                   /* stock N close 0xaeeb6 */
            static const uint16_t pwroff[5] = { 0xc00f, 0x0400, 0, 0x000c, 0xc000 };

            write_raw(pwroff, 5);
            thread_sleep(31);
            n_reg00(N_TURNOFF);
            thread_sleep(31);
        }
        cache[POWER] &= (uint16_t)~(POWER_EN | POWER_DIS);
        wr(5);
        tuner_power(false);
        is_on = false;
    }
    mutex_unlock(&lock);
    audio_power(false);
}

bool fm_is_on(void)
{
    return is_on;
}

void fm_tune(int khz)
{
    if (khz < 87500) khz = 108000;
    if (khz > 108000) khz = 87500;
    lock_init();
    mutex_lock(&lock, WAIT_FOREVER);
    if (is_on)
        set_frequency(khz);
    else
        freq_khz = khz;
    mutex_unlock(&lock);
}

int fm_frequency(void)
{
    return freq_khz;
}

int fm_rssi(void)
{
    int r = 0;

    lock_init();
    mutex_lock(&lock, WAIT_FOREVER);
    if (is_on) {
        rd(2);
        r = cache[RSSI] & 0xff;
    }
    mutex_unlock(&lock);
    return r;
}

bool fm_stereo(void)
{
    bool st = false;

    lock_init();
    mutex_lock(&lock, WAIT_FOREVER);
    if (is_on) {
        rd(1);
        st = (cache[STATUS] & ST) != 0;
    }
    mutex_unlock(&lock);
    return st;
}

/* A station: the RSSI at or above the stock seek threshold. Seeking is
 * done a channel at a time by the caller (Rockbox's scan does the same):
 * a whole sweep blocked the UI for minutes (2026-11-07). */
bool fm_station(int rssi)
{
    return rssi >= SEEK_THRESHOLD;
}

void fm_mute(bool m)
{
    lock_init();
    mutex_lock(&lock, WAIT_FOREVER);
    muted = m;
    if (is_on)
        apply_powercfg();
    mutex_unlock(&lock);
}

/* From the codec's volume (tenths of a dB): full tuner volume at 0 dB and
 * above, about 2 dB per tuner step below, muting at the bottom. */
void fm_set_volume_db10(int db10)
{
    int v = VOLUME_MAX + db10 / 20;

    volume = v < 0 ? 0 : v > VOLUME_MAX ? VOLUME_MAX : v;
    if (is_on && lock_ready && !mutex_lock(&lock, 0)) {
        apply_powercfg();
        mutex_unlock(&lock);
    }
}
