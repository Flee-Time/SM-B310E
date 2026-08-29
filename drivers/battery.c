/*
 * B310E-OS — drivers/battery.c
 *
 * Battery fuel gauge: SC6530 internal ADC reads the VBAT_SENSE channel.
 * **VBAT = channel 5** (HW-verified 2026-08-22 via the RSOFT 16-channel
 * sweep — ch5 is the only channel that moves with the battery; the stock
 * charger code reads ch6 but that is the USB/charge-detect rail).
 * Register map + conversion flow decoded VERBATIM from adc_phy_v5.c in the
 * kern region of dump_firmware.bin (@0x020FC0: conversion @0x20ef6,
 * channel select @0x20e76, channel mode @0x20eb8, ADC enable @0x20d54).
 *
 * No external fuel-gauge IC: the docs' part list has only U100 (RT9532GQW
 * linear charger) — VBAT_SENSE goes to the SoC ADC (docs/Troubleshooting
 * net list: VBAT_SENSE / CHG_DET / CHG_EN / CHG_STATE). Battery is
 * AB463446BU, 800 mAh.
 *
 * HW LESSONS (2026-08-21 user logs + 2026-08-22 dump re-read):
 *   (1) A minimal "enable + select + poll" driver returned 0 mV and hung
 *       ~1 s — the done-wait must be TIME-budgeted (stock uses sys_timer_ms
 *       ~1 ms), not a 1M-iteration spin. Fixed with a 5 ms budget on the
 *       free-running 0x8100300c counter.
 *   (2) The conversion NEVER completes without the recovery sequence:
 *       stock mode 0 sets BOTH bits 4 AND 5 of 0x82001684 (adc_phy_v5.c
 *       @0x20eb8 — the old "bit 5 is a different mode that never
 *       completes" note was a misread of the disassembly), and the timeout
 *       recovery @0x20db6 arms the ANA analog block (0x82001040=1,2 /
 *       0x820010E0=0x20,0x10 / 0x820010E4=0x10 / 0x820010A0=8 /
 *       0x820010B0=8), the ADC data path (0x82001680 |=1, 0x820016C8=0xE0,
 *       0x820016B4/AC/B0/A8 |= 0x40) and the per-channel mux (0x820016AC
 *       low4=2 / 0x820016B4 low4=3, @0x20d6a). The stock runs the recovery
 *       ONLY on a done-bit timeout and then RETRIES once (@0x20f34-0x20f4e)
 *       — the first conversion after boot always times out; the recovery
 *       is what makes it complete.
 *   (3) The recovery's 0x82001040 / 0x820010E0 / 0x820010E4 writes are
 *       NOT the watchdog: fpdoom sys_wdg_reset touches 0x82001040 only on
 *       _chip==1 and uses 0x820010E0=4 / 0x820010E4=2 on SC6530C
 *       (syscode.c:1110-1121) — the recovery writes different values (EIC
 *       channels 4/5). They DO clobber the keypad's EIC channel 3 config
 *       (0x820010E0=0x80 / 0x820010E4=0x20), so keypad_eic_reapply() runs
 *       after the recovery to keep the END key alive.
 *
 * SAFETY: ADI mailbox access only (0x82000018/1c/20, same as led.c),
 * bounded waits — a wedged ADC degrades to a no-op. HOST_TEST compiles the
 * hardware path out (API links, returns 0).
 */

#include "battery.h"

#include "../arch/chip.h"   /* MEM4, DELAY */
#include "keypad.h"         /* keypad_eic_reapply() (undo the EIC clobber) */

