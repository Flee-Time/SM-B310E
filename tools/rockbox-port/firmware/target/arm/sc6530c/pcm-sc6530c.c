/* SC6530C stereo playback, reconstructed from the e52q7a stock DMA/VBC path.
 * Copyright (C) 2026 B310E-OS project
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "config.h"
#include "system.h"
#include "audiohw.h"
#include "pcm-internal.h"
#include "pcm_sampr.h"
#include "audio-target.h"

/* Stock uses standard channel 4 for DA0 and channel 3 for DA1 (one-based).
 * Interleaved Rockbox data is staged as two contiguous halfword planes.
 * Each IRQ means a bank was filled, with up to 160 frames still in VBC.
 * The next plane can then be prepared while that bank plays. */
#define DMA_LEFT  3
#define DMA_RIGHT 2
#define DMA_CHANNELS ((1u << DMA_LEFT) | (1u << DMA_RIGHT))
#define DMA_IRQ (1u << 20)
#define DMA_REG(ch, off) SC_AUDIO_REG(0x20101000u + (ch) * 0x40u + (off))
#define DMA_BLOCK_IRQ 2u
#define DMA_ERROR_IRQ 16u
#define DMA_ACK 0x1f000000u

static int16_t plane[2][SC_VBC_FRAMES] __attribute__((aligned(32)));
static const int16_t *source;
static size_t source_frames;
static unsigned lock_depth, completed;
static bool running, requesting, eof, draining;

static void sink_lock(void)
{
    int old = disable_irq_save();
    if (lock_depth++ == 0)
        SC_AUDIO_REG(0x8000000c) = DMA_IRQ;
    restore_irq(old);
}

static void sink_unlock(void)
{
    int old = disable_irq_save();
    if (lock_depth && --lock_depth == 0 && running)
        SC_AUDIO_REG(0x80000008) |= DMA_IRQ;
    restore_irq(old);
}

static void stop_hardware(void)
{
    SC_AUDIO_REG(0x8000000c) = DMA_IRQ;
    SC_VBC_CTRL &= ~SC_VBC_PLAY;
    DMA_REG(DMA_LEFT, 8) = 0;
    DMA_REG(DMA_RIGHT, 8) = 0;
    DMA_REG(DMA_LEFT, 12) = DMA_ACK;
    DMA_REG(DMA_RIGHT, 12) = DMA_ACK;
    audiohw_mute(true);
    running = false;
    completed = 0;
}

static void sink_stop(void)
{
    /* The core stops synchronously when a completion callback runs dry.
     * Let the bank already queued in VBC finish before muting the DAC.
     * An explicit stop outside that callback remains immediate. */
    if (requesting)
        return;
    stop_hardware();
    source = NULL;
    source_frames = 0;
    eof = draining = false;
}

static unsigned fill_plane(void)
{
    unsigned n;
    for (n = 0; n < SC_VBC_FRAMES; n++)
    {
        if (!source_frames)
        {
            const void *addr = NULL;
            size_t size = 0;
            if (eof)
                break;
            requesting = true;
            bool more = pcm_play_dma_complete_callback(PCM_DMAST_OK, &addr, &size);
            requesting = false;
            if (!more || !addr || size < 4)
            {
                eof = true;
                break;
            }
            source = addr;
            source_frames = size / 4;
            pcm_play_dma_status_callback(PCM_DMAST_STARTED);
        }
        plane[0][n] = *source++;
        plane[1][n] = *source++;
        source_frames--;
    }
    unsigned valid = n;
    while (n < SC_VBC_FRAMES)
    {
        plane[0][n] = plane[1][n] = 0;
        n++;
    }
    commit_dcache_range(plane, sizeof(plane));
    return valid;
}

static void arm_channel(unsigned ch, const int16_t *samples, uint32_t port)
{
    DMA_REG(ch, 8) = 0;
    DMA_REG(ch, 12) = DMA_ACK | DMA_BLOCK_IRQ | DMA_ERROR_IRQ;
    DMA_REG(ch, 16) = (uintptr_t)samples;
    DMA_REG(ch, 20) = port;
    /* Halfword widths [31:30]/[29:28], destination fixed [21:20],
     * 320-byte fragment/block. These fields differ from older SC6530. */
    DMA_REG(ch, 24) = 0x50300140u;
    DMA_REG(ch, 28) = SC_VBC_FRAMES * 2;
    DMA_REG(ch, 8) = 0x3001u; /* priority 3, normal request, enable */
}

static void arm_stereo(void)
{
    completed = 0;
    arm_channel(DMA_LEFT, plane[0], SC_VBC_BASE);
    arm_channel(DMA_RIGHT, plane[1], SC_VBC_BASE + 4);
}

void DMA(void)
{
    uint32_t pending = SC_AUDIO_REG(0x20100010) & DMA_CHANNELS;
    bool error = false;
    for (unsigned ch = DMA_RIGHT; ch <= DMA_LEFT; ch++)
    {
        if (!(pending & (1u << ch)))
            continue;
        uint32_t status = DMA_REG(ch, 12);
        DMA_REG(ch, 12) = DMA_ACK | DMA_BLOCK_IRQ | DMA_ERROR_IRQ;
        error |= (status & (1u << 12)) != 0;
        if (status & (1u << 9))
            completed |= 1u << ch;
    }
    if (!running)
        return;
    if (error)
    {
        const void *addr;
        size_t size;
        stop_hardware();
        pcm_play_dma_complete_callback(PCM_DMAST_ERR_DMA, &addr, &size);
        return;
    }
    /* Do not reuse either plane until both channels finished reading it. */
    if (completed != DMA_CHANNELS)
        return;
    if (draining)
    {
        stop_hardware();
        /* A very short initial buffer can run dry during ops.play(),
         * before the core sets pcm_playing. Finish that state here too. */
        if (pcm_is_playing())
            pcm_play_stop_int();
        return;
    }
    draining = fill_plane() == 0 && eof;
    arm_stereo();
}

static void sink_set_freq(uint16_t freq)
{
    audiohw_set_frequency(freq);
}

static void sink_init(void)
{
    audiohw_init();
    stop_hardware();
}

static void sink_play(const void *addr, size_t size)
{
    sink_stop();
    if (!addr || size < 4)
        return;
    source = addr;
    source_frames = size / 4;
    eof = draining = false;
    fill_plane();
    arm_stereo();
    running = true;
    audiohw_mute(false);
    SC_VBC_CTRL = (SC_VBC_CTRL & ~SC_VBC_CPU_ACCESS) | SC_VBC_PLAY;
    if (!lock_depth)
        SC_AUDIO_REG(0x80000008) |= DMA_IRQ;
}

struct pcm_sink builtin_pcm_sink = {
    .caps = {
        .samprs = hw_freq_sampr,
        .num_samprs = HW_NUM_FREQ,
        .default_freq = HW_FREQ_DEFAULT,
        .volume_type = PCM_NATIVE_VOLUME_TYPE,
    },
    .ops = {
        .init = sink_init,
        .postinit = audiohw_postinit,
        .set_freq = sink_set_freq,
        .lock = sink_lock,
        .unlock = sink_unlock,
        .play = sink_play,
        .stop = sink_stop,
    },
};
