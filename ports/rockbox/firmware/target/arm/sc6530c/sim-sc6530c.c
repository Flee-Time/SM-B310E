/* SC6530C SIM card reader: power a slot, read the ATR, and read the
 * ICCID and IMSI with GSM (class A0) T=0 commands. No modem involved.
 * Copyright (C) 2026 B310E-OS project
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Controller, clocks, supply and activation come from the stock
 * e52q7a sim_drv.c (NOR 0x36400..0x37400): see docs/sim.md. The T=0
 * procedure-byte handling follows ISO/IEC 7816-3. Polled, thread context.
 */
#include "config.h"
#include "system.h"
#include "kernel.h"
#include <string.h>
#include "sim-target.h"

#define REG32(a) (*(volatile uint32_t *)(a))
#define SIM_BASE 0x85000000u
#define SIM_TX   REG32(SIM_BASE + 0x00)
#define SIM_RX   REG32(SIM_BASE + 0x04)
#define SIM_STS0 REG32(SIM_BASE + 0x08)
#define SIM_STS1 REG32(SIM_BASE + 0x0c)
#define SIM_IE   REG32(SIM_BASE + 0x10)
#define SIM_ICLR REG32(SIM_BASE + 0x14)
#define SIM_CTL0 REG32(SIM_BASE + 0x18)
#define SIM_CTL1 REG32(SIM_BASE + 0x1c)
#define SIM_RXCD REG32(SIM_BASE + 0x20)
#define SIM_SHE  REG32(SIM_BASE + 0x24)
#define SIM_TGC  REG32(SIM_BASE + 0x28)
#define SIM_WDT  REG32(SIM_BASE + 0x2c)
#define SIM_IMSK REG32(SIM_BASE + 0x30)
#define SIM_TXCD REG32(SIM_BASE + 0x34)
#define SIM_WDT1 REG32(SIM_BASE + 0x38)
#define SIM_CTL2 REG32(SIM_BASE + 0x3c)
#define SIM_STS2 REG32(SIM_BASE + 0x40)

#define CTL0_CONV      0x0003      /* convention / logic level */
#define CTL0_FIFO_RST  0x0030      /* RX (b4) and TX (b5) FIFO reset */
#define CTL0_TXFIFO    0x0020
#define CTL0_RST       0x0040      /* card RST pin */
#define CTL0_POWER     0x0080
#define CTL0_RX_EN     0x0200
#define CTL0_TX_EN     0x0400
#define CTL0_IO_LOW    0x8000
#define CTL1_CLK_EN    0x0800
#define RX_COUNT()     (SIM_STS2 & 0x7f)
#define TX_COUNT()     ((SIM_STS2 >> 8) & 0x7f)
#define ETU_CLOCKS     0xba0       /* 8 x 372: 26 MHz / 8 = 3.25 MHz, F=372 D=1 */

/* ANA (through the ADI bridge) */
#define ADI_RD_CMD   0x82000018u
#define ADI_RD_DATA  0x8200001cu
#define ADI_FIFO_STS 0x82000020u
#define LDO_PD_SET   0x82001184u   /* VSIM power-down: set = off */
#define LDO_PD_CLR   0x820011a4u   /* set = on */
#define LDO_VSIM_V   0x820011c8u   /* VSIM0 bits 7:4, VSIM1 bits 11:8 */

static uint32_t adi_read(uint32_t addr)
{
    uint32_t a = 0, n = 1000000;
    REG32(ADI_RD_CMD) = addr & 0xfff;
    while ((a = REG32(ADI_RD_DATA)) >> 31)
        if (!--n)
            break;
    return a & 0xffff;
}

static void adi_write(uint32_t addr, uint32_t value)
{
    uint32_t n = 1000000;
    while (REG32(ADI_FIFO_STS) & (1u << 9))
        if (!--n)
            return;
    REG32(addr) = value;
    n = 1000000;
    while (!(REG32(ADI_FIFO_STS) & (1u << 8)))
        if (!--n)
            return;
}

static void vsim(int slot, bool on, bool three_volt)
{
    uint32_t bit = slot ? 0x4 : 0x2;
    uint32_t shift = slot ? 8 : 4;
    if (on)
    {
        /* Stock 0x355f6: level 0 = 1.8 V (0xa), level 2 = 3 V (0x6). */
        uint32_t v = adi_read(LDO_VSIM_V) & ~(0xfu << shift);
        adi_write(LDO_VSIM_V, v | (three_volt ? 0x6u : 0xau) << shift);
        adi_write(LDO_PD_SET, adi_read(LDO_PD_SET) & ~bit);
        adi_write(LDO_PD_CLR, adi_read(LDO_PD_CLR) | bit);
    }
    else
    {
        adi_write(LDO_PD_CLR, adi_read(LDO_PD_CLR) & ~bit);
        adi_write(LDO_PD_SET, adi_read(LDO_PD_SET) | bit);
        uint32_t v = adi_read(LDO_VSIM_V) & ~(0xfu << shift);
        adi_write(LDO_VSIM_V, v | 0xau << shift);
    }
}

