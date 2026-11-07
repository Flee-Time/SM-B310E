/* B310E OS codec module: AAC in MP4 (libfaad).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "glue.h"

static const int formats[] = { AFMT_MP4_AAC, AFMT_MP4_AAC_HE, 0 };

CODEC_MODULE("aac", "m4a m4b mp4", "AAC (libfaad)", 512 * 1024, formats,
             get_mp4_metadata);
