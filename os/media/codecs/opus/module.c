/* B310E OS codec module: Opus (libopus).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "glue.h"

static const int formats[] = { AFMT_OPUS, 0 };

CODEC_MODULE("opus", "opus ogg oga", "Opus (libopus)", 256 * 1024, formats,
             get_ogg_metadata);