/* Stock mV mapping constants — outside #ifndef HOST_TEST: the pure
 * battery_mv_from_raw() (host-testable) uses them.
 *
 * 0x00040F10 / 0x000124F8 (265488/75000 = 3.54 mV/count, full-scale 3.62V)
 * is what the stock charger code uses, but it CANNOT represent a full
 * Li-ion cell (4.2V > 3.62V) — the stock reads ch6, the USB-detect rail.
 * For the REAL battery channel (ch5) the HW-verified scale (2026-08-22,
 * multimeter 4.15V at raw 946, and the unloaded-charger float 4.2V at raw
 * 977) is ~4.39 mV/count: 4390/1000, full-scale 4.49V. */
#define BAT_CONV_NUM      4390u       /* 4.39 mV/count (HW-calibrated)     */
#define BAT_CONV_DEN      1000u

#ifndef HOST_TEST
/* ADI mailbox (fpdoom adi_read/adi_write, SC6530 chip-2 path) — identical
 * to led.c; kept local so the driver stands alone. */
#define BAT_ADI_RD_CMD    0x82000018u
#define BAT_ADI_RD_DATA   0x8200001cu
#define BAT_ADI_FIFO_STS  0x82000020u
#define BAT_ADI_FIFO_FULL  (1u << 9)
#define BAT_ADI_FIFO_EMPTY (1u << 8)
#define BAT_ADI_BUDGET     1000000u

/* ADC registers (adc_phy_v5.c): base 0x82001680. */
#define BAT_ADC_BASE      0x82001680u
#define BAT_ADC_ENABLE    (BAT_ADC_BASE + 0x00u)   /* |= 2 = ADC on (conv) */
#define BAT_ADC_CHSEL     (BAT_ADC_BASE + 0x04u)   /* low4 = ch; bits 4+5 mode */
#define BAT_ADC_START     (BAT_ADC_BASE + 0x54u)   /* |= 1 start; |=0x3FF reset */
#define BAT_ADC_STATUS    (BAT_ADC_BASE + 0x5cu)   /* bit 0 = done         */
#define BAT_ADC_RESULT    (BAT_ADC_BASE + 0x4cu)   /* 10-bit in bits 0-9   */

/* ANA analog block — the recovery's config part (@0x20db6). */
#define BAT_ANA_BASE      0x82001040u   /* ANA clock/power enable (1 then 2) */
#define BAT_ANA_E0        0x820010E0u   /* EIC channel config (clobbers the
                                           keypad's ch3 — re-apply after)  */
#define BAT_ANA_E4        0x820010E4u
#define BAT_ANA_A0        0x820010A0u   /* ADC clock divider (8)           */
#define BAT_ANA_B0        0x820010B0u   /* ADC clock divider (8)           */

/* VBAT channel — HARDWARE-VERIFIED as ch5, NOT ch6 (2026-08-22). The
 * RSOFT 16-channel sweep showed ch5 is the ONLY channel that moves with
 * the battery (977 raw / 3.46V battery-out -> 946 raw / 3.35V battery-in,
 * i.e. VBAT_SENSE sees the unloaded charger float without a battery and
 * the cell's own voltage with one); ch6 reads the USB-detect rail (~1.1V
 * with USB, ~0 without, battery-independent). The stock firmware reads
 * ch6 in its charger-management code, but that code is the USB/charge
 * detector, NOT the battery meter. */
#define BAT_CH_VBAT       5u
#define BAT_CONV_BUDGET_ITERS 1000000u /* ~5-10ms @ 208MHz (ADI budget scale;
                                          the stock waits ~1ms via sys_timer_ms) */
#define BAT_CONV_TIMEOUT  0xFFFFFFFFu /* sentinel: done bit never set      */

/* 60-sample sliding average of the VBAT raw (1 per second = a 1-minute
 * window) so the displayed voltage eases through the USB unplug/replug
 * jump (the battery rail moves between the cell's own voltage and the
 * charger float). ONLY the per-second display calls this (one push per
 * sample period); the HASH log and the RSOFT sweep stay instantaneous. */
#define BAT_AVG_SAMPLES   60u
static uint32_t s_bat_ring[BAT_AVG_SAMPLES];
static uint32_t s_bat_ring_idx;
static uint32_t s_bat_ring_n;

