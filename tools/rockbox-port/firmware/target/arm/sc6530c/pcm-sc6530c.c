/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2025 by Sho Tanimoto
 * Copyright (C) 2026 by B310E-OS project
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/
#include <stdlib.h>
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "thread.h"
#include "backlight.h"
#include "logf.h"
#include "audio.h"
#include "audiohw.h"
#include "sound.h"
#include "file.h"
#include "pcm-internal.h"
#include "pcm_mixer.h"

/* DIAGNOSTIC INSTRUMENT (keylight blink): proves the pcm completion path
 * fires. backlight-sc6530c.c's keylight_set + the current setting global. */
extern void keylight_set(int level);

/*
 * B310E-OS Rockbox port — b310e/target/arm/sc6530c/pcm-sc6530c.c
 * (GPLv2, Rockbox-derived; modeled on Rockbox's s3c2440/gigabeat-fx/
 * pcm-meg-fx.c builtin_pcm_sink — the CURRENT sink API).
 *
 * The sink pushes the mixer's 16-bit stereo frames at 8 kHz to the SC6530C
 * VBC DA path (VBDAL/VBDAR 0x82003000/04) — the custom-OS audio route
 * (the ARM codec chain alone is documented as hang-free but SILENT; the
 * music data path is DSP/NV-driven at 48 kHz, out of scope — Rockbox's
 * DSP layer resamples everything down to the sink's 8000).
 *
 * The full hardware bring-up (subsystem power P2-P6 + codec ladder +
 * DAC path + DP DAC + PA + VBC data path with the D8 fixes) happens in
 * audiohw-sc6530c.c audiohw_init(), called from sink_init() — the same
 * convention as the other ARM sinks (pcm-gigabeat-s.c).
 *
 * VOLUME: volume_type is PCM_SINK_SWVOL — Rockbox's swvol DSP path applies
 * the volume setting to the samples BEFORE ops.play(), so the sink output
 * is already scaled; audiohw_set_volume() deliberately does not re-scale.
 */

#define REG32(a) (*(volatile uint32_t *)(a))

/* ---- sink caps ----------------------------------------------------------
 * The hardware's ONLY honest rate is 8 kHz (the VBC DA path). The table
 * stays {8000} even though HW_SAMPR_CAPS also advertises 44 kHz (see
 * config/b310e.h — pcm_sampr.h's HW_FREQ_DEFAULT chain #errors without a
 * 44/48 cap); pcm_set_frequency() rounds every request down to 8000 and
 * the DSP resamples to it. */
static const unsigned long b310e_sampr[] =
{
    8000,
};
#define B310E_NUM_SAMPRS  1
#define B310E_DEFAULT_FREQ 0

/* ---- VBC DA registers (vbc_phy_v5.h + dsp-data-path.md D8) ------------- */
#define VBC_VBDAL     0x82003000u
#define VBC_VBDAR     0x82003004u

/* ---- ops ---------------------------------------------------------------- */

static void sink_init(void)
{
    /* Bring up the whole audio chain (subsystem power P2-P6 -> codec ladder
     * -> DAC path r2=1 -> DAC-on -> DP DAC -> PA -> VBC DA data path) and
     * leave the VBC enabled. See audiohw-sc6530c.c for the exact
     * register sequence and the D1-D8 fixes from the audio findings. */
    audiohw_init();
}

static void sink_postinit(void)
{
}

static void sink_set_freq(uint16_t freq)
{
    /* Only one rate (8000) is advertised; remember it, do nothing. */
    (void)freq;
}

static void sink_lock(void)
{
    /* No DMA interrupt to guard — the swvol double-buffer is the only
     * producer and runs in thread context. */
}

static void sink_unlock(void)
{
}

