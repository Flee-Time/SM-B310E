/* SC6530C codec setup from the unpatched e52q7a stock playback trace.
 * Copyright (C) 2026 B310E-OS project
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "audiohw.h"
#include "audio.h"
#include <string.h>
#include "pcm_sampr.h"
#include "pcm_sw_volume.h"
#include "audio-target.h"
#if CONFIG_TUNER
#include "tuner.h"
#endif

/* The running stock codec pointer at 0x042354b0 contains 0x82001a00.
 * 0x82001280 in the older port belongs to a different register layout. */
#define CODEC 0x82001a00u
#define ADI_STS SC_AUDIO_REG(0x82000020)
#define ADI_CMD SC_AUDIO_REG(0x82000018)
#define ADI_DATA SC_AUDIO_REG(0x8200001c)
#define ADI_BUDGET 100000u
#define EIC_CLOCKS ((1u << 25) | (1u << 26))
static bool initialized, adi_failed;
static bool speaker_enabled;
static bool fm_on;           /* analog FM line-in bypass active */
#ifdef HAVE_RECORDING
static bool recording_on;
#endif
static unsigned current_fsel = HW_FREQ_DEFAULT;
static int current_volume = -1000;
static int digital_volume = -1000;
static int volume_left = -1000, volume_right = -1000;

static bool adi_wait(uint32_t mask, bool set)
{
    unsigned budget = ADI_BUDGET;
    while (!!(ADI_STS & mask) != set)
        if (!--budget)
        {
            adi_failed = true;
            return false;
        }
    return true;
}

static bool adi_read_checked(uint32_t addr, uint16_t *value)
{
    int old = disable_irq_save();
    uint32_t data = 0;
    unsigned budget = ADI_BUDGET;
    bool valid = false;
    if (!adi_wait(1u << 8, true))
        goto done;
    ADI_CMD = addr & 0xfff;
    do
    {
        data = ADI_DATA;
        if (!--budget)
        {
            adi_failed = true;
            goto done;
        }
    } while (data & (1u << 31));
    if (((data >> 16) & 0x1fff) != (addr & 0xfff))
        adi_failed = true;
    else
        valid = true;
done:
    *value = data;
    restore_irq(old);
    return valid;
}

static uint16_t adi_read(uint32_t addr)
{
    uint16_t value;
    adi_read_checked(addr, &value);
    return value;
}

static void adi_write(uint32_t addr, uint16_t value)
{
    int old = disable_irq_save();
    if (adi_wait(1u << 9, false))
    {
        SC_AUDIO_REG(addr) = value;
        adi_wait(1u << 8, true);
    }
    restore_irq(old);
}

static void adi_update(uint32_t addr, uint16_t mask, uint16_t bits)
{
    int old = disable_irq_save();
    uint16_t value = adi_read(addr);
    if (!adi_failed)
        adi_write(addr, (value & ~mask) | bits);
    restore_irq(old);
}

void audiohw_mute(bool mute)
{
    /* Stock 0x8131e: enable the ramp controller (15), request mute (14).
     * 0x8000 with bit14 clear is enabled and UNMUTED. */
    SC_CODEC_DP_CTL = (SC_CODEC_DP_CTL & ~SC_CODEC_MUTE) |
                     (mute ? SC_CODEC_MUTE : 0x8000u);
}

void audiohw_set_frequency(int fsel)
{
    /* Rockbox passes an index into hw_freq_sampr, not Hz. */
    static const unsigned rates[] = {
        96000, 48000, 44100, 32000, 24000, 22050,
        16000, 12000, 11025, 9600, 8000,
    };
    current_fsel = (unsigned)fsel < HW_NUM_FREQ ? (unsigned)fsel : HW_FREQ_DEFAULT;
    unsigned rate = hw_freq_sampr[current_fsel];
#ifdef HAVE_RECORDING
    /* Stock 0x81220/0x811f8: ADC_CTL[3:0] = rate / 4000, 1..15. Rates
     * that are not multiples of 4 kHz keep the previous ADC rate. */
    if (rate % 4000 == 0 && rate / 4000 >= 1 && rate / 4000 <= 15)
        SC_AUDIO_REG(0x8a002018) = (SC_AUDIO_REG(0x8a002018) & ~15u) | rate / 4000;
#endif
    for (unsigned mode = 0; mode < sizeof(rates) / sizeof(rates[0]); mode++)
        if (rates[mode] == rate)
        {
            SC_CODEC_DP_CTL = (SC_CODEC_DP_CTL & ~15u) | mode;
            return;
        }
}

