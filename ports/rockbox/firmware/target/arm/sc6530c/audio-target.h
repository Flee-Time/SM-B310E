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

/* VBC +0x80 bits 0x30 for capture (unconfirmed; toggled from Debug >
 * View I/O ports) and codec readback: +0x00, +0x40, +0x5c, +0x60, +0x64,
 * +0x70, +0xac, then 0x82001164. */
bool sc6530_rec_route(void);
void sc6530_rec_set_route(bool set);
void sc6530_rec_debug(uint16_t regs[8], bool at_start);
/* Headset mic check (audiohw/powermgmt): raw ADC channel 1 at the last mic
 * recording start, and the +0x64 path used (0xc0 phone, 0x0c headset). */
int sc6530_adc_headset_raw(void);
/* Headset buttons (audiohw): plug-in mic check and mic-line sampling,
 * called from the 100 Hz button tick with the raw EIC channel 4 level;
 * returns the buttons held (Play, Volume up/down). */
unsigned sc6530_headset_tick(bool hook);   /* BUTTON_* mask */
struct sc6530_headset_info {
    bool enabled, check_level, level;
    int check_raw, state, idle, raw, press_raw;
    char button;                    /* 'C', '+', '-' or ' ' */
    unsigned presses[3];            /* centre, +, - (classified) */
    unsigned taps[2], holds[2];     /* +/-: Next/Prev taps, volume holds */
    unsigned muted;                 /* ticks off for headset-mic recording */
    unsigned log_n;                 /* last presses, oldest first */
    char log_cls[6];
    int16_t log_raw[6];
};
void sc6530_headset_debug(struct sc6530_headset_info *info);
int sc6530_headset_mic_raw(void);
int sc6530_rec_mic_path(void);
/* Capture layout (pcm-sc6530c.c): mono from AD0, or stereo AD0 + AD1. */
void sc6530_rec_set_stereo(bool stereo);
/* Stereo banks where AD1 (ADC R) sat at the rail and was replaced with
 * the left channel; and the ADC R power-up variant (audiohw). */
unsigned sc6530_rec_right_railed(void);
unsigned sc6530_rec_banks(void);
void sc6530_rec_right_raw(int16_t *min, int16_t *max, int16_t head[4]);
/* Stats-only stereo capture (pcm) and the ADC R bit sweep (audiohw) that
 * uses it: one entry per register bit flipped, plus a baseline (addr 0). */
void sc6530_rec_probe_start(void);
void sc6530_rec_probe_take(int16_t min[2], int16_t max[2], unsigned *banks);
void sc6530_rec_probe_stop(void);
struct sc6530_sweep {
    uint32_t addr;          /* register written; 0 = baseline */
    uint16_t mask, value;   /* bits changed and their test value */
    int16_t min[2], max[2]; /* left, right */
    unsigned banks;         /* stereo banks seen in the window */
};
int sc6530_adcr_sweep(struct sc6530_sweep *out, int max);
int sc6530_adcr_variant(void);
void sc6530_set_adcr_variant(int variant);

/* Analog FM route on/off (audiohw-sc6530c.c). */
void audiohw_fm_route(bool on);

#endif
