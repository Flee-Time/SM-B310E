/* B310E OS codec module: Apple Lossless in MP4 (libalac).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "glue.h"

static const int formats[] = { AFMT_MP4_ALAC, 0 };

CODEC_MODULE("alac", "m4a m4b mp4", "Apple Lossless (libalac)", 512 * 1024, formats,
             get_mp4_metadata);