bool headphones_inserted(void)
{
    /* Stock product GPIO ID17: active-low logical EIC0. NOR 0xca92c
     * maps it to the DIGITAL bank, not the analog END-button bank.
     * EIC_D open at NOR 0x673c0 enables both APB and RTC sampling clocks.
     * The SD loader only enables analog EIC for END; an unclocked digital
     * input reads zero and falsely reports an inserted headset. Keep the
     * clocks on even when playback closes so Rockbox can poll hotplug. */
    int old = disable_irq_save();
    SC_AUDIO_REG(0x8b0000a0) = EIC_CLOCKS;
    SC_AUDIO_REG(0x8a001004) |= 1;
    bool inserted = !(SC_AUDIO_REG(0x8a001000) & 1);
    restore_irq(old);
    return inserted;
}

static unsigned headphone_gain(void)
{
    /* Stock 0x80a70 programs independent 4-bit gains at CODEC+0x94.
     * Codec codes 1..12 are -33..0 dB in 3 dB steps; 0 mutes. The captured
     * ringtone's code4 is -24 dB, not the maximum. Extend the existing
     * volume scale above 0 using codes5..12 and software attenuation for
     * the intermediate 1 dB steps. No positive digital gain is needed. */
    return 4 + (current_volume > 0 ? (current_volume + 29) / 30 : 0);
}

static void apply_pcm_volume(int gain)
{
    int left = volume_left - gain;
    int right = volume_right - gain;
    if (left > 0) left = 0;
    if (right > 0) right = 0;
    digital_volume = MAX(left, right);
    pcm_set_master_volume(volume_left <= -1000 ? INT_MIN : left,
                          volume_right <= -1000 ? INT_MIN : right);
}

static void apply_volume(bool speaker)
{
#if CONFIG_TUNER
    /* Software PCM volume cannot reach the analog FM path. */
    if (fm_on)
        rtc6211_set_volume_db(MIN(current_volume, 0));
#endif
    int gain = speaker ? 0 : (headphone_gain() - 4) * 30;
    /* The speaker stays at its proven stock PA gain. Positive headphone
     * volume settings saturate at the speaker's existing maximum. */
    apply_pcm_volume(gain);
    if (!speaker)
        adi_update(CODEC + 0x94, 0xff, headphone_gain() * 0x11);
}

static void headset_output(bool enable)
{
    /* Stock product GPIO ID34 at 0x0423cafc: active-high physical GPIO0.
     * ID33 instead calls 0x24d24 -> 0x69d00, enabling the on-die speaker
     * PA; no guessed GPIO18/39 is involved in these captured routes. */
    int old = disable_irq_save();
    sc6530_gpio_clock_on();
    SC_AUDIO_REG(0x8a000018) &= ~1u; /* no pin interrupt */
    SC_AUDIO_REG(0x8a000004) |= 1;   /* data mask */
    SC_AUDIO_REG(0x8a000000) = (SC_AUDIO_REG(0x8a000000) & ~1u) | enable;
    SC_AUDIO_REG(0x8a000008) |= 1;   /* output direction */
    restore_irq(old);
}

