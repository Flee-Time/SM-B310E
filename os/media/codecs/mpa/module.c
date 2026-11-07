/* B310E OS codec module: MPEG audio layers 1, 2 and 3 (libmad).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "glue.h"

static const int formats[] = { AFMT_MPA_L3, AFMT_MPA_L2, AFMT_MPA_L1, 0 };

CODEC_MODULE("mpa", "mp3 mp2 mp1 mpa", "MPEG audio (libmad)", 4 * 1024,
             formats, get_mp3_metadata);
