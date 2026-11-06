/* SC6530C I2C controller 1 for the RTC6211 FM tuner, plus tuner power.
 * Copyright (C) 2026 B310E-OS project
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Register behaviour from the e52q7a stock i2c_phy_v5 path (NOR 0xa6ba4
 * init, 0xa6c2a start, 0xa6cc4 write, 0xa6d7e read, 0xa6e06 stop) and the
 * FM manager (0x7d714 on, 0x7d81c off). See docs/rockbox-fmradio.md.
 */
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "power.h"
#include "fmradio_i2c.h"

#define REG32(a) (*(volatile uint32_t *)(a))

/* Controller 1 at 0x86001000. */
#define I2C_CTL   REG32(0x86001000u)   /* b0 INT, b3 IE, b4 EN */
#define I2C_CMD   REG32(0x86001004u)   /* data 15:8, see I2C_CMD_* */
#define I2C_CLKD0 REG32(0x86001008u)
#define I2C_CLKD1 REG32(0x8600100cu)
#define I2C_RST   REG32(0x86001010u)
#define I2C_CMD_INT_ACK 0x01
#define I2C_CMD_NACK    0x02           /* master sends NACK (last read) */
#define I2C_CMD_WRITE   0x04
#define I2C_CMD_READ    0x08
#define I2C_CMD_STOP    0x10
#define I2C_CMD_START   0x20
#define I2C_CMD_RXNACK  0x40           /* slave did not ACK */
#define I2C_CMD_BUSY    0x80
#define I2C_BUDGET      200000u

/* FM_RST is GPIO31 (bank 1 bit 15), low = reset/off. */
#define FM_RST_BIT  (1u << 15)
#define GPIO1_DATA  REG32(0x8a000080u)
#define GPIO1_DMSK  REG32(0x8a000084u)
#define GPIO1_DIR   REG32(0x8a000088u)

/* The 32 kHz reference leaves on the IISMCK pad in function 1
 * (stock FM_ConfigClk 0xa1484). Boot default 0x140 is restored on off.
 * This is the only pinmux register the port writes. */
#define PIN_FM_32K  REG32(0x8c00017cu)
#define PIN_FM_32K_ON 0x111u

static struct mutex i2c_mutex;
static bool powered;
static uint32_t pin_saved;

static void i2c_hw_init(void)
{
    REG32(0x8b0000a0) = 0x80000000u;     /* I2C1 APB clock SET */
    REG32(0x8b000060) = 1u << 25;        /* I2C1 reset pulse */
    udelay(10);
    REG32(0x8b000064) = 1u << 25;
    I2C_RST = 1;
    I2C_CTL &= ~0x38u;
    I2C_CLKD0 = 26000000 / (4 * 100000) - 1;   /* 100 kHz: 64 */
    I2C_CLKD1 = 0;
    REG32(0x8b0001e0) |= 2;              /* APB_PIN_CTL0: I2C1 pad select */
    I2C_CTL |= 0x18;
    I2C_CMD |= I2C_CMD_INT_ACK;
}

/* Issue one command, wait for it, acknowledge the interrupt. Returns the
 * data byte (reads), or -1 on timeout / missing ACK. */
static int i2c_cmd(uint32_t cmd, bool check_ack)
{
    unsigned n = I2C_BUDGET;
    I2C_CMD = cmd;
    while (!(I2C_CTL & 1))
        if (!--n)
            return -1;
    n = I2C_BUDGET;
    uint32_t value;
    while ((value = I2C_CMD) & I2C_CMD_BUSY)
        if (!--n)
            return -1;
    I2C_CMD = value | I2C_CMD_INT_ACK;
    if (check_ack && (I2C_CMD & I2C_CMD_RXNACK))
        return -1;
    return (value >> 8) & 0xff;
}

static void i2c_abort(void)
{
    i2c_cmd(I2C_CMD_STOP, false);
}

void fmradio_i2c_init(void)
{
    mutex_init(&i2c_mutex);
}

/* address is the 8-bit write address (0xc8 for the RTC6211). */
int fmradio_i2c_write(unsigned char address, const unsigned char *buf, int count)
{
    int ret = 0;
    mutex_lock(&i2c_mutex);
    if (i2c_cmd((address & 0xfe) << 8 | I2C_CMD_START | I2C_CMD_WRITE, true) < 0)
        ret = -1;
    for (int i = 0; ret == 0 && i < count; i++)
    {
        uint32_t cmd = buf[i] << 8 | I2C_CMD_WRITE;
        if (i == count - 1)
            cmd |= I2C_CMD_STOP;
        if (i2c_cmd(cmd, true) < 0)
            ret = -1;
    }
    if (ret)
        i2c_abort();
    mutex_unlock(&i2c_mutex);
    return ret;
}

int fmradio_i2c_read(unsigned char address, unsigned char *buf, int count)
{
    int ret = 0;
    mutex_lock(&i2c_mutex);
    if (i2c_cmd((address | 1) << 8 | I2C_CMD_START | I2C_CMD_WRITE, true) < 0)
        ret = -1;
    for (int i = 0; ret == 0 && i < count; i++)
    {
        uint32_t cmd = i == count - 1 ?
            I2C_CMD_READ | I2C_CMD_STOP | I2C_CMD_NACK : I2C_CMD_READ;
        int value = i2c_cmd(cmd, false);
        if (value < 0)
            ret = -1;
        else
            buf[i] = value;
    }
    if (ret)
        i2c_abort();
    mutex_unlock(&i2c_mutex);
    return ret;
}

/* Stock order: reset pulse, 32K on, settle. The probe then re-pulses
 * reset with a 100 ms wake; here the clock is started first and a single
 * long pulse covers both. */
bool tuner_power(bool status)
{
    if (status == powered)
        return powered;
    if (status)
    {
        sc6530_gpio_clock_on();
        i2c_hw_init();
        pin_saved = PIN_FM_32K;
        PIN_FM_32K = PIN_FM_32K_ON;
        int old = disable_irq_save();
        GPIO1_DMSK |= FM_RST_BIT;
        GPIO1_DATA &= ~FM_RST_BIT;
        GPIO1_DIR |= FM_RST_BIT;
        restore_irq(old);
        sleep(HZ / 100 + 1);                   /* >= 10 ms low */
        old = disable_irq_save();
        GPIO1_DATA |= FM_RST_BIT;
        restore_irq(old);
        sleep(HZ / 10 + 1);                    /* >= 100 ms wake */
    }
    else
    {
        int old = disable_irq_save();
        GPIO1_DATA &= ~FM_RST_BIT;
        restore_irq(old);
        PIN_FM_32K = pin_saved;
    }
    powered = status;
    return !status;
}

bool tuner_powered(void)
{
    return powered;
}