static void select_output(bool speaker)
{
    bool muted = (SC_CODEC_DP_CTL & SC_CODEC_MUTE) == SC_CODEC_MUTE;
    audiohw_mute(true);
    headset_output(false);
    adi_update(CODEC + 0x44, 0xa8, 0); /* speaker PA off */
    adi_update(CODEC + 0x84, 0xfc, 0);
    adi_update(CODEC + 0x7c, 0xff, 0);
    adi_update(CODEC + 0x94, 0xff, 0);
    adi_update(CODEC + 0x9c, 0xff, 0);
    udelay(2000);
    if (speaker)
    {
        /* Unplugged stock ringtone: sum both DAC channels into SP+/SP-,
         * AOL enabled, PA differential/demi mode, gain 0x70, PA_EN last. */
        adi_update(CODEC + 0x7c, 0xff, 0x33);
        adi_update(CODEC + 0x84, 0xfc, 0x10);
        adi_update(CODEC + 0x88, 0xf0, 0x30);
        adi_update(CODEC + 0x8c, 0x80, 0x80);
        adi_update(CODEC + 0x9c, 0xff, 0x70);
        adi_update(CODEC + 0x44, 0xa8, 0x88);
    }
    else
    {
        /* Inserted stock ringtone: separate DAC L/R to headphone L/R,
         * with independent stereo gain and output enable GPIO0 high. */
        adi_update(CODEC + 0x88, 0xf0, 0);
        adi_update(CODEC + 0x8c, 0x80, 0);
        adi_update(CODEC + 0x7c, 0xff, 0x84);
        adi_update(CODEC + 0x84, 0xfc, 0xc4);
    }
    if (fm_on)
    {
        /* Analog FM: the DAC leaves the outputs; line-in PGA buffers feed
         * them instead (stock 0x80dea HP, 0x80cf4 SPK, 0x80c54 buffers):
         * HP gets PGAL_BUFP->HPL and PGAR_BUFP->HPR, the speaker gets
         * both PGAs on SP+/SP-. */
        adi_update(CODEC + 0x7c, 0xff, 0);
        adi_update(CODEC + 0x78, 0xff, speaker ? 0x33 : 0x84);
        adi_update(CODEC + 0x60, 0x0f, speaker ? 0x0f : 0x0a);
    }
    else
    {
        adi_update(CODEC + 0x78, 0xff, 0);
        adi_update(CODEC + 0x60, 0x0f, 0);
    }
    speaker_enabled = speaker;
    apply_volume(speaker);
    if (!speaker)
        headset_output(true);
    if (!muted)
        audiohw_mute(false);
}

void audiohw_enable_speaker(bool on)
{
    if (initialized && speaker_enabled != on)
        select_output(on);
}

