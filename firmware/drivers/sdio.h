/*
 * B310E-OS — drivers/sdio.h
 *
 * SD/MMC driver on the SC6530 SDIO0 controller @ 0x20700000. Ported from
 * fpdoom sdio.c (Unlicense, SC6530 branch verbatim — _chip == 3).
 *
 * Block API: sdio_read_block / sdio_write_block (single 512-byte blocks,
 * DMA data path). `sdio_shl` is the byte-addressing shift for the card
 * class: 0 for SDHC/SDXC (block addressing), 9 for SDSC (byte
 * addressing) — sector index must be shifted by it before every call.
 *
 * FAT binding: FAT_READ_SYS / FAT_WRITE_SYS are defined to the block API
 * so microfat.c's sector I/O hooks into this driver (same pattern as
 * fpdoom sdboot/main.c).
 *
 * SAFETY: register writes are restricted to the SDIO0 block (0x20700000),
 * AHB power/reset (0x20500060/0x20500020), the freq gate (0x8b000040),
 * and the SD power LDOs (ADI 0x82001184/0x820011a4). The SDIO pinmux
 * regs 0x8c000250-0x264 are a DIFFERENT set from the forbidden
 * 0x8c0002a4 (UART-TX) — fpdoom's sdboot runs on the B310E with them,
 * but they are a live-pin config: verify on hardware before relying on
 * the 4-bit/high-speed path. All waits are bounded (poll + timeout).
 */

#ifndef B310E_OS_DRIVERS_SDIO_H
#define B310E_OS_DRIVERS_SDIO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Card class shift: 0 = block-addressed (SDHC/SDXC), 9 = byte-addressed
 * (SDSC). Shift every logical sector by this before sdio_read/write. */
extern unsigned sdio_shl;

/* Controller init (power, reset, clock, pins, LDO) — idempotent. */
void sdio_init(void);

/* Card init: CMD0/8/55+ACMD41/2/3/9/7, 4-bit bus, block length, optional
 * high speed. Returns 0 on success; non-zero = the failing stage (see
 * drivers/sdio.c sdcard_init — 1 CMD8 echo, 2 no card, 3 CID, 4 RCA,
 * 5 CSD, 6 select, 7 4-bit, 8 blocklen). diag-sd prints the code. */
int sdcard_init(void);

/* Read/write ONE 512-byte block at logical sector `idx` (shifted by
 * sdio_shl internally). Returns 0 on success, -1 on error. */
int sdio_read_block(uint32_t idx, uint8_t *buf);
int sdio_write_block(uint32_t idx, uint8_t *buf);

#ifdef __cplusplus
}
#endif

#endif /* B310E_OS_DRIVERS_SDIO_H */