static void spin(unsigned loops)
{
    volatile unsigned n = loops;
    while (n--)
        ;
}

static void sim_deactivate(int slot)
{
    /* Stock 0x36faa -> 0x36f26. */
    SIM_IE = 0;
    SIM_CTL0 &= ~CTL0_RST;
    udelay(50);
    SIM_CTL1 &= ~CTL1_CLK_EN;
    udelay(50);
    SIM_CTL0 |= CTL0_IO_LOW;
    udelay(50);
    vsim(slot, false, false);
    REG32(0x8b0000a4) = 0x02000004;      /* SIM0/SIM1 APB clocks off */
}

/* Read one byte, or -1 after timeout_ticks. */
static int sim_getc(long timeout_ticks)
{
    long end = current_tick + timeout_ticks;
    while (!RX_COUNT())
    {
        if (TIME_AFTER(current_tick, end))
            return -1;
        yield();
    }
    return SIM_RX & 0xff;
}

static bool sim_tx_drain(void)
{
    long end = current_tick + HZ / 2;
    while (TX_COUNT())
        if (TIME_AFTER(current_tick, end))
            return false;
    udelay(3000);                         /* last character + guard time */
    return true;
}

static void sim_send(const uint8_t *data, int len)
{
    SIM_CTL0 &= ~CTL0_TX_EN;
    SIM_CTL0 |= CTL0_TXFIFO;
    spin(10);
    SIM_CTL0 &= ~CTL0_TXFIFO;
    SIM_CTL0 |= CTL0_TX_EN;
    for (int i = 0; i < len; i++)
    {
        while (TX_COUNT() >= 32)
            ;
        SIM_TX = data[i];
    }
    sim_tx_drain();
    SIM_CTL0 &= ~CTL0_TX_EN;               /* turn the line around */
}

/* T=0 case 2/3 command: hdr[5] = CLA INS P1 P2 P3. If out, P3 bytes are
 * sent after the ACK; otherwise up to P3 bytes are received into in.
 * Returns SW1SW2, or 0 on timeout. */
static unsigned sim_apdu(const uint8_t hdr[5], const uint8_t *out,
                         uint8_t *in, int *got)
{
    int len = hdr[4], done = 0;
    if (got)
        *got = 0;
    /* Stock 0x36e46: FIFO reset, RX mark 1, RX+TX on, retransmit 3/3. */
    SIM_CTL0 |= CTL0_FIFO_RST;
    spin(10);
    SIM_CTL0 &= ~CTL0_FIFO_RST;
    SIM_CTL2 = (SIM_CTL2 & 0xff00) | 1;
    SIM_CTL0 |= CTL0_RX_EN | CTL0_TX_EN;
    SIM_SHE = 0x33;
    SIM_WDT = 0x803;
    sim_send(hdr, 5);
    for (int nulls = 0; nulls < 100;)
    {
        int pb = sim_getc(HZ + HZ / 10);  /* WWT ~1.1 s at WI = 10 */
        if (pb < 0)
            return 0;
        if (pb == 0x60)
        {
            nulls++;
            continue;
        }
        if ((pb & 0xf0) == 0x60 || (pb & 0xf0) == 0x90)
        {
            int sw2 = sim_getc(HZ);
            if (got)
                *got = done;
            return sw2 < 0 ? 0 : (unsigned)pb << 8 | (unsigned)sw2;
        }
        bool all = pb == hdr[1];
        bool one = pb == (hdr[1] ^ 0xff);
        if (!all && !one)
            return 0;                     /* not a procedure byte */
        int n = all ? len - done : 1;
        if (out)
        {
            sim_send(out + done, n);
            done += n;
        }
        else
            for (int i = 0; i < n && done < len; i++)
            {
                int c = sim_getc(HZ);
                if (c < 0)
                    return 0;
                in[done++] = c;
            }
    }
    return 0;
}

static unsigned select_file(uint16_t fid)
{
    static const uint8_t hdr[5] = { 0xa0, 0xa4, 0x00, 0x00, 0x02 };
    uint8_t id[2] = { fid >> 8, fid & 0xff };
    return sim_apdu(hdr, id, NULL, NULL);
}

static unsigned read_binary(uint8_t *buf, int len, int *got)
{
    uint8_t hdr[5] = { 0xa0, 0xb0, 0x00, 0x00, (uint8_t)len };
    return sim_apdu(hdr, NULL, buf, got);
}

/* ICCID (EF 2FE2): 10 bytes, BCD with swapped nibbles, 0xF padding. */
static void bcd_swapped(const uint8_t *in, int len, char *out, int skip)
{
    int k = 0;
    for (int i = 0; i < len; i++)
        for (int half = 0; half < 2; half++)
        {
            int d = half ? in[i] >> 4 : in[i] & 0xf;
            if (skip)
            {
                skip--;
                continue;
            }
            if (d > 9)
                continue;
            out[k++] = '0' + d;
        }
    out[k] = 0;
}