void audiohw_init(void)
{
    if (initialized)
        return;
    adi_failed = false;
    /* These are write-one set/reset aliases, not readable power halves.
     * Enable only audio and DMA. Do not replay the OS-wide clock ladder
     * or touch keypad/EIC resets, LCD pins or unrelated regulator bits. */
    SC_AUDIO_REG(0x20500060) = 1; /* DMA AHB clock */
    SC_AUDIO_REG(0x8b0000a0) = 1u << 28; /* audio APB clock */
    /* NOR 0x81562: the analog audio clock has its own write-one enable.
     * Later SET writes of 1 and 2 enable the DAC paths without clearing it. */
    adi_write(0x82001440, 4);
    SC_AUDIO_REG(0x8b000060) = (1u << 21) | (1u << 18);
    adi_write(0x82001450, 1);
    udelay(10);
    SC_AUDIO_REG(0x8b000064) = (1u << 21) | (1u << 18);
    adi_write(0x82001454, 1);
    SC_AUDIO_REG(0x8b0001c4) |= 0x764; /* ARM ownership + DA0/1 + analog */
    audiohw_mute(true);
    SC_VBC_CTRL &= ~SC_VBC_PLAY;

    /* NOR 0x80982 and regulator table 0xca38c: the former GPIO IDs
     * 28..31/2 actually clear analog rail power-down bits at 0x1164. */
    adi_update(CODEC + 0x40, 2, 2);      /* bandgap */
    adi_update(CODEC + 0x40, 8, 8);      /* bias */
    adi_update(0x82001164, 0x100, 0);
    adi_update(CODEC + 0x40, 0x80, 0x80); /* VCM */
    adi_update(0x82001164, 0x10, 0);
    adi_update(CODEC + 0x40, 0x40, 0x40); /* VCM buffer */
    adi_update(0x82001164, 0x20, 0);
    adi_update(CODEC + 0x40, 0x20, 0x20); /* VB */
    adi_update(0x82001164, 0x40, 0);
    adi_update(CODEC + 0x40, 0x10, 0x10); /* output bias */
    adi_update(0x82001164, 0x80, 0);
    adi_write(CODEC + 0x14, 0);
    adi_update(CODEC + 0x48, 0xc0, 0x40); /* captured VCM trim */
    adi_update(CODEC + 0x4c, 0x3f, 0x28);
    udelay(3000);

    /* Enable DAC cores; select the stock speaker or headset route below.
     * Earpiece, line-in and recording routes stay disabled. */
    adi_update(CODEC + 0xac, 0x18, 0x18);
    adi_write(0x82001440, 1);
    adi_write(0x82001440, 2);
    SC_AUDIO_REG(0x8a002010) = 0x100;
    SC_AUDIO_REG(0x8a002014) = 8;
    SC_AUDIO_REG(0x8a002000) |= 5;
    adi_update(CODEC + 0x00, 5, 5);
    adi_update(CODEC + 0x74, 0xc0, 0xc0);
    adi_update(CODEC + 0x78, 0xff, 0);
    adi_update(CODEC + 0x80, 0xc0, 0);
    adi_update(CODEC + 0x98, 0xf0, 0);
    adi_update(0x82001290, 1, 0); /* ARM analog ownership gate */
    select_output(!headphones_inserted());

    SC_AUDIO_REG(SC_VBC_BASE + 0x10) =
        (SC_AUDIO_REG(SC_VBC_BASE + 0x10) & ~0xff00u) |
        ((SC_VBC_FRAMES - 1) << 8);
    SC_AUDIO_REG(SC_VBC_BASE + 0x3c) &= ~3u;
    SC_AUDIO_REG(SC_VBC_BASE + 0x40) &= ~3u;
    SC_AUDIO_REG(SC_VBC_BASE + 0x48) &= ~(1u << 10);
    SC_AUDIO_REG(SC_VBC_BASE + 0x78) &= ~(1u << 11);
    SC_AUDIO_REG(SC_VBC_BASE + 0x7c) &= ~(1u << 11);
    /* Clear both banks under CPU ownership before the first DMA request. */
    for (unsigned bank = 0; bank < 2; bank++)
    {
        SC_VBC_CTRL = SC_VBC_CPU_ACCESS | (bank << 9);
        for (unsigned i = 0; i < SC_VBC_FRAMES; i++)
        {
            SC_AUDIO_REG(SC_VBC_BASE) = 0;
            SC_AUDIO_REG(SC_VBC_BASE + 4) = 0;
        }
    }
    SC_VBC_CTRL = 0;
    audiohw_set_frequency(current_fsel);
    initialized = !adi_failed;
    if (adi_failed)
        audiohw_close();
}

#ifdef HAVE_RECORDING
/* Main microphone capture, from the stock ARM-side codec driver (no DSP):
 * mic input path 0x80eec, bias 0x80876/0x80846, ADC power 0x80b4e (ADC L
 * only, as stock records mono), ADC rate 0x81220. Register names are
 * inferred from the stock call structure; see docs/rockbox-recording.md. */
static bool vbc_ad_route = false;
static int mic_gain = DEFAULT_REC_MIC_GAIN;

/* VBC +0x80 bits 0x30 (stock 0xb08ce via 0xa7e28) route AD to the DSP.
 * Hardware 2026-11-06: with them set the ARM FIFO reads all zeros; with
 * them clear the mic records cleanly. Debug > View I/O ports key 7 still
 * toggles them for experiments. */
bool sc6530_rec_route(void)
{
    return vbc_ad_route;
}

void sc6530_rec_set_route(bool set)
{
    vbc_ad_route = set;
    if (recording_on)
        SC_AUDIO_REG(SC_VBC_BASE + 0x80) =
            (SC_AUDIO_REG(SC_VBC_BASE + 0x80) & ~0x30u) | (set ? 0x30u : 0);
}

static uint16_t rec_snap[8];   /* codec state right after recording start */

static void rec_read(uint16_t regs[8])
{
    static const uint16_t offs[7] = { 0x00, 0x40, 0x5c, 0x60, 0x64, 0x70, 0xac };
    for (unsigned i = 0; i < 7; i++)
        regs[i] = adi_read(CODEC + offs[i]);
    regs[7] = adi_read(0x82001164);
}

