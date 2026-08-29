/*
 * B310E-OS — arch/chip.h
 *
 * SC6530C chip-level register accessors + init API (Wave 3).
 * The init sequence is copied from fpdoom's init_sc6530.h (Unlicense /
 * public domain — verbatim reuse is explicitly permitted).
 *
 * SAFETY RULE (B310E quirk, fpdoom syscode.c:189-197): NEVER write the
 * UART-TX pinmux register 0x8c0002a4 = 0x231 — it HANGS the phone.
 * In fact no 0x8cxxxxxx pin-mux writes at all until the LCD/keypad waves
 * add the specific registers extracted from the stock firmware.
 */

#ifndef B310E_OS_CHIP_H
#define B310E_OS_CHIP_H

#include <stdint.h>

/* ---- register accessors (freestanding, no libc) ------------------------ */
#define MEM4(a)  (*(volatile uint32_t *)(a))
#define MEM2(a)  (*(volatile uint16_t *)(a))
#define DELAY(t) { volatile int _d = (t)*10; while (_d--) ; }  /* rough busy delay */

/* ---- SMC remap --------------------------------------------------------- */
/* SDRAM-remap status bit (fpdoom entry.c:7). Drives the remap-window base
 * used by the SMC init writes below. Read-only AHB register; harmless to
 * probe. (fpdoom forces 0 when built without SDIO — same result here.) */
#define MEM_REMAP (MEM4(0x205000e0) & 1)

/* ---- init API ---------------------------------------------------------- */
/* Full SC6530C chip init: CPU freq -> 208 MHz, SMC/SDRAM controller, ADI
 * enable/reset, IRAM enable, peripheral power gating. No pin mux (Wave 3
 * intentionally skips all 0x8cxxxxxx writes — see header comment). */
void sc6530_chip_init(void);

#endif /* B310E_OS_CHIP_H */
