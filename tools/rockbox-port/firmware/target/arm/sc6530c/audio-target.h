/* SC6530C audio register facts from unpatched e52q7a stock playback.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SC6530C_AUDIO_TARGET_H
#define SC6530C_AUDIO_TARGET_H

#define SC_AUDIO_REG(a) (*(volatile uint32_t *)(a))
#define SC_VBC_BASE 0x82003000u
#define SC_VBC_CTRL SC_AUDIO_REG(SC_VBC_BASE + 0x18)
#define SC_VBC_FRAMES 160u
#define SC_VBC_CPU_ACCESS (1u << 10)
#define SC_VBC_PLAY ((1u << 15) | (1u << 14) | (1u << 13))
#define SC_CODEC_DP_CTL SC_AUDIO_REG(0x8a00200c)
#define SC_CODEC_MUTE 0xc000u

void audiohw_mute(bool mute);
bool headphones_inserted(void);
void DMA(void);

static const struct { const char *name; uint32_t addr; } sc_audio_analog_regs[] = {
    { "RAILS", 0x82001164 }, { "DAC EN", 0x82001a00 },
    { "RESET", 0x82001a14 }, { "BIAS", 0x82001a40 },
    { "SPK PA", 0x82001a44 }, { "TRIM", 0x82001a48 },
    { "VCM", 0x82001a4c }, { "DAC OUT", 0x82001a74 },
    { "ROUTE", 0x82001a7c }, { "OUTPUT", 0x82001a84 },
    { "PA MODE", 0x82001a88 }, { "PA DIFF", 0x82001a8c },
    { "HP GAIN", 0x82001a94 }, { "SP GAIN", 0x82001a9c },
    { "DAC CORE", 0x82001aac },
};
#define SC_AUDIO_ANALOG_COUNT (sizeof(sc_audio_analog_regs) / sizeof(sc_audio_analog_regs[0]))
struct sc6530_audio_debug {
    uint16_t analog[SC_AUDIO_ANALOG_COUNT];
    uint32_t valid, banks;
    unsigned peak[2]; /* largest absolute post-volume PCM since playback start */
    int volume, digital_volume;
    uint32_t headset_data, headset_mask, headset_clocks;
    bool initialized, adi_failed, speaker;
};
void sc6530_audio_debug(struct sc6530_audio_debug *info);
void sc6530_pcm_debug(uint32_t *banks, unsigned peak[2]);

#endif
