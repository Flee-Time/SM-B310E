/* B310E OS codec module: FLAC (libffmpegFLAC).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "glue.h"

static const int formats[] = { AFMT_FLAC, 0 };

CODEC_MODULE("flac", "flac", "FLAC (libffmpegFLAC)", 4 * 1024, formats,
             get_flac_metadata);