static void sink_play(const void *addr, size_t size)
{
    const void *buf = addr;
    size_t sz = size;

    /* DIAGNOSTIC: a keylight pulse at entry proves start_pcm -> ops.play
     * actually ran when the user presses PLAY. */
    keylight_set(0);
    sleep(1);
    keylight_set(backlight_brightness);

    /* Blocking completion loop (the sink IS the DMA ISR — nothing in
     * pcm.c/pcm_sw_volume.c re-calls ops.play). Per buffer: push it to
     * the VBC (fast bounded writes, no drain — the DA overrun is
     * irrelevant while the output is silent, and the position is derived
     * from CONSUMED samples), yield briefly (sleep(1) — UI stays
     * responsive, NO CPU spin), then the reference handshake:
     * complete_callback returns the next double-buffer (or false = stop)
     * and status_callback(STARTED) flips/refills it.
     *
     * PACING: the previous version slept ~20 ms per 160-word burst
     * (≈ real-time) — the pipeline then never visibly advanced (the
     * position is driven by the codec's decode, which in turn only
     * advances as buffers are consumed; at real-time pacing the visible
     * update lag made it look frozen). The task allows a fast-counting
     * timestamp over a frozen one: consume as fast as the codec supplies
     * (MP3 at 208 MHz cached decodes faster than real-time) with only a
     * minimal per-buffer yield to keep the scheduler fair. */
    for (;;) {
        const int16_t *samples = (const int16_t *)buf;
        size_t n = sz / sizeof(int16_t);
        size_t i;

        for (i = 0; i + 1 < n; i += 2) {
            REG32(VBC_VBDAL) = (uint32_t)(uint16_t)samples[i];
            REG32(VBC_VBDAR) = (uint32_t)(uint16_t)samples[i + 1];
        }
        if (i < n) {
            uint32_t s = (uint32_t)(uint16_t)samples[i];

            REG32(VBC_VBDAL) = s;
            REG32(VBC_VBDAR) = s;
        }

        sleep(1);

        /* DIAGNOSTIC: blink the keylight per completed buffer. The user
         * reports: blinking/flickering = the completion path fires; a
         * single pulse then nothing = the loop exits after the first
         * buffer; no change at all = sink_play never entered. */
        keylight_set(0);
        sleep(1);
        keylight_set(backlight_brightness);

        if (!pcm_play_dma_complete_callback(PCM_DMAST_OK, &buf, &sz))
            break;
        pcm_play_dma_status_callback(PCM_DMAST_STARTED);
    }

    /* STOP→RESTART HANDOFF (2026-08-27 fix): the pcmbuf's restart branch
     * (apps/pcmbuf.c pcmbuf_request_buffer, the !playing path) requires
     * mixer_channel_status(PCM_MIXER_CHAN_PLAYBACK) == CHANNEL_STOPPED, but
     * on this target the channel's STOPPED status relies entirely on
     * mixer_buffer_callback's dry-detection firing while the sink's loop
     * unwinds — if any state (fade_out_complete, a paused/playing residue)
     * makes the channel read non-STOPPED, the restart never fires and the
     * codec blocks forever on a full pcmbuf. Force the channel back to
     * CHANNEL_STOPPED here so the next pcmbuf_request_buffer always hits the
     * restart branch (idempotent; safe with our no-op pcm locks). */
    mixer_channel_stop(PCM_MIXER_CHAN_PLAYBACK);
}

static void sink_stop(void)
{
}

/* ---- the sink ----------------------------------------------------------- */

struct pcm_sink builtin_pcm_sink =
{
    .caps = {
        .samprs       = b310e_sampr,
        .num_samprs   = B310E_NUM_SAMPRS,
        .default_freq = B310E_DEFAULT_FREQ,
        .volume_type  = PCM_NATIVE_VOLUME_TYPE,
    },
    .ops = {
        .init     = sink_init,
        .postinit = sink_postinit,
        .set_freq = sink_set_freq,
        .lock     = sink_lock,
        .unlock   = sink_unlock,
        .play     = sink_play,
        .stop     = sink_stop,
    },
};
