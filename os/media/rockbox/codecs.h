/* B310E OS — Rockbox's codecs.h for the codec modules: struct codec_api
 * is ours (os/codec.h, the same member names), the module header is
 * written by os/media/codecs/glue.c, so CODEC_HEADER is empty.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef _CODECS_H_
#define _CODECS_H_

#include "config.h"
#include "platform.h"
#include "metadata.h"
#include "os/codec.h"
#include <limits.h>

#define CODEC_HEADER
#define CODEC_API_VERSION 50

#ifndef LDEBUGF
#define LDEBUGF(...) do { } while (0)
#endif
#undef LOGF
#define LOGF(...) do { } while (0)

/* dsp_misc.h */
struct dsp_replay_gains {
    long track_gain, album_gain, track_peak, album_peak;
};

extern struct codec_api *ci;

enum codec_status codec_start(enum codec_entry_call_reason reason);
enum codec_status codec_main(enum codec_entry_call_reason reason);
enum codec_status codec_run(void);

#endif
