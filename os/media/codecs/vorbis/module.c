/* B310E OS codec module: Ogg Vorbis (Tremor).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "glue.h"

static const int formats[] = { AFMT_OGG_VORBIS, 0 };

CODEC_MODULE("vorbis", "ogg oga", "Ogg Vorbis (Tremor)", 512 * 1024, formats,
             get_ogg_metadata);