void sc6530_rec_debug(uint16_t regs[8], bool at_start)
{
    if (at_start)
        memcpy(regs, rec_snap, sizeof(rec_snap));
    else
        rec_read(regs);
}

void audiohw_set_recvol(int left, int right, int type)
{
    (void)right;
    if (type != AUDIO_GAIN_MIC)
        return;
    mic_gain = left < 0 ? 0 : left > 15 ? 15 : left;
    if (initialized)
        adi_update(CODEC + 0x70, 0xf0, mic_gain << 4);
}

void audiohw_enable_recording(bool source_mic)
{
    if (!source_mic || recording_on)
        return;
    if (!initialized)
        audiohw_init();
    /* Stock capture DAI modes are 0x30xxxxxx: both AD channels are
     * ARM-owned and enabled, DMA drains AD0 only. */
    SC_AUDIO_REG(0x8b0001c4) |= 0x18;
    SC_AUDIO_REG(SC_VBC_BASE + 0x80) =
        (SC_AUDIO_REG(SC_VBC_BASE + 0x80) & ~0x30u) | (vbc_ad_route ? 0x30u : 0);
    audiohw_set_frequency(current_fsel);      /* ADC fs before ADC enable */
    adi_update(CODEC + 0x70, 0xf0, mic_gain << 4);
    adi_update(CODEC + 0x64, 0xc0, 0xc0);     /* main mic -> ADC L */
    adi_update(0x82001164, 0x200, 0);         /* mic bias supply on */
    adi_update(CODEC + 0x40, 0x04, 0x04);     /* mic bias */
    udelay(20000);                            /* stock has no wait */
    /* ADC core, stock power state 5 (0x820d4 -> 0x8153e, 0x80af8). Stock
     * will not enter the ADC state (0x82146) without it; skipping it left
     * the ADC pinned at full scale on hardware. */
    adi_write(0x82001440, 1);
    adi_write(0x82001440, 2);
    adi_update(CODEC + 0x5c, 0x80, 0);
    adi_update(CODEC + 0x60, 0xa0, 0);
    adi_update(CODEC + 0xac, 0x40, 0);
    adi_update(CODEC + 0x5c, 0x02, 0);
    SC_AUDIO_REG(0x8a002000) |= 0x02;         /* digital ADC L */
    adi_update(CODEC + 0x00, 0x02, 0x02);     /* analog ADC L */
    adi_update(CODEC + 0x5c, 0x20, 0);        /* ADC L power-down off */
    SC_AUDIO_REG(SC_VBC_BASE + 0x10) =
        (SC_AUDIO_REG(SC_VBC_BASE + 0x10) & ~0xffu) | (SC_VBC_FRAMES - 1);
    rec_read(rec_snap);
    recording_on = true;
}

void audiohw_disable_recording(void)
{
    if (!recording_on)
        return;
    adi_update(CODEC + 0x5c, 0x20, 0x20);
    adi_update(CODEC + 0x00, 0x02, 0);
    SC_AUDIO_REG(0x8a002000) &= ~0x02u;
    adi_update(CODEC + 0x5c, 0x80, 0x80);     /* ADC core off */
    adi_update(CODEC + 0x60, 0xa0, 0xa0);
    adi_update(CODEC + 0xac, 0x40, 0x40);
    adi_update(CODEC + 0x64, 0xc0, 0);
    adi_update(CODEC + 0x40, 0x04, 0);
    adi_update(0x82001164, 0x200, 0x200);
    SC_AUDIO_REG(0x8b0001c4) &= ~0x18u;
    SC_AUDIO_REG(SC_VBC_BASE + 0x80) &= ~0x30u;
    recording_on = false;
}

void audiohw_set_monitor(bool enable)
{
    (void)enable;                             /* no analog mic monitor */
}
#endif /* HAVE_RECORDING */

#if CONFIG_TUNER
/* FM line-in bypass: AIL/AIR -> ADC PGAs -> PGA output buffers -> HP or
 * speaker mixer, no ADC/DAC data path and no DSP (stock sprd_codec v0
 * LINEIN function). See docs/rockbox-fmradio.md. */
#define FM_LINE_GAIN 0x88           /* +0x70 L/R PGA step; NV value unknown */

