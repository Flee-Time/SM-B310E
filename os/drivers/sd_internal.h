/* B310E OS — between the SD block layer (sd.c) and the SDIO driver.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef OS_SD_INTERNAL_H
#define OS_SD_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

extern unsigned sdio_shl;
extern uint32_t sdio_num_blocks;
extern uint32_t sdio_acmd41_tries;  /* the last probe's ACMD41 tries */

/* recover: the full 350 ms power-up wait (a wedged card); else 1 ms when the
 * SD supply was on already, 40 ms when switched on here. true: it was on;
 * *ldo (may be NULL) the LDO registers read. */
bool sdio_init(bool recover, uint32_t *ldo);
int sdcard_init(void);
int sdio_card_status(uint32_t *status);
void sdio_stop(void);
int sdio_select(bool on);
void sdio_power_off(void);
void sdio_power_on(void);
int sdio_transfer(uint32_t lba, uint8_t *buf, unsigned count, bool write);

#endif