static uint32_t bat_adi_read(uint32_t addr)
{
    uint32_t a = 0, n = BAT_ADI_BUDGET;

    MEM4(BAT_ADI_RD_CMD) = addr & 0xfffu;
    while ((a = MEM4(BAT_ADI_RD_DATA)) >> 31)   /* wait busy clear */
        if (--n == 0) break;
    return a & 0xffffu;
}

static void bat_adi_write(uint32_t addr, uint32_t val)
{
    uint32_t n = BAT_ADI_BUDGET;

    while (MEM4(BAT_ADI_FIFO_STS) & BAT_ADI_FIFO_FULL)  /* FIFO full  */
        if (--n == 0) return;
    MEM4(addr) = val;
    n = BAT_ADI_BUDGET;
    while (!(MEM4(BAT_ADI_FIFO_STS) & BAT_ADI_FIFO_EMPTY))
        if (--n == 0) return;
}

/* Select ADC channel (stock @0x20e76 VERBATIM, two writes: clear the low
 * 4 bits of 0x82001684, then OR in the channel). */
static void bat_adc_select(uint32_t ch)
{
    uint32_t v = bat_adi_read(BAT_ADC_CHSEL);

    bat_adi_write(BAT_ADC_CHSEL, v & ~0xFu);
    bat_adi_write(BAT_ADC_CHSEL,
                  bat_adi_read(BAT_ADC_CHSEL) | (ch & 0xFu));
}

/* ADC mode 0 (stock @0x20eb8 VERBATIM): 0x82001684 |= 0x10 THEN |= 0x20.
 * The old note claiming bit 5 "selects a mode that never completes" was a
 * misread — the stock mode-0 path sets both bits (disasm @0x20ed8/0x20ee8:
 * orr 0x10, write, orr 0x20, write). */
static void bat_adc_mode0(void)
{
    bat_adi_write(BAT_ADC_CHSEL, bat_adi_read(BAT_ADC_CHSEL) | 0x10u);
    bat_adi_write(BAT_ADC_CHSEL, bat_adi_read(BAT_ADC_CHSEL) | 0x20u);
}

/* Per-channel mux (stock @0x20d6a VERBATIM): the channel-select low
 * nibbles of the ADC data-path registers. */
static void bat_adc_mux(void)
{
    bat_adi_write(BAT_ADC_BASE + 0x2cu,
                  (bat_adi_read(BAT_ADC_BASE + 0x2cu) & ~0xFu) | 2u);
    bat_adi_write(BAT_ADC_BASE + 0x34u,
                  (bat_adi_read(BAT_ADC_BASE + 0x34u) & ~0xFu) | 3u);
}

/* Timeout recovery — adc_phy_v5.c @0x20db6 VERBATIM (the routine the
 * conversion calls on a done-bit timeout @0x20f34 before retrying):
 *   ANA analog config block (@0x20db6-0x20dfc): 0x82001040=1,2;
 *   0x820010E0=0x20,0x10; 0x820010E4=0x10; 0x820010A0=8; settle; 0x820010B0=8.
 *   ADC data path (@0x20e00-0x20e6a): enable |=1, 0x820016C8=0xE0,
 *   0x820016B4/AC/B0/A8 |= 0x40.
 *   per-channel mux (@0x20d6a, via bat_adc_mux).
 * Then re-asserts the keypad EIC channel 3 (the config block rewrote
 * 0x820010E0/E4 = EIC channels 4/5). */
