/* B310E OS codec module: WAV (Rockbox's wav.c and libpcm).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "glue.h"

static const int formats[] = { AFMT_PCM_WAV, 0 };

CODEC_MODULE("wav", "wav", "WAV (libpcm)", 4 * 1024, formats, get_wave_metadata);
