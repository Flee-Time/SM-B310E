/* Blood on B310E OS: what all of its files are built with (-include, before anything).
 *
 * int32_t as int, as on the desktops NBlood is written for: this toolchain's is long,
 * and C++ will not take an int32_t * for an int * (the engine's are int). The same
 * size, so C and C++ agree.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#undef __INT32_TYPE__
#define __INT32_TYPE__ int
#undef __UINT32_TYPE__
#define __UINT32_TYPE__ unsigned int
#undef __INT_LEAST32_TYPE__
#define __INT_LEAST32_TYPE__ int
#undef __UINT_LEAST32_TYPE__
#define __UINT_LEAST32_TYPE__ unsigned int

#include <stdint.h>
#include <strings.h>                    /* (strcasecmp: jfbuild's compat.h) */
