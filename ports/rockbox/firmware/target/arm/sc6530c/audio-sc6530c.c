/* SC6530C audio input routing: playback or the main microphone.
 * Copyright (C) 2026 B310E-OS project
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "config.h"
#include "system.h"
#include "audio.h"
#include "audiohw.h"

void audio_set_output_source(int source)
{
    (void)source;                   /* the DAC only plays PCM */
}

void audio_input_mux(int source, unsigned flags)
{
    static int last_source = AUDIO_SRC_PLAYBACK;
    (void)flags;

    if (source != AUDIO_SRC_MIC)
        source = AUDIO_SRC_PLAYBACK;
    if (source == last_source)
        return;
    if (source == AUDIO_SRC_MIC)
        audiohw_enable_recording(true);
    else
        audiohw_disable_recording();
    last_source = source;
}