static bool activate(int slot, bool three_volt, struct sim_info *info)
{
    /* Clocks (stock clock devices SIM0/SIM1, ops 0x673c0..): APB enable
     * and reset bit 20. Slot select and clock-stop level per slot. */
    REG32(0x8b0000a0) = 0x02000004;
    REG32(0x8b000060) = 0x100000;
    spin(1000);
    REG32(0x8b000064) = 0x100000;
    REG32(0x8b0001e0) = (REG32(0x8b0001e0) & ~0xcu) | (uint32_t)slot << 2;
    REG32(0x8b000220) &= ~(2u << slot);

    vsim(slot, true, three_volt);
    sleep(HZ / 50 + 1);                   /* LDO settle (stock >= 1 ms) */

    /* Stock 0x36ea0 InitAfterLDOValid + 0x36b46 clock setup. */
    SIM_CTL2 = 1;
    SIM_CTL1 = (SIM_CTL1 & ~0x1f00u) | 0x200;   /* 26 MHz / 8 */
    SIM_RXCD = ETU_CLOCKS;
    SIM_TXCD = ETU_CLOCKS;
    SIM_STS0 = 0;
    SIM_STS1 = 0;
    SIM_IE = 0;
    SIM_ICLR = 0x1ff;
    SIM_CTL0 = 0x2003;
    SIM_SHE = 0;
    SIM_TGC = 0x505;
    SIM_WDT = 0x802;
    SIM_WDT1 = 0x100;
    SIM_IMSK = 0;

    /* Stock 0x36d90 ClearBeforeReset. */
    while (RX_COUNT())
        (void)SIM_RX;
    SIM_ICLR = 0xffff;
    SIM_SHE = 0x0a;
    SIM_CTL2 = (SIM_CTL2 & 0xff00) | 1;

    /* Cold reset: clock on, RST low, stock 0x36dd0 StartToReset, then RST
     * high after >= 400 clocks (stock HAL waits 20 ms). */
    SIM_CTL1 |= CTL1_CLK_EN;
    sleep(HZ / 50 + 1);
    SIM_CTL0 &= ~CTL0_RST;
    SIM_CTL0 |= CTL0_RX_EN | CTL0_TX_EN;
    SIM_CTL0 |= CTL0_POWER;
    SIM_CTL1 |= CTL1_CLK_EN;
    udelay(30);
    SIM_CTL0 &= ~CTL0_IO_LOW;
    sleep(HZ / 50 + 1);
    SIM_CTL0 |= CTL0_RST;

    /* ATR: first byte within 300 ms, end after 50 ms of silence. */
    int c = sim_getc(HZ * 3 / 10);
    info->atr_len = 0;
    while (c >= 0 && info->atr_len < (int)sizeof(info->atr))
    {
        info->atr[info->atr_len++] = c;
        c = sim_getc(HZ / 20);
    }
    if (info->atr_len && info->atr[0] == 0x03)
    {
        /* Inverse convention (stock 0x36b18 toggles CTL0 bits 0-1). */
        SIM_CTL0 ^= CTL0_CONV;
        info->atr[0] = 0x3f;
    }
    return info->atr_len > 0 && (info->atr[0] == 0x3b || info->atr[0] == 0x3f);
}

void sim_read(int slot, struct sim_info *info)
{
    memset(info, 0, sizeof(*info));
    info->slot = slot;
    info->three_volt = false;
    if (!activate(slot, false, info))
    {
        sim_deactivate(slot);
        sleep(HZ / 10);
        info->three_volt = true;
        if (!activate(slot, true, info))
        {
            sim_deactivate(slot);
            info->status = info->atr_len ? SIM_BAD_ATR : SIM_NO_CARD;
            return;
        }
    }
    info->status = SIM_ATR_OK;

    uint8_t buf[16];
    int got;
    /* ICCID: MF 3F00 -> EF_ICCID 2FE2, always readable. */
    info->sw_select = select_file(0x3f00);
    if ((info->sw_select >> 8) == 0x9f || info->sw_select == 0x9000)
    {
        unsigned sw = select_file(0x2fe2);
        if ((sw >> 8) == 0x9f || sw == 0x9000)
        {
            info->sw_iccid = read_binary(buf, 10, &got);
            if (info->sw_iccid == 0x9000 && got == 10)
            {
                bcd_swapped(buf, 10, info->iccid, 0);
                info->status = SIM_ICCID_OK;
            }
        }
    }
    /* IMSI: DF_GSM 7F20 -> EF_IMSI 6F07; needs CHV1 (PIN) if enabled.
     * Byte 0 is the length, byte 1's low nibble is the parity digit. */
    if ((select_file(0x7f20) >> 8) == 0x9f && (select_file(0x6f07) >> 8) == 0x9f)
    {
        info->sw_imsi = read_binary(buf, 9, &got);
        if (info->sw_imsi == 0x9000 && got == 9)
        {
            bcd_swapped(buf + 1, 8, info->imsi, 1);
            info->status = SIM_IMSI_OK;
        }
        else if (info->sw_imsi == 0x9804)
            info->pin_required = true;
    }
    sim_deactivate(slot);
}