static void bat_adc_recovery(void)
{
    bat_adi_write(BAT_ANA_BASE, 1u);
    bat_adi_write(BAT_ANA_BASE, 2u);
    bat_adi_write(BAT_ANA_E0, 0x20u);
    bat_adi_write(BAT_ANA_E0, 0x10u);
    bat_adi_write(BAT_ANA_E4, 0x10u);
    bat_adi_write(BAT_ANA_A0, 8u);
    DELAY(10);                       /* stock settle loop (~10 iterations) */
    bat_adi_write(BAT_ANA_B0, 8u);

    bat_adi_write(BAT_ADC_ENABLE, bat_adi_read(BAT_ADC_ENABLE) | 1u);
    bat_adi_write(BAT_ADC_BASE + 0x48u, 0xE0u);
    bat_adi_write(BAT_ADC_BASE + 0x34u,
                  bat_adi_read(BAT_ADC_BASE + 0x34u) | 0x40u);
    bat_adi_write(BAT_ADC_BASE + 0x2cu,
                  bat_adi_read(BAT_ADC_BASE + 0x2cu) | 0x40u);
    bat_adi_write(BAT_ADC_BASE + 0x30u,
                  bat_adi_read(BAT_ADC_BASE + 0x30u) | 0x40u);
    bat_adi_write(BAT_ADC_BASE + 0x28u,
                  bat_adi_read(BAT_ADC_BASE + 0x28u) | 0x40u);

    bat_adc_mux();

    keypad_eic_reapply();
}

/* Done-bit wait with a pure ITERATION budget (counter-FREE — the
 * free-running 0x8100300c counter glitches intermittently on this phone,
 * the "HASH sometimes hangs" cause, and the stock's ~1ms conversion must
 * not be cut short by a broken time source). Returns 1 when the done bit
 * set, 0 on a wedged ADC (degrades to a no-op, never a hang). */
static uint32_t bat_adc_wait_done(void)
{
    uint32_t n = BAT_CONV_BUDGET_ITERS;

    while (!(bat_adi_read(BAT_ADC_STATUS) & 1u))
        if (--n == 0)
            return 0;
    return 1;
}

/* One conversion attempt (stock adc_read @0x20ef6 VERBATIM order):
 * START (0x820016D4 |= 1) -> SELECT (0x82001684) -> MODE 0 (|= 0x10 then
 * |= 0x20) -> ENABLE (0x82001680 |= 2) -> poll done (0x820016DC bit 0,
 * iteration-budgeted — the stock waits ~1ms; our old 5-iteration cap
 * (~25µs) read the result mid-integration, which is why the "successful"
 * readings were ~306 instead of ~7) -> result (0x820016CC & 0x3FF) ->
 * reset (0x820016D4 |= 0x3FF). Returns the raw value, or
 * BAT_CONV_TIMEOUT if the done bit never set (the caller runs the
 * recovery and retries — stock behavior). */
static uint32_t bat_adc_convert_ch(uint32_t ch)
{
    uint32_t raw;

    bat_adi_write(BAT_ADC_START, bat_adi_read(BAT_ADC_START) | 1u);
    bat_adc_select(ch);
    bat_adc_mode0();
    bat_adi_write(BAT_ADC_ENABLE, bat_adi_read(BAT_ADC_ENABLE) | 2u);

    if (!bat_adc_wait_done()) {
        /* wedged: reset + report (never return a garbage value) */
        bat_adi_write(BAT_ADC_START, bat_adi_read(BAT_ADC_START) | 0x3ffu);
        return BAT_CONV_TIMEOUT;
    }

    /* Result: 0x820016CC & 0x3FF (10-bit; stock @0x20f62). */
    raw = bat_adi_read(BAT_ADC_RESULT) & 0x3ffu;

    /* Reset the converter: 0x820016D4 |= 0x3FF (stock @0x20f84). */
    bat_adi_write(BAT_ADC_START, bat_adi_read(BAT_ADC_START) | 0x3ffu);
    return raw;
}
#endif /* !HOST_TEST */

void battery_init(void)
{
#ifndef HOST_TEST
    /* Nothing to do: the recovery (ANA config + data path + mux) runs on
     * demand from battery_read_mv's timeout path, exactly like the stock
     * adc_phy_v5.c (which calls the recovery only when a conversion's
     * done-bit times out, then retries). */
#endif
}

