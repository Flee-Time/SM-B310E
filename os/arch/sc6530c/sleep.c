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
 * On this board the 32 kHz clock is made from the oscillator (no 32 kHz crystal),
 * so the oscillator stays up by design. sleep_idle() stops the PLL and takes the
 * system sleep in dark idle: timer 2 runs on through it, so the tickless timer
 * still ends it and no time is lost.
 * apps/sleeptest.c tries each level on the phone and logs what happened. See
 * os/README.md, "Deep sleep".
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/arch.h"
#include "os/kernel.h"
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

uint32_t sleep_idle_count;              /* system sleeps the idle loop took */

/* Clock gates closed around the idle loop's system sleep (power audit B0,
 * 2026-11-18): stock's deep-sleep set (NOR 0x374b4: APB 0xba01efff, AHB
 * 0x1fc7f) minus what must stay awake here: the timers' bus (bit 16: timer 2
 * is our tick and the sleep's end), the keypad (6), GPIO (7: the torch's pin)
 * and the digital EIC (25). The 32 kHz domain (19, 22, 26) and the ADI (24)
 * are not in stock's set. AHB bit 8 is the PSRAM controller's clock: closing
 * it froze the phone (2026-11-09); it is not in the set and must never be.
 * sleep_gates (dev, poked from the shell): bit 0 the APB set, bit 1 the AHB
 * set; 0 (the default) closes nothing. */
#define APB_EB_SET  0x8b0000a0u
#define APB_EB_CLR  0x8b0000a4u
#define APB_EB      0x8b0000a8u
#define AHB_EB_SET  0x20500060u
#define AHB_EB_CLR  0x20500070u
#define AHB_EB      0x20500080u
#define AHB_PSRAM   (1u << 8)
#define IDLE_APB_GATES (0xba01efffu & ~(1u << 16 | 1u << 6 | 1u << 7 | 1u << 25))
#define IDLE_AHB_GATES (0x1fc7fu & ~AHB_PSRAM)
_Static_assert(!(IDLE_AHB_GATES & AHB_PSRAM), "AHB bit 8 is the PSRAM clock");
_Static_assert(!(IDLE_APB_GATES & (1u << 16)), "timer 2 needs the timers' bus");
volatile uint32_t sleep_gates;
/* The audio DMA runs (pcm.c): its interrupt comes every 3.3 ms and the VBC
 * holds no more, so the PLL is not stopped (a restart is 9.8 ms with
 * interrupts off) and the system does not sleep. Only reached in
 * power_burst_dev mode: otherwise the codec holds the clock up. */
volatile bool sleep_keep_pll;
uint32_t sleep_gated_apb, sleep_gated_ahb;     /* what the last one closed */
static bool chip_ready, idle_on = true;

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
    /* VSIM1 (bit 2 of the LDO pair): neither forced on nor off at boot (phone:
     * 1184 0x0012, 11a4 0x0001). Off, as sim.c's power-down leaves it; sim.c
     * switches it on for a slot-1 session (power audit A5, 2026-11-18). */
    adi_write(0x820011a4, adi_read(0x820011a4) & ~4u);
    adi_write(0x82001184, adi_read(0x82001184) | 4u);
    chip_ready = true;
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

void sleep_idle_enable(bool on)
{
    uint32_t s = arch_irq_save();

    idle_on = on;
    if (!on)
        clock_pll_need();
    arch_irq_restore(s);
}

/* The idle loop's wait, interrupts off. Only at 26 MHz with no hold: anything
 * using a bus or a clock (SD, USB, the codec, the DSP, the SIM) holds the clock
 * up, and the panel must be asleep for 26 MHz (or cpufreq_lit_low on, where
 * each LCD transfer holds the clock instead). Timer 2 runs through this
 * sleep (phone, 2026-11-08), so it cannot sleep past the kernel's next timeout. */
void sleep_idle(void)
{
    if (idle_on && chip_ready && clock_level() == 0 && !cpufreq_holds() && sleep_keep_pll) {
        /* no system sleep either: tried as "half-deep" (power_burst_dev 2,
         * 2026-11-18), it stalled the audio DMA in every sleep: 133 s of audio
         * played in 156 s, heard as broken sound in the dark */
        arch_wait_for_irq();
    } else if (idle_on && chip_ready && clock_level() == 0 && !cpufreq_holds()) {
        uint32_t apb = 0, ahb = 0, g = sleep_gates;

        sleep_idle_count++;
        clock_pll_stop();               /* until something needs it */
        if (g & 1) {
            apb = REG32(APB_EB) & IDLE_APB_GATES;
            REG32(APB_EB_CLR) = apb;
        }
        if (g & 2) {
            ahb = REG32(AHB_EB) & IDLE_AHB_GATES;
            REG32(AHB_EB_CLR) = ahb & ~AHB_PSRAM;
        }
        sleep_stop(0);
        if (ahb)
            REG32(AHB_EB_SET) = ahb;
        if (apb)
            REG32(APB_EB_SET) = apb;
        sleep_gated_apb = apb;
        sleep_gated_ahb = ahb;
    } else {
        arch_wait_for_irq();
    }
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
