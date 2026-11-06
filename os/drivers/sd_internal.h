/* B310E OS — between the SD block layer (sd.c) and the SDIO driver.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef OS_SD_INTERNAL_H
#define OS_SD_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

extern unsigned sdio_shl;
extern uint32_t sdio_num_blocks;

void sdio_init(void);
int sdcard_init(void);
int sdio_card_status(uint32_t *status);
void sdio_stop(void);
int sdio_transfer(uint32_t lba, uint8_t *buf, unsigned count, bool write);

#endif
