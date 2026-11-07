/* B310E OS — the Rockbox build configuration the codec sources see.
 *
 * Rockbox's decoders (lib/rbcodec/codecs) and metadata parsers
 * (lib/rbcodec/metadata) are built unchanged against these shim headers
 * (os/media/rockbox/) instead of Rockbox's firmware and config headers:
 * an ARM926EJ-S (ARMv5TE, no FPU), one core, little endian, 4 MB, no
 * IRAM sections. Also included by assembly (libmad's mad_iram.h).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef B310E_RB_CONFIG_H
#define B310E_RB_CONFIG_H

#define ROCKBOX 1
#define CODEC 1
#define TARGET_ID 0
#define NUM_CORES 1
#define MEMORYSIZE 4
#define CPU_ARM 1
#define CPU_ARM_CLASSIC 1
#define ARM_ARCH 5
#define ROCKBOX_LITTLE_ENDIAN 1
#define CONFIG_PLATFORM 1
#define PLATFORM_NATIVE 1
#define PLATFORM_HOSTED 2
#define PLATFORM_ANDROID 4
#define CONFIG_CPU 0
#define CONFIG_CODEC 1
#define SWCODEC 1
#define HZ 100
#define MAX_PATH 260
#define MEM_ALIGN_SIZE 4
#define CACHEALIGN_SIZE 32
#define NATIVE_FREQUENCY 44100
#define HAVE_ALBUMART 1             /* the tag parsers note embedded covers */
#define CODEC_SIZE 0x80000          /* the MP4 demuxer bounds its tables by it */

/* no IRAM: every section stays in the module image */
#define ICODE_ATTR
#define ICONST_ATTR
#define IDATA_ATTR
#define IBSS_ATTR
#define ICODE_ATTR_FLAC
#define IBSS_ATTR_FLAC_LARGE_IRAM
#define ICODE_ATTR_FLAC_LARGE_IRAM
#define ICODE_ATTR_ALAC
#define IBSS_ATTR_SHORTEN_DECODED0
#define ICODE_ATTR_TREMOR_NOT_MDCT
#define ICODE_ATTR_TREMOR_MDCT
#define ICONST_ATTR_TREMOR_WINDOW
#define IBSS_ATTR_TREMOR_IRAM
#define IBSS_ATTR_TREMOR_LARGE
#define ICODE_ATTR_MPA_SYNTH
#define ICONST_ATTR_MPA_HUFFMAN
#define IBSS_ATTR_MPC_SAMPLE_BUF
#define ICODE_ATTR_DEMAC
#define ICODE_ATTR_FAAD_LARGE_IRAM
#define IBSS_ATTR_FAAD_LARGE_IRAM
#define ICONST_ATTR_FAAD_LARGE_IRAM
#define IBSS_ATTR_FAAD_XLR_IRAM
#define ICODE_ATTR_FAAD_XLR_IRAM
#define ICODE_ATTR_OPUS_LARGE_IRAM
#define IBSS_ATTR_OPUS_LARGE_IRAM
#define ICONST_ATTR_OPUS_LARGE_IRAM
#define ICONST_ATTR_OPUS_XLR_IRAM
#define IBSS_ATTR_WMA_LARGE_IRAM
#define IBSS_ATTR_WMA_XL_IRAM
#define ICODE_ATTR_WMA_LARGE_IRAM
#define ICONST_ATTR_WMA_LARGE_IRAM
#define ICONST_ATTR_WMA_XL_IRAM
#define DATA_ATTR
#define SHAREDBSS_ATTR
#define SHAREDDATA_ATTR
#define INIT_ATTR
#define INITDATA_ATTR
#define NO_PROF_ATTR

#ifdef __ASSEMBLER__
/* Rockbox's return macros (firmware/export/config.h); on ARMv5 a load
 * into pc interworks */
.macro ldmpc cond="", order="ia", regs
    ldm\order\cond sp!, { \regs, pc }
.endm
.macro ldrpc cond=""
    ldr\cond pc, [sp], #4
.endm
#else
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define MEM_ALIGN_ATTR      __attribute__((aligned(MEM_ALIGN_SIZE)))
#define CACHEALIGN_ATTR     __attribute__((aligned(CACHEALIGN_SIZE)))
#define MEM_ALIGN_UP(x)     (((x) + MEM_ALIGN_SIZE - 1) & ~(MEM_ALIGN_SIZE - 1))
#define ALIGN_UP(n, a)      (((n) + (a) - 1) & ~((a) - 1))
#define ALIGN_DOWN(n, a)    ((n) & ~((a) - 1))
#define CACHEALIGN_UP(x)    ALIGN_UP(x, CACHEALIGN_SIZE)
#define ATTRIBUTE_PRINTF(f, v) __attribute__((format(printf, f, v)))
#define LIKELY(x)           __builtin_expect(!!(x), 1)
#define UNLIKELY(x)         __builtin_expect(!!(x), 0)
#define FORCE_INLINE        inline __attribute__((always_inline))
#define NO_INLINE           __attribute__((noinline))
#define UNUSED_ATTR         __attribute__((unused))
#define USED_ATTR           __attribute__((used))
#define IF_PRIO(...)
#define IF_COP(...)

#ifndef ARRAYLEN
#define ARRAYLEN(a)         (sizeof(a) / sizeof((a)[0]))
#endif
#ifndef MIN
#define MIN(x, y)           ((x) < (y) ? (x) : (y))
#endif
#ifndef MAX
#define MAX(x, y)           ((x) > (y) ? (x) : (y))
#endif
#ifndef BIT_N
#define BIT_N(n)            (1U << (n))
#endif
#define MASK_N(type, n, shift) ((type)((((type)1 << (n)) - (type)1) << (shift)))

/* rbendian.h */
static inline uint16_t swap16(uint16_t v) { return (uint16_t)(v << 8 | v >> 8); }
static inline uint32_t swap32(uint32_t v) { return __builtin_bswap32(v); }
static inline uint64_t swap64(uint64_t v) { return __builtin_bswap64(v); }
#define swap_odd_even32(v)  ((((v) & 0xff00ff00u) >> 8) | (((v) & 0x00ff00ffu) << 8))
#define letoh16(x) (x)
#define letoh32(x) (x)
#define letoh64(x) (x)
#define htole16(x) (x)
#define htole32(x) (x)
#define htole64(x) (x)
#define betoh16(x) swap16(x)
#define betoh32(x) swap32(x)
#define betoh64(x) swap64(x)
#define htobe16(x) swap16(x)
#define htobe32(x) swap32(x)
#define htobe64(x) swap64(x)
#define SWAP16(x)  ((uint16_t)((((x) & 0xff) << 8) | (((x) >> 8) & 0xff)))
#define SWAP32(x)  ((uint32_t)((((x) & 0xff) << 24) | (((x) & 0xff00) << 8) | \
                               (((x) >> 8) & 0xff00) | (((x) >> 24) & 0xff)))
#define H_TO_LE16(x) (x)
#define H_TO_LE32(x) (x)
#define H_TO_BE16(x) SWAP16(x)
#define H_TO_BE32(x) SWAP32(x)
#define LE_TO_H16(x) (x)
#define LE_TO_H32(x) (x)
#define BE_TO_H16(x) SWAP16(x)
#define BE_TO_H32(x) SWAP32(x)

/* kernel bits a few codecs name */
#define TIMEOUT_BLOCK  -1
#define TIMEOUT_NOBLOCK 0
#define DEFAULT_STACK_SIZE 0x1000
#endif /* !__ASSEMBLER__ */

#endif
