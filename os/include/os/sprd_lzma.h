/* B310E OS — decoder for the SC6530C's LZMA1 variant.
 *
 * The stock images (the DSP program at NOR 0xcc874, the paged user image)
 * use LZMA1 with 11-bit probabilities and plain literals even after a
 * match; standard decoders expect matched literals and reject them. The
 * stream has the usual 13-byte header (properties, dictionary size, 64-bit
 * unpacked size) and no end marker.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef OS_SPRD_LZMA_H
#define OS_SPRD_LZMA_H

#include <stddef.h>
#include <stdint.h>

enum {
    SPRD_LZMA_OK = 0,
    SPRD_LZMA_EHEADER = -1,     /* bad properties or sizes */
    SPRD_LZMA_ENOMEM = -2,
    SPRD_LZMA_EDATA = -3,       /* corrupt stream or ran out of input */
};

/* The unpacked size from the header, or 0 if the header is bad. */
uint32_t sprd_lzma_size(const uint8_t *src, size_t len);

/* Decodes the first `wanted` bytes (at most the unpacked size) into out.
 * Allocates its probability table with kmalloc and frees it again. */
int sprd_lzma_decode(const uint8_t *src, size_t len, uint8_t *out, size_t wanted);

#endif
