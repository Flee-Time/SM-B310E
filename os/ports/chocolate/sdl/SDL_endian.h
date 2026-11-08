/* Chocolate Doom on B310E OS: no SDL (see SDL.h); the ARM is little-endian.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CHOCO_SDL_ENDIAN_H
#define CHOCO_SDL_ENDIAN_H
#include <stdint.h>
#define SDL_LIL_ENDIAN 1234
#define SDL_BIG_ENDIAN 4321
#define SDL_BYTEORDER SDL_LIL_ENDIAN
#define SDL_SwapLE16(x) ((uint16_t)(x))
#define SDL_SwapLE32(x) ((uint32_t)(x))
#define SDL_SwapBE16(x) __builtin_bswap16(x)
#define SDL_SwapBE32(x) __builtin_bswap32(x)
#endif
