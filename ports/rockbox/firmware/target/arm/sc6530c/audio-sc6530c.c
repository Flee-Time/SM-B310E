/* SC6530C audio input routing: playback, a microphone or FM radio.
 * Copyright (C) 2026 B310E-OS project
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "config.h"
#include "system.h"
#include "audio.h"
#include "audiohw.h"
#include "audio-target.h"

void audio_set_output_source(int source)
{
    (void)source;                   /* the DAC only plays PCM */
}

/* Playback, a microphone, or FM. FM always plays through the analog
 * bypass; with SRCF_RECORDING its line-in is also captured in stereo. */
void audio_input_mux(int source, unsigned flags)
{
    static int last_source = AUDIO_SRC_PLAYBACK;
    static bool last_rec;
    bool rec = (flags & SRCF_RECORDING) != 0;

    if (source != AUDIO_SRC_MIC && source != AUDIO_SRC_FMRADIO)
        source = AUDIO_SRC_PLAYBACK;
    if (source != AUDIO_SRC_FMRADIO)
        rec = source == AUDIO_SRC_MIC;
    if (source == last_source && rec == last_rec)
        return;
    /* Leave the old source first, then enter the new one. */
    if (last_rec)
        audiohw_disable_recording();
    if (last_source == AUDIO_SRC_FMRADIO && source != AUDIO_SRC_FMRADIO)
        audiohw_fm_route(false);
    if (source == AUDIO_SRC_MIC)
        audiohw_enable_recording(true);
    else if (source == AUDIO_SRC_FMRADIO)
    {
        audiohw_fm_route(true);
        if (rec)
            audiohw_enable_recording(false);
    }
    last_source = source;
    last_rec = rec;
}
