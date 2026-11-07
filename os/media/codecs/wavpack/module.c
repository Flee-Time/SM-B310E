/* B310E OS codec module: WavPack (libwavpack).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "glue.h"

static const int formats[] = { AFMT_WAVPACK, 0 };

CODEC_MODULE("wavpack", "wv", "WavPack (libwavpack)", 4 * 1024, formats,
             get_wavpack_metadata);