/* Stock mV mapping: (raw * 4390 + 500) / 1000 = raw * 4.39 mV/count.
 * HW-calibrated (2026-08-22): multimeter 4.15V at raw 946 (battery in),
 * unloaded charger float ~4.2V at raw 977 (battery out) — full-scale 1023
 * -> 4491mV. NOT the stock charger constants (3.54 mV/count) which only
 * fit the ch6 USB-detect rail. */
uint32_t battery_mv_from_raw(uint32_t raw)
{
    return (raw * BAT_CONV_NUM + BAT_CONV_DEN / 2u) / BAT_CONV_DEN;
}

uint32_t battery_read_mv(void)
{
#if !defined(HOST_TEST)
    uint32_t raw = battery_read_raw();

    if (raw == 0)
        return 0;            /* wedged even after the recovery+retry */
    return battery_mv_from_raw(raw);
#else
    return 0;
#endif
}

/* Raw ADC count for VBAT (channel 5) — the calibration hook. Runs the
 * stock conversion + timeout-recovery + retry. Returns the 10-bit result
 * register value (& 0x3FF), or 0 if the conversion stayed wedged. The
 * stock formula maps it as mV = raw*265488/75000 (~3.54 mV/count). The
 * exact scale is EMPIRICAL: log the raw + a battery-in measurement and
 * divide to confirm. */
uint32_t battery_read_raw(void)
{
#if !defined(HOST_TEST)
    return battery_read_raw_ch(BAT_CH_VBAT);
#else
    return 0;
#endif
}

/* Averaged VBAT raw: pushes the current sample into the 60-slot ring and
 * returns the mean (a 1-minute sliding average when called once per
 * second). Smooths the USB unplug/replug jump. Returns 0 if wedged. */
uint32_t battery_read_raw_avg(void)
{
#if !defined(HOST_TEST)
    uint32_t raw = battery_read_raw();
    uint32_t i, sum = 0;

    s_bat_ring[s_bat_ring_idx] = raw;
    s_bat_ring_idx = (s_bat_ring_idx + 1u) % BAT_AVG_SAMPLES;
    if (s_bat_ring_n < BAT_AVG_SAMPLES)
        s_bat_ring_n++;
    for (i = 0; i < s_bat_ring_n; i++)
        sum += s_bat_ring[i];
    return sum / s_bat_ring_n;
#else
    return 0;
#endif
}

/* Raw ADC count for an ARBITRARY channel 0-15 — the channel-sweep
 * diagnostic (which input does the battery actually sit on?). Runs the
 * stock conversion + timeout-recovery + retry for that channel. Returns
 * the 10-bit result register value (& 0x3FF), or 0 if wedged. */
uint32_t battery_read_raw_ch(uint32_t ch)
{
#if !defined(HOST_TEST)
    uint32_t raw = bat_adc_convert_ch(ch & 0xFu);

    if (raw == BAT_CONV_TIMEOUT) {
        /* Stock @0x20f34: a timed-out conversion runs the recovery
         * (@0x20db6) then RETRIES once — the first conversion after boot
         * always times out; the recovery is what makes it complete. */
        bat_adc_recovery();
        raw = bat_adc_convert_ch(ch & 0xFu);
        if (raw == BAT_CONV_TIMEOUT)
            return 0;            /* wedged even after the recovery */
    }
    return raw;
#else
    return 0;
#endif
}

int battery_level_percent(uint32_t mv)
{
    /* Linear map over 3.3 V (0 %) .. 4.2 V (100 %) for the 800 mAh
     * Li-ion cell. MARKED EMPIRICAL: replace with a real SoC curve once
     * battery_read_mv is verified on hardware. */
    if (mv <= 3300u) return 0;
    if (mv >= 4200u) return 100;
    return (int)((mv - 3300u) * 100u / 900u);
}