void audiohw_fm_route(bool on)
{
    if (on == fm_on)
        return;
    if (on)
    {
        if (!initialized)
            audiohw_init();
        adi_update(CODEC + 0x5c, 0x80, 0);       /* ADC input buffer */
        adi_update(CODEC + 0x60, 0xa0, 0);       /* PGA L/R power */
        adi_update(CODEC + 0x70, 0xff, FM_LINE_GAIN);
        adi_update(CODEC + 0x6c, 0xfc, 0xac);    /* AIL->L, AIR->R, VCM */
        sleep(HZ / 4);                           /* stock line-in settle */
        fm_on = true;
    }
    else
    {
        fm_on = false;
        adi_update(CODEC + 0x6c, 0xfc, 0);
#ifdef HAVE_RECORDING
        if (!recording_on)
#endif
        {
            adi_update(CODEC + 0x60, 0xa0, 0xa0);
            adi_update(CODEC + 0x5c, 0x80, 0x80);
        }
    }
    select_output(speaker_enabled);
}
#endif /* CONFIG_TUNER */

void audiohw_preinit(void) {}
void audiohw_postinit(void) {}

void audiohw_close(void)
{
#ifdef HAVE_RECORDING
    audiohw_disable_recording();
#endif
    audiohw_mute(true);
    SC_VBC_CTRL &= ~SC_VBC_PLAY;
    headset_output(false);
    adi_update(CODEC + 0x44, 0xa8, 0);
    adi_update(CODEC + 0x84, 0xfc, 0);
    adi_update(CODEC + 0x7c, 0xff, 0);
    adi_update(CODEC + 0x88, 0xf0, 0);
    adi_update(CODEC + 0x8c, 0x80, 0);
    adi_update(CODEC + 0x74, 0xc0, 0);
    adi_update(CODEC + 0x00, 5, 0);
    SC_AUDIO_REG(0x8a002000) &= ~5u;
    adi_update(CODEC + 0xac, 0x18, 0);
    adi_update(CODEC + 0x40, 0xfa, 0);
    adi_update(0x82001164, 0x1f0, 0x1f0);
    SC_AUDIO_REG(0x8b0001c4) &= ~0x160u;
    adi_write(0x82001444, 7); /* NOR 0x8153e/0x81562: DAC paths + audio clock */
    SC_AUDIO_REG(0x8b0000a4) = 1u << 28;
    initialized = false;
}

void audiohw_set_volume(int left, int right)
{
    /* sound.c supplies tenths of a dB. Preserve the old range/default;
     * additional headphone headroom ends at codec unity, code12. */
    unsigned previous_gain = headphone_gain();
    volume_left = MIN(MAX(left, -1000), 240);
    volume_right = MIN(MAX(right, -1000), 240);
    current_volume = MAX(volume_left, volume_right);
    if (!initialized)
    {
        /* No analog access before the codec has power/clocks. */
        apply_pcm_volume(0);
        return;
    }
    bool change_gain = !speaker_enabled && previous_gain != headphone_gain();
    bool muted = (SC_CODEC_DP_CTL & SC_CODEC_MUTE) == SC_CODEC_MUTE;
    if (change_gain)
        audiohw_mute(true);
    apply_volume(speaker_enabled);
    if (change_gain && !muted)
        audiohw_mute(false);
}

void sc6530_audio_debug(struct sc6530_audio_debug *info)
{
    /* Only the known codec/rail registers listed in audio-target.h.
     * Analog reads must use the ADI mailbox, never direct MMIO loads. */
    info->valid = 0;
    for (unsigned i = 0; i < SC_AUDIO_ANALOG_COUNT; i++)
        if (adi_read_checked(sc_audio_analog_regs[i].addr, &info->analog[i]))
            info->valid |= 1u << i;
    info->initialized = initialized;
    info->adi_failed = adi_failed;
    info->speaker = speaker_enabled;
    info->volume = current_volume;
    info->digital_volume = digital_volume;
    info->headset_data = SC_AUDIO_REG(0x8a001000);
    info->headset_mask = SC_AUDIO_REG(0x8a001004);
    info->headset_clocks = SC_AUDIO_REG(0x8b0000a8);
    sc6530_pcm_debug(&info->banks, info->peak);
}
