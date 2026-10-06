/* SC6530C codec setup from the unpatched e52q7a stock playback trace.
 * Copyright (C) 2026 B310E-OS project
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "config.h"
#include "system.h"
#include "audiohw.h"
#include "pcm_sampr.h"
#include "pcm_sw_volume.h"
#include "audio-target.h"

/* The running stock codec pointer at 0x042354b0 contains 0x82001a00.
 * 0x82001280 in the older port belongs to a different register layout. */
#define CODEC 0x82001a00u
#define ADI_STS SC_AUDIO_REG(0x82000020)
#define ADI_CMD SC_AUDIO_REG(0x82000018)
#define ADI_DATA SC_AUDIO_REG(0x8200001c)
#define ADI_BUDGET 100000u
static bool initialized, adi_failed;
static bool speaker_enabled;
static unsigned current_fsel = HW_FREQ_DEFAULT;
static int current_volume = -1000;

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
     * maps it to the DIGITAL bank, not the analog END-button bank. */
    SC_AUDIO_REG(0x8a001004) |= 1;
    return !(SC_AUDIO_REG(0x8a001000) & 1);
}

static void headset_output(bool enable)
{
    /* Stock product GPIO ID34 at 0x0423cafc: active-high physical GPIO0.
     * ID33 instead calls 0x24d24 -> 0x69d00, enabling the on-die speaker
     * PA; no guessed GPIO18/39 is involved in these captured routes. */
    int old = disable_irq_save();
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
         * analog gain 0x44 and external output enable GPIO0 high. */
        adi_update(CODEC + 0x88, 0xf0, 0);
        adi_update(CODEC + 0x8c, 0x80, 0);
        adi_update(CODEC + 0x7c, 0xff, 0x84);
        adi_update(CODEC + 0x84, 0xfc, 0xc4);
        adi_update(CODEC + 0x94, 0xff, 0x44);
        headset_output(true);
    }
    speaker_enabled = speaker;
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

void audiohw_preinit(void) {}
void audiohw_postinit(void) {}

void audiohw_close(void)
{
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

void audiohw_set_volume(int val)
{
    current_volume = val;
    /* sound.c supplies tenths of a dB. The software PCM scaler starts
     * muted until this hook sets its master factors. Keep the captured
     * analog gains fixed and apply the user's volume before deinterleave. */
    pcm_set_master_volume(val <= -1000 ? INT_MIN : val,
                          val <= -1000 ? INT_MIN : val);
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
    sc6530_pcm_debug(&info->banks, info->peak);
}
