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
static uint32_t bank_count;
static unsigned peak_sample[2];
static bool rec_running;
static unsigned rec_lock_depth;

/* The DMA IRQ line is shared by playback and capture. It is enabled only
 * while one of them runs and neither side holds its lock. */
static void dma_irq_update(void)
{
    if ((running || rec_running) && !lock_depth && !rec_lock_depth)
        SC_AUDIO_REG(0x80000008) |= DMA_IRQ;
    else
        SC_AUDIO_REG(0x8000000c) = DMA_IRQ;
}

void sc6530_pcm_debug(uint32_t *banks, unsigned peak[2])
{
    int old = disable_irq_save();
    *banks = bank_count;
    peak[0] = peak_sample[0];
    peak[1] = peak_sample[1];
    restore_irq(old);
}

static void sink_lock(void)
{
    int old = disable_irq_save();
    lock_depth++;
    dma_irq_update();
    restore_irq(old);
}

static void sink_unlock(void)
{
    int old = disable_irq_save();
    if (lock_depth)
        lock_depth--;
    dma_irq_update();
    restore_irq(old);
}

static void stop_hardware(void)
{
    /* VB enable (bit15) is shared with capture; clear only DA0/DA1 while
     * a recording runs. */
    SC_VBC_CTRL &= rec_running ? ~(SC_VBC_PLAY & ~0x8000u) : ~SC_VBC_PLAY;
    DMA_REG(DMA_LEFT, 8) = 0;
    DMA_REG(DMA_RIGHT, 8) = 0;
    DMA_REG(DMA_LEFT, 12) = DMA_ACK;
    DMA_REG(DMA_RIGHT, 12) = DMA_ACK;
    audiohw_mute(true);
    running = false;
    completed = 0;
    dma_irq_update();
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
        for (unsigned ch = 0; ch < 2; ch++)
        {
            int sample = plane[ch][n];
            unsigned magnitude = sample < 0 ? -sample : sample;
            if (magnitude > peak_sample[ch])
                peak_sample[ch] = magnitude;
        }
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

#ifdef HAVE_RECORDING
static void rec_irq(void);
#endif

void DMA(void)
{
#ifdef HAVE_RECORDING
    rec_irq();
#endif
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
    bank_count++;
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
    bank_count = 0;
    peak_sample[0] = peak_sample[1] = 0;
    fill_plane();
    /* Stock 0xa7b96: bind VBC DA0/DA1 hardware requests15/16 to
     * one-based DMA channels4/3. Channel enable and destination address
     * alone do not connect the peripheral's request to the DMA engine. */
    SC_AUDIO_REG(0x20102038) = DMA_LEFT + 1;
    SC_AUDIO_REG(0x2010203c) = DMA_RIGHT + 1;
    arm_stereo();
    running = true;
    audiohw_mute(false);
    SC_VBC_CTRL = (SC_VBC_CTRL & ~SC_VBC_CPU_ACCESS) | SC_VBC_PLAY;
    dma_irq_update();
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

#ifdef HAVE_RECORDING
/* Capture through the VBC AD FIFOs (stock audio_dai_vbc/vbc_phy_v5).
 * Like stock, both AD channels are enabled (VBC_CTRL bits 12/11, owner
 * 0x18 set by audiohw_enable_recording):
 *   AD0 (ADC L): FIFO 0x8200300c, request 14 -> map 0x20102034, DMA ch5
 *   AD1 (ADC R): FIFO 0x82003008, request 13 -> map 0x20102030, DMA ch6
 * Mono (microphone) drains AD0 only and writes each sample to both
 * channels. Stereo (FM line-in) drains both and interleaves them; a bank
 * is used once both channels have filled it. */
#define DMA_REC_L    4                      /* channels 5 and 6, one-based */
#define DMA_REC_R    5
#define SC_VBC_REC   ((1u << 12) | (1u << 11))

static const struct { uint32_t fifo, map; } rec_port[2] = {
    { SC_VBC_BASE + 0x0c, 0x20102034u },
    { SC_VBC_BASE + 0x08, 0x20102030u },
};
static int16_t rec_bank[2][2][SC_VBC_FRAMES] __attribute__((aligned(32)));
static unsigned rec_next, rec_done;
static bool rec_stereo;
static uint32_t *rec_addr;
static size_t rec_size;
static unsigned rec_banks, rec_railed;
/* Raw AD1 (right) data of the last stereo bank, before any fallback:
 * min, max and the first four samples (I/O dump). */
static int16_t rec_r_min, rec_r_max, rec_r_head[4];

/* Stats-only stereo capture for the ADC R bit sweep (I/O test screen):
 * each bank is measured and dropped; no Rockbox recording runs. */
static void rec_hw_stop(void);
static void rec_arm(void);
static bool rec_probe;
static int16_t probe_min[2], probe_max[2];
static unsigned probe_banks;

static void probe_reset(void)
{
    for (unsigned c = 0; c < 2; c++)
    {
        probe_min[c] = 32767;
        probe_max[c] = -32768;
    }
    probe_banks = 0;
}

void sc6530_rec_probe_start(void)
{
    rec_hw_stop();
    rec_stereo = true;
    rec_next = 0;
    int old = disable_irq_save();
    probe_reset();
    rec_probe = true;
    restore_irq(old);
    SC_AUDIO_REG(rec_port[0].map) = DMA_REC_L + 1;
    SC_AUDIO_REG(rec_port[1].map) = DMA_REC_R + 1;
    SC_VBC_CTRL = (SC_VBC_CTRL & ~SC_VBC_CPU_ACCESS) | 0x8000u | SC_VBC_REC;
    rec_arm();
    rec_running = true;
    dma_irq_update();
}

/* Stats since the last call, then start a new window. */
void sc6530_rec_probe_take(int16_t min[2], int16_t max[2], unsigned *banks)
{
    int old = disable_irq_save();
    for (unsigned c = 0; c < 2; c++)
    {
        min[c] = probe_min[c];
        max[c] = probe_max[c];
    }
    *banks = probe_banks;
    probe_reset();
    restore_irq(old);
}

void sc6530_rec_probe_stop(void)
{
    rec_hw_stop();
    rec_probe = false;
}

void sc6530_rec_right_raw(int16_t *min, int16_t *max, int16_t head[4])
{
    *min = rec_r_min;
    *max = rec_r_max;
    for (unsigned i = 0; i < 4; i++)
        head[i] = rec_r_head[i];
}

unsigned sc6530_rec_right_railed(void)
{
    return rec_railed;
}

unsigned sc6530_rec_banks(void)
{
    return rec_banks;
}

/* ADC R pinned at the negative rail (seen on the phone): every sample of
 * the bank below -30000. Such a bank carries no audio at all. */
static bool bank_railed(const int16_t *bank)
{
    for (unsigned n = 0; n < SC_VBC_FRAMES; n++)
        if (bank[n] > -30000)
            return false;
    return true;
}

void sc6530_rec_set_stereo(bool stereo)
{
    if (!rec_running)
        rec_stereo = stereo;
}

static unsigned rec_channels(void)
{
    return rec_stereo ? 2 : 1;
}

static void rec_arm(void)
{
    rec_done = 0;
    for (unsigned c = 0; c < rec_channels(); c++)
    {
        unsigned ch = c ? DMA_REC_R : DMA_REC_L;
        int16_t *bank = rec_bank[rec_next][c];
        discard_dcache_range(bank, sizeof(rec_bank[0][0]));
        DMA_REG(ch, 8) = 0;
        DMA_REG(ch, 12) = DMA_ACK | DMA_BLOCK_IRQ | DMA_ERROR_IRQ;
        DMA_REG(ch, 16) = rec_port[c].fifo;
        DMA_REG(ch, 20) = (uintptr_t)bank;
        /* Halfword widths, source fixed (bit20 without bit21), 320 bytes. */
        DMA_REG(ch, 24) = 0x50100140u;
        DMA_REG(ch, 28) = SC_VBC_FRAMES * 2;
        DMA_REG(ch, 8) = 0x3001u;
    }
}

static void rec_hw_stop(void)
{
    rec_running = false;
    for (unsigned c = 0; c < 2; c++)
    {
        unsigned ch = c ? DMA_REC_R : DMA_REC_L;
        DMA_REG(ch, 8) = 0;
        DMA_REG(ch, 12) = DMA_ACK;
        SC_AUDIO_REG(rec_port[c].map) = 0;
    }
    SC_VBC_CTRL &= running ? ~SC_VBC_REC : ~(SC_VBC_REC | 0x8000u);
    dma_irq_update();
}

static void rec_irq(void)
{
    uint32_t pending = SC_AUDIO_REG(0x20100010);
    bool error = false;
    for (unsigned c = 0; c < 2; c++)
    {
        unsigned ch = c ? DMA_REC_R : DMA_REC_L;
        if (!(pending & (1u << ch)))
            continue;
        uint32_t status = DMA_REG(ch, 12);
        DMA_REG(ch, 12) = DMA_ACK | DMA_BLOCK_IRQ | DMA_ERROR_IRQ;
        error |= (status & (1u << 12)) != 0;
        if (status & (1u << 9))
            rec_done |= 1u << c;
    }
    if (!rec_running)
        return;
    if (error)
    {
        rec_hw_stop();
        pcm_rec_dma_complete_callback(PCM_DMAST_ERR_DMA, (void **)&rec_addr,
                                      &rec_size);
        return;
    }
    if (rec_done != (rec_stereo ? 3u : 1u))
        return;
    unsigned done_bank = rec_next;
    rec_next ^= 1;
    rec_arm();                      /* keep the VBC drained first */
    const int16_t *left = rec_bank[done_bank][0];
    const int16_t *right = rec_bank[done_bank][rec_stereo ? 1 : 0];
    discard_dcache_range(rec_bank[done_bank], sizeof(rec_bank[0]));
    if (rec_probe)
    {
        for (unsigned c = 0; c < 2; c++)
            for (unsigned n = 0; n < SC_VBC_FRAMES; n++)
            {
                int16_t v = rec_bank[done_bank][c][n];
                if (v < probe_min[c]) probe_min[c] = v;
                if (v > probe_max[c]) probe_max[c] = v;
            }
        probe_banks++;
        return;
    }
    rec_banks++;
    if (rec_stereo)
    {
        int16_t lo = right[0], hi = right[0];
        for (unsigned n = 1; n < SC_VBC_FRAMES; n++)
        {
            if (right[n] < lo) lo = right[n];
            if (right[n] > hi) hi = right[n];
        }
        rec_r_min = lo;
        rec_r_max = hi;
        for (unsigned i = 0; i < 4; i++)
            rec_r_head[i] = right[i];
    }
    if (rec_stereo && bank_railed(right))
    {
        rec_railed++;
        right = left;               /* better mono than a dead channel */
    }
    for (unsigned n = 0; n < SC_VBC_FRAMES; n++)
    {
        if (rec_size < 4)
        {
            void *addr = NULL;
            size_t size = 0;
            if (!pcm_rec_dma_complete_callback(PCM_DMAST_OK, &addr, &size))
            {
                rec_hw_stop();
                return;
            }
            rec_addr = addr;
            rec_size = size;
            pcm_rec_dma_status_callback(PCM_DMAST_STARTED);
        }
        *rec_addr++ = (uint16_t)left[n] | (uint32_t)(uint16_t)right[n] << 16;
        rec_size -= 4;
    }
}

void pcm_rec_lock(void)
{
    int old = disable_irq_save();
    rec_lock_depth++;
    dma_irq_update();
    restore_irq(old);
}

void pcm_rec_unlock(void)
{
    int old = disable_irq_save();
    if (rec_lock_depth)
        rec_lock_depth--;
    dma_irq_update();
    restore_irq(old);
}

void pcm_rec_dma_init(void)
{
    rec_hw_stop();
}

void pcm_rec_dma_close(void)
{
    rec_hw_stop();
}

void pcm_rec_dma_start(void *addr, size_t size)
{
    rec_hw_stop();
    rec_addr = addr;
    rec_size = size & ~3u;
    rec_next = 0;
    rec_banks = rec_railed = 0;
    /* Stock 0xa78e4 order: VBC AD enable, then the DMA channels. */
    SC_AUDIO_REG(rec_port[0].map) = DMA_REC_L + 1;
    if (rec_stereo)
        SC_AUDIO_REG(rec_port[1].map) = DMA_REC_R + 1;
    SC_VBC_CTRL = (SC_VBC_CTRL & ~SC_VBC_CPU_ACCESS) | 0x8000u | SC_VBC_REC;
    rec_arm();
    rec_running = true;
    dma_irq_update();
}

void pcm_rec_dma_stop(void)
{
    rec_hw_stop();
}

const void *pcm_rec_dma_get_peak_buffer(void)
{
    return rec_addr;
}
#endif /* HAVE_RECORDING */
