/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2009 by Bob Cousins
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
#include "cpu.h"
#include "system.h"
#include "button.h"
#include "kernel.h"
#include "button-target.h"

/*
 * B310E-OS Rockbox port — b310e/target/arm/sc6530c/button-sc6530c.c
 * (GPLv2, Rockbox-derived; modeled on Rockbox's mini2440/button-mini2440.c
 * and the B310E-OS drivers/keypad.c — fpdoom syscode.c, Unlicense).
 *
 * The SC6530 keypad controller is EDGE-TRIGGERED: int_raw bits 0-3 report
 * PRESS edges for matrix columns 0-3, bits 4-7 report RELEASE edges (a
 * column's key-up appears at bit i+4, decoding the same key_status row
 * byte via the (i & 3) wrap). While a key is held the controller reports
 * nothing, so this driver ACCUMULATES a "currently held" mask: press
 * edges set the Rockbox button bit, release edges clear it. The EIC
 * power button (END) is LEVEL-readable and tracked separately.
 *
 * Rockbox's button framework (firmware/drivers/button.c) polls
 * button_read_device() and synthesizes BUTTON_REL/BUTTON_REPEAT from the
 * returned mask — we only return the physical held state.
 *
 * SAFETY: no 0x8c pinmux writes; ADI mailbox accesses are bounded.
 */

#define REG32(a) (*(volatile uint32_t *)(a))

/* ---- matrix controller (fpdoom syscode.h:27-32, Unlicense) ------------- */
#define KEYPAD_BASE_ADDR   0x87000000u
#define KEYPAD_APB_PWR_REG 0x8b0000a0u
#define KEYPAD_APB_PWR_BITS 0x80040u
#define KEYPAD_INT_ALL     0xfffu

typedef volatile struct {
    uint32_t ctrl, int_en, int_raw, int_mask;
    uint32_t int_clr, dummy_14, polarity, debounce;
    uint32_t long_key, sleep_cnt, clk_divide, key_status;
    uint32_t sleep_stat, dbg_stat1, dbg_stat2;
} keypad_base_t;

/* ---- EIC power button (END) — fpdoom keypad_read_pb -------------------- */
#define KEYPAD_EIC_DBNC_DATA  0x82001900u
#define KEYPAD_EIC_DBNC_DMSK  0x82001904u
#define KEYPAD_EIC_PB_CH      3u

/* ---- bounded ADI mailbox (B310E-OS drivers/keypad.c, fpdoom) ----------- */
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

/* ---- B310E keymap (drivers/keypad.c s_keytrn, dump @0xc6e70) ----------- */
static const uint8_t s_keytrn[64] = {
    0x0d, 0x01, 0xff, 0xff, 0x06, 0xff, 0xff, 0xff,
    0x31, 0x32, 0x33, 0x08, 0x05, 0xff, 0xff, 0xff,
    0x34, 0x35, 0x36, 0x09, 0xff, 0xff, 0xff, 0xff,
    0x37, 0x38, 0x39, 0xff, 0x04, 0xff, 0xff, 0xff,
    0x2a, 0x30, 0x23, 0xff, 0x07, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
};

/* Map the translated key code (fpdoom KEYPAD_ENUM, see B310E-OS keypad.h)
 * to a Rockbox button bit. */
static int button_code(uint8_t code)
{
    switch (code) {
    case 0x01: return BUTTON_PLAY;    /* DIAL     */
    case 0x04: return BUTTON_UP;
    case 0x05: return BUTTON_DOWN;
    case 0x06: return BUTTON_LEFT;
    case 0x07: return BUTTON_RIGHT;
    case 0x08: return BUTTON_MENU;    /* LSOFT    */
    case 0x09: return BUTTON_BACK;    /* RSOFT    */
    case 0x0d: return BUTTON_SELECT;  /* CENTER   */
    case 0x23: return BUTTON_HASH;
    case 0x2a: return BUTTON_HOME;    /* STAR     */
    case 0x30: return BUTTON_0;
    case 0x31: return BUTTON_1;
    case 0x32: return BUTTON_VOL_UP;  /* 2        */
    case 0x33: return BUTTON_3;
    case 0x34: return BUTTON_PREV;    /* 4        */
    case 0x35: return BUTTON_5;
    case 0x36: return BUTTON_NEXT;    /* 6        */
    case 0x37: return BUTTON_7;
    case 0x38: return BUTTON_VOL_DOWN;/* 8        */
    case 0x39: return BUTTON_9;
    default:   return 0;
    }
}

