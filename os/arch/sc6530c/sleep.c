/* B310E OS — chip sleep: the pieces a deep sleep is made of.
 *
 * What the stock firmware does, read from its code (NOR addresses in the
 * comments): the core stops by a bit in 0x205000d0, and with 0x18 in
 * 0x205000d4 the system may stop with it, which closes the PLL and the
 * crystal when nothing keeps them. The 26 MHz timers stop then; timer 0
 * (32768 Hz, 0x81000000, IRQ 4) and the 1 ms counter (0x81003000) run on.
 * The stop runs from IRAM with interrupts off and no PSRAM access, as
 * stock's does (copied to 0x40000000, NOR 0x109478).
 *
 * Not used by the idle loop yet: apps/sleeptest.c tries each level on the
 * phone and logs what happened. See os/README.md, "Deep sleep".
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/arch.h"
#include "os/lib.h"

#define STOP_IRAM   0x40003000u         /* IRAM bank 0, always on; the loader's
                                         * .bss, free once we run */
#define AHB_SLP1    0x205000d4u
#define COUNTER     0x81003004u
#define COUNTER2    0x8100300cu

#define T0_LOAD     0x81000000u
#define T0_COUNT    0x81000004u
#define T0_CTL      0x81000008u
#define T0_INT      0x8100000cu

static void ana_bits(uint32_t addr, uint32_t clear, uint32_t set)
{
    adi_write(addr, (adi_read(addr) & ~clear) | set);
    udelay(3000);                       /* stock: 3 ms after each */
}

uint32_t sleep_analog_id(void)
{
    return adi_read(0x82001000) << 16 | adi_read(0x82001004);
}

/* This board has no 32 kHz crystal: on this analog die the slow clock is
 * made from the oscillator, which stock forces on, its power-down in sleep
 * off, at boot (NOR 0x1a4f4 -> 0x1a3c4(1); the steps 0x6a14c, 0x6a2a4,
 * 0x6a35a, in their order). The boot loader and this OS never ran that. */
static void slow_clock_setup(void)
{
    if (sleep_analog_id() != SLEEP_DCXO_ID)
        return;
    ana_bits(0x82001180, 0x10, 0);      /* ABB: on */
    ana_bits(0x820011a0, 0, 0x10);
    ana_bits(0x82001180, 0x8000, 0);    /* the oscillator: on */
    ana_bits(0x820011a0, 0, 0x8000);
    ana_bits(0x820012a0, 0x10, 0);      /* the slow clock from it */
    ana_bits(0x820012a0, 0, 0x20);
    ana_bits(0x820012a4, 0x40, 0);
    ana_bits(0x820012a4, 0, 0x80);
    ana_bits(0x820012a0, 4, 0);
    ana_bits(0x820012a0, 0, 8);
    ana_bits(0x82001014, 0x4000, 0);
    ana_bits(0x82001014, 0, 0x8000);
    adi_write(0x82001288, adi_read(0x82001288) | 0x400);
    ana_bits(0x82001288, 0x200, 0);
    ana_bits(0x820012a4, 0x4000, 0);
    ana_bits(0x8200116c, 0x400, 0);
}

/* The set-up stock does once. At reset the rails' sleep register holds 0xfb79,
 * which would cut the flash, the oscillator, the SD card and RF in a sleep;
 * stock's table (NOR 0xca38c, applied at 0x3554c) leaves 0x1340. Then table
 * NOR 0xca808 and SLEEP_Init 0x373ee. Thread context: it waits. */
void sleep_chip_init(void)
{
    static bool done;
    uint32_t s;

    if (done)
        return;
    done = true;
    slow_clock_setup();
    s = arch_irq_save();
    adi_write(0x82001160, (adi_read(0x82001160) & ~0xe8bbu) | 0x1340u);
    adi_write(0x82001164, (adi_read(0x82001164) & ~8u) | 0x3f0u);
    adi_write(0x82001168, adi_read(0x82001168) & ~0x1000u);
    REG32(0x205000d0) &= ~0x21u;
    REG32(AHB_SLP1) = (REG32(AHB_SLP1) | 4u) & ~0x40u;
    REG32(0x8b000100) &= ~6u;
    REG32(0x8b000000) |= 1u << 10;
    REG32(0x8b000020) |= 1u;
    adi_write(0x82001168, adi_read(0x82001168) | 0x8000u);
    REG32(0x8b000220) |= 0x10u;
    REG32(0x8b000000) = (REG32(0x8b000000) & ~(0x3ffu << 16)) | 32u << 16;
    adi_write(0x82001010, 32);
    adi_write(0x820011a0, adi_read(0x820011a0) & ~2u);  /* the core's LDO off: */
    adi_write(0x82001180, adi_read(0x82001180) | 2u);   /* it runs on the DC-DC */
    adi_write(0x82001160, adi_read(0x82001160) & ~1u);
    adi_write(0x820011e8, (adi_read(0x820011e8) & ~8u) | 4u);
    arch_irq_restore(s);
}

/* level 0: the system may stop; 1: the core only. Interrupts off; any line
 * enabled at the INTC wakes it. */
void sleep_stop(int level)
{
    extern char sleep_stop_code[], sleep_stop_end[];
    static bool copied;
    void (*stop)(int) = (void (*)(int))STOP_IRAM;

    if (!copied) {
        memcpy((void *)STOP_IRAM, sleep_stop_code, (size_t)(sleep_stop_end - sleep_stop_code));
        dcache_clean_range((void *)STOP_IRAM, (size_t)(sleep_stop_end - sleep_stop_code));
        icache_invalidate_all();
        copied = true;
    }
    stop(level);
    REG32(AHB_SLP1) &= ~0x18u;
}

/* The 1 ms counter; it can glitch while it counts: two reads that agree. */
uint32_t sleep_counter_ms(void)
{
    uint32_t a = REG32(COUNTER);

    for (int i = 0; i < 100; i++) {
        uint32_t b = REG32(COUNTER);

        if (a == b)
            break;
        a = b;
    }
    return a;
}

/* Timer 0, one shot. The timers' clock gates are on (timer_init). */
void sleep_timer_start(uint32_t ms)
{
    for (int i = 0; i < 100000 && (REG32(T0_INT) & 0x10u); i++)
        ;
    REG32(T0_CTL) = 0;
    REG32(T0_INT) = 9;                  /* clear, interrupt on */
    REG32(T0_LOAD) = ms * 32768u / 1000u;
    REG32(T0_CTL) = 0x80;
}

/* Stops it. Returns its interrupt register before that (the logs' T0 word). */
uint32_t sleep_timer_stop(void)
{
    uint32_t v = REG32(T0_INT);

    REG32(T0_CTL) = 0;
    REG32(T0_INT) = 8;                  /* clear, interrupt off */
    return v;
}
