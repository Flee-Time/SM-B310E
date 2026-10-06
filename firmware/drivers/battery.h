/*
 * B310E-OS — drivers/battery.h
 *
 * Battery fuel gauge for the SM-B310E (SC6530C). The B310E has NO external
 * fuel-gauge IC — the charger is a linear RT9532GQW (U100) with logic pins
 * (CHG_EN/CHG_DET/CHG_STATE/VBAT_SENSE), and the battery voltage is read by
 * the SC6530's internal ADC on **channel 5** (HW-verified via the RSOFT
 * channel sweep 2026-08-22: ch5 is the only input that moves with the
 * battery; the stock charger code reads ch6, which is the USB/charge-detect
 * rail). The docs' Troubleshooting PDF confirms the VBAT_SENSE net runs to
 * the SoC.
 *
 * Register map (dump-mined from adc_phy_v5.c, kern region @0x020FC0):
 *   - 0x82001680  ADC enable: |= 2 before use
 *   - 0x82001684  channel select: (val & ~0xF) | channel (4-bit)
 *   - 0x820016D4  start: |= 1;  reset after: |= 0x3FF
 *   - 0x820016DC  status: bit 0 = conversion done
 *   - 0x820016CC  result: 10-bit value in bits 0-9
 *   - VBAT = channel 5
 *   - mV = raw * 4.39  (HW-calibrated 2026-08-22: multimeter 4.15V at
 *     raw 946; full-scale 1023 -> 4491mV. The stock charger constants
 *     265488/75000 = 3.54 mV/count only fit the ch6 USB-detect rail.)
 *
 * SAFETY: ADI mailbox access only (same primitives as led.c/keypad.c),
 * bounded waits — a wedged ADC degrades to a no-op, never hangs the
 * cooperative scheduler. HOST_TEST: hardware paths compile out.
 */

#ifndef B310E_OS_DRIVERS_BATTERY_H
#define B310E_OS_DRIVERS_BATTERY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize the ADC block (idempotent). Safe to call any time after chip
 * init; the ADI mailbox is alive. */
void battery_init(void);

/* Read the battery voltage in millivolts (ADC channel 5 = VBAT_SENSE).
 * Returns 0 on a wedged/no-op conversion (bounded wait timed out). The
 * mapping is the stock charger-code formula; the scale is MARKED EMPIRICAL
 * until battery_read_raw is calibrated against a battery-in measurement. */
uint32_t battery_read_mv(void);

/* Raw ADC count for VBAT (channel 5, result register & 0x3FF) — the
 * calibration hook. Runs the stock conversion + timeout-recovery + retry.
 * Returns 0 on a wedged conversion. */
uint32_t battery_read_raw(void);

/* Averaged VBAT raw: 60-sample sliding mean (a 1-minute window at one
 * sample per second) that smooths the USB unplug/replug jump. ONLY the
 * per-second display should call this. Returns 0 if wedged. */
uint32_t battery_read_raw_avg(void);

/* Raw ADC count for an ARBITRARY channel 0-15 (same conversion path) —
 * the channel-sweep diagnostic to find which input the battery sits on.
 * Returns 0 on a wedged conversion. */
uint32_t battery_read_raw_ch(uint32_t ch);

/* Stock mV mapping for a raw ADC count (pure arithmetic, no hardware):
 * raw * 4390 / 1000 — HW-calibrated, full-scale 1023 -> 4491 mV. */
uint32_t battery_mv_from_raw(uint32_t raw);

/* Battery level 0-100 from the millivolts (simple linear map over the
 * 3.3 V..4.2 V Li-ion range — a real SoC curve table can replace it once
 * battery_read_mv is HW-verified). */
int battery_level_percent(uint32_t mv);

#ifdef __cplusplus
}
#endif

#endif /* B310E_OS_DRIVERS_BATTERY_H */