/* Accumulated held-button mask. */
static int held_mask;

void button_init_device(void)
{
    keypad_base_t *kpd = (keypad_base_t *)KEYPAD_BASE_ADDR;
    unsigned row = 0, col = 0, c, r, ctrl;

    /* Enable exactly the matrix rows/cols used by the real keymap
     * (fpdoom keypad_init, syscode.c:877-880). */
    for (c = 0; c < 8; c++)
        for (r = 0; r < 8; r++)
            if (s_keytrn[r * 8 + c] != 0xff)
                col |= 1u << c, row |= 1u << r;
    row &= 0xfcu;                       /* SC6530/31 usable mask */
    col &= 0xfcu;

    REG32(KEYPAD_APB_PWR_REG) |= KEYPAD_APB_PWR_BITS;

    kpd->int_clr = KEYPAD_INT_ALL;
    kpd->clk_divide = 1;
    kpd->debounce = 16;
    kpd->int_en = KEYPAD_INT_ALL;
    kpd->polarity = 0xffffu;

    ctrl = kpd->ctrl;
    ctrl |= 1u;                         /* enable */
    ctrl &= ~2u;                        /* no sleep */
    ctrl |= 4u;                         /* long-press detect on */
    ctrl &= ~(0xfcu << 16 | 0xfcu << 8);
    ctrl |= row << 16 | col << 8;
    kpd->ctrl = ctrl;

    /* END (EIC power button): unmask its debounce channel so the level
     * read sees the real state (fpdoom eic_enable, syscode.c:851-868). */
    adi_write(0x820010e4, 0x20);
    adi_write(0x820010e0, 0x80);
    adi_write(KEYPAD_EIC_DBNC_DMSK,
              adi_read(KEYPAD_EIC_DBNC_DMSK) | (1u << KEYPAD_EIC_PB_CH));

    held_mask = 0;
}

int button_read_device(int *data)
{
    keypad_base_t *kpd = (keypad_base_t *)KEYPAD_BASE_ADDR;
    uint32_t event, status;
    int i;

    *data = 0;

    /* EIC power/END level (1 = held). */
    if ((adi_read(KEYPAD_EIC_DBNC_DATA) >> KEYPAD_EIC_PB_CH) & 1u)
        held_mask |= BUTTON_POWER;
    else
        held_mask &= ~BUTTON_POWER;

    /* Matrix edge frame. */
    event = kpd->int_raw & 0xffu;
    status = kpd->key_status;
    if (event != 0)
        kpd->int_clr = KEYPAD_INT_ALL;  /* ack + re-arm */

    if (status & 8u)
        return held_mask;               /* fpdoom: status bit 3 = bad frame */

    for (i = 0; i < 8; i++) {
        uint32_t byte, k;
        uint8_t code;
        int b;

        if (!(event & (1u << i)))
            continue;
        byte = status >> ((i & 3u) * 8u);
        k = (byte & 0x70u) >> 1 | (byte & 7u);   /* fpdoom index math */
        code = s_keytrn[k];
        if (code == 0xffu)
            continue;
        b = button_code(code);
        if (b == 0)
            continue;
        if (i < 4)
            held_mask |= b;             /* press edge  */
        else
            held_mask &= ~b;            /* release edge */
    }
    return held_mask;
}
