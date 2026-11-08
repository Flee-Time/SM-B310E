/* B310E OS — the SC6530C codec: power, speaker / headphone output, volume,
 * microphone and FM line-in routes, headset detection and its buttons.
 *
 * Ported from the Rockbox port's audiohw-sc6530c.c (GPLv2-or-later, built
 * from the unpatched e52q7a stock playback and recording traces and
 * verified on the phone: playback on both outputs, main and headset mic
 * recording, the FM bypass, the headset buttons). Register comments name
 * the stock code they come from. Everything stays on the ARM: no DSP.
 *
 * Analog codec registers (CODEC + n) and the rails at 0x82001164 go
 * through the ADI; the digital codec (0x8a002000..) and the VBC
 * (0x82003000) are plain MMIO.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/arch.h"
#include "os/audio.h"
#include "os/fs.h"
#include "os/drivers.h"
#include "os/kernel.h"
#include "drivers/audio_internal.h"

#define CODEC       0x82001a00u
#define RAILS       0x82001164u
#define EIC_CLOCKS  ((1u << 25) | (1u << 26))

static mutex_t lock;
static bool lock_ready;
static bool initialized, speaker_enabled, fm_routed, recording_on;
static bool force_speaker;              /* the speaker even with headphones in */
static uint16_t rec_mic_path;           /* +0x64 bits: 0xc0 phone, 0x0c headset */
static bool rec_fm_mix;
static unsigned current_rate = 44100;
static int codec_db10 = -200;               /* tenths of a dB, -1000 = mute */
static int mic_gain = 10, line_gain[2] = { 8, 8 };
static int headset_mic_raw = -2;
static bool hs_powered;                 /* the headset buttons keep the codec on */

static void enter(void)
{
    if (!lock_ready) {
        mutex_init(&lock);
        lock_ready = true;
    }
    mutex_lock(&lock, WAIT_FOREVER);
}

static void leave(void)
{
    mutex_unlock(&lock);
}

static void adi_update(uint32_t addr, uint16_t mask, uint16_t bits)
{
    uint32_t s = arch_irq_save();

    adi_write(addr, (adi_read(addr) & ~mask) | bits);
    arch_irq_restore(s);
}

/* ---- mic bias: shared by recording and the headset buttons -------------- */
#define BIAS_REC   1u
#define BIAS_HOOK  2u
static unsigned bias_users;

/* Mic bias (codec +0x40 bit 2) and its supply (rails bit 0x200, active
 * low); on while any user holds it. */
static void mic_bias(unsigned user, bool on)
{
    uint32_t s = arch_irq_save();
    unsigned before = bias_users;

    bias_users = on ? before | user : before & ~user;
    if (!before != !bias_users) {
        if (bias_users) {
            adi_update(RAILS, 0x200, 0);
            adi_update(CODEC + 0x40, 0x04, 0x04);
        } else {
            adi_update(CODEC + 0x40, 0x04, 0);
            adi_update(RAILS, 0x200, 0x200);
        }
    }
    arch_irq_restore(s);
}

/* ---- jack, mute, rate --------------------------------------------------- */

/* Stock GPIO ID17: active-low digital EIC0 (NOR 0xca92c). The digital EIC
 * needs its APB and RTC sampling clocks (0x673c0), or it reads 0: a false
 * "inserted". */
bool audio_headphones(void)
{
    uint32_t s = arch_irq_save();
    bool in;

    REG32(0x8b0000a0) = EIC_CLOCKS;
    REG32(0x8a001004) |= 1;
    in = !(REG32(0x8a001000) & 1);
    arch_irq_restore(s);
    return in;
}

void audio_mute(bool mute)
{
    /* Stock 0x8131e: ramp controller on (15), mute request (14). */
    VBC_DP_CTL = (VBC_DP_CTL & ~CODEC_MUTE) | (mute ? CODEC_MUTE : 0x8000u);
}

static void set_rate(unsigned rate)
{
    static const unsigned rates[] = {
        96000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 9600, 8000,
    };

    /* ADC_CTL[3:0] = rate / 4000 (stock 0x81220), when a multiple of 4 kHz */
    if (rate % 4000 == 0 && rate / 4000 >= 1 && rate / 4000 <= 15)
        REG32(0x8a002018) = (REG32(0x8a002018) & ~15u) | rate / 4000;
    for (unsigned mode = 0; mode < sizeof(rates) / sizeof(rates[0]); mode++)
        if (rates[mode] == rate) {
            VBC_DP_CTL = (VBC_DP_CTL & ~15u) | mode;
            current_rate = rate;
            return;
        }
}

bool audio_rate_ok(unsigned rate)
{
    return rate == 96000 || rate == 48000 || rate == 44100 || rate == 32000 ||
           rate == 24000 || rate == 22050 || rate == 16000 || rate == 12000 ||
           rate == 11025 || rate == 9600 || rate == 8000;
}

/* ---- volume and outputs ------------------------------------------------- */

/* Headphone gain codes 1..12 are -33..0 dB in 3 dB steps (stock 0x80a70);
 * code 4 is the stock ringtone's level. The speaker stays at its proven
 * stock PA gain; the rest is digital attenuation in pcm.c. */
static unsigned headphone_code(void)
{
    return 4 + (codec_db10 > 0 ? (unsigned)(codec_db10 + 29) / 30 : 0);
}

static void apply_volume(void)
{
    int gain = speaker_enabled ? 0 : ((int)headphone_code() - 4) * 30;
    int digital = codec_db10 - gain;

    if (digital > 0)
        digital = 0;
    pcm_set_attenuation(codec_db10 <= -1000 ? -1000 : digital);
    if (fm_routed)
        fm_set_volume_db10(codec_db10 < 0 ? codec_db10 : 0);
    if (!speaker_enabled && initialized)
        adi_update(CODEC + 0x94, 0xff, (uint16_t)(headphone_code() * 0x11));
}

/* Stock GPIO ID34: active-high GPIO0 enables the headphone output. */
static void headset_output(bool enable)
{
    uint32_t s = arch_irq_save();

    gpio_clock_on();
    REG32(0x8a000018) &= ~1u;
    REG32(0x8a000004) |= 1;
    REG32(0x8a000000) = (REG32(0x8a000000) & ~1u) | enable;
    REG32(0x8a000008) |= 1;
    arch_irq_restore(s);
}

static void select_output(bool speaker)
{
    bool muted = (VBC_DP_CTL & CODEC_MUTE) == CODEC_MUTE;

    audio_mute(true);
    headset_output(false);
    adi_update(CODEC + 0x44, 0xa8, 0);  /* speaker PA off */
    adi_update(CODEC + 0x84, 0xfc, 0);
    adi_update(CODEC + 0x7c, 0xff, 0);
    adi_update(CODEC + 0x94, 0xff, 0);
    adi_update(CODEC + 0x9c, 0xff, 0);
    thread_sleep(2);
    if (speaker) {
        /* Unplugged stock ringtone: both DAC channels into SP+/SP-, AOL,
         * PA differential, gain 0x70, PA_EN last. */
        adi_update(CODEC + 0x7c, 0xff, 0x33);
        adi_update(CODEC + 0x84, 0xfc, 0x10);
        adi_update(CODEC + 0x88, 0xf0, 0x30);
        adi_update(CODEC + 0x8c, 0x80, 0x80);
        adi_update(CODEC + 0x9c, 0xff, 0x70);
        adi_update(CODEC + 0x44, 0xa8, 0x88);
    } else {
        /* Inserted stock ringtone: DAC L/R to headphones L/R. */
        adi_update(CODEC + 0x88, 0xf0, 0);
        adi_update(CODEC + 0x8c, 0x80, 0);
        adi_update(CODEC + 0x7c, 0xff, 0x84);
        adi_update(CODEC + 0x84, 0xfc, 0xc4);
    }
    if (fm_routed) {
        /* Analog FM: the line-in PGA buffers feed the outputs instead of
         * the DAC (stock 0x80dea headphones, 0x80cf4 speaker). */
        adi_update(CODEC + 0x7c, 0xff, 0);
        adi_update(CODEC + 0x78, 0xff, speaker ? 0x33 : 0x84);
        adi_update(CODEC + 0x60, 0x0f, speaker ? 0x0f : 0x0a);
    } else {
        adi_update(CODEC + 0x78, 0xff, 0);
        adi_update(CODEC + 0x60, 0x0f, 0);
    }
    speaker_enabled = speaker;
    apply_volume();
    if (!speaker)
        headset_output(true);
    if (!muted)
        audio_mute(false);
}

static void codec_close(void);

/* Power the codec up (stock 0x80982 order, regulator table 0xca38c). Only
 * audio and DMA clocks; no OS-wide clock ladder, no unrelated rails. */
static void codec_init(void)
{
    if (initialized)
        return;
    cpufreq_hold();                     /* audio DMA and codec at a fixed AHB */
    REG32(0x20500060) = 1;              /* DMA AHB clock */
    REG32(0x8b0000a0) = 1u << 28;       /* audio APB clock */
    adi_write(0x82001440, 4);           /* analog audio clock (0x81562) */
    REG32(0x8b000060) = (1u << 21) | (1u << 18);
    adi_write(0x82001450, 1);
    udelay(10);
    REG32(0x8b000064) = (1u << 21) | (1u << 18);
    adi_write(0x82001454, 1);
    REG32(0x8b0001c4) |= 0x764;         /* ARM ownership, DA0/1, analog */
    audio_mute(true);
    VBC_CTRL &= ~VBC_PLAY;

    adi_update(CODEC + 0x40, 2, 2);     /* bandgap */
    adi_update(CODEC + 0x40, 8, 8);     /* bias */
    adi_update(RAILS, 0x100, 0);
    adi_update(CODEC + 0x40, 0x80, 0x80);       /* VCM */
    adi_update(RAILS, 0x10, 0);
    adi_update(CODEC + 0x40, 0x40, 0x40);       /* VCM buffer */
    adi_update(RAILS, 0x20, 0);
    adi_update(CODEC + 0x40, 0x20, 0x20);       /* VB */
    adi_update(RAILS, 0x40, 0);
    adi_update(CODEC + 0x40, 0x10, 0x10);       /* output bias */
    adi_update(RAILS, 0x80, 0);
    adi_write(CODEC + 0x14, 0);
    adi_update(CODEC + 0x48, 0xc0, 0x40);       /* captured VCM trim */
    adi_update(CODEC + 0x4c, 0x3f, 0x28);
    thread_sleep(3);

    adi_update(CODEC + 0xac, 0x18, 0x18);       /* DAC cores */
    adi_write(0x82001440, 1);
    adi_write(0x82001440, 2);
    REG32(0x8a002010) = 0x100;
    REG32(0x8a002014) = 8;
    REG32(0x8a002000) |= 5;
    adi_update(CODEC + 0x00, 5, 5);
    adi_update(CODEC + 0x74, 0xc0, 0xc0);
    adi_update(CODEC + 0x78, 0xff, 0);
    adi_update(CODEC + 0x80, 0xc0, 0);
    adi_update(CODEC + 0x98, 0xf0, 0);
    adi_update(0x82001290, 1, 0);       /* ARM analog ownership gate */
    initialized = true;
    select_output(force_speaker || !audio_headphones());

    REG32(VBC_BASE + 0x10) = (REG32(VBC_BASE + 0x10) & ~0xff00u) |
                             ((VBC_FRAMES - 1) << 8);
    REG32(VBC_BASE + 0x3c) &= ~3u;
    REG32(VBC_BASE + 0x40) &= ~3u;
    REG32(VBC_BASE + 0x48) &= ~(1u << 10);
    REG32(VBC_BASE + 0x78) &= ~(1u << 11);
    REG32(VBC_BASE + 0x7c) &= ~(1u << 11);
    for (unsigned bank = 0; bank < 2; bank++) { /* clear both banks */
        VBC_CTRL = VBC_CPU_ACCESS | (bank << 9);
        for (unsigned i = 0; i < VBC_FRAMES; i++) {
            REG32(VBC_BASE) = 0;
            REG32(VBC_BASE + 4) = 0;
        }
    }
    VBC_CTRL = 0;
    set_rate(current_rate);
}

static void codec_close(void)
{
    if (!initialized)
        return;
    audio_mute(true);
    VBC_CTRL &= ~VBC_PLAY;
    headset_output(false);
    adi_update(CODEC + 0x44, 0xa8, 0);
    adi_update(CODEC + 0x84, 0xfc, 0);
    adi_update(CODEC + 0x7c, 0xff, 0);
    adi_update(CODEC + 0x88, 0xf0, 0);
    adi_update(CODEC + 0x8c, 0x80, 0);
    adi_update(CODEC + 0x74, 0xc0, 0);
    adi_update(CODEC + 0x00, 5, 0);
    REG32(0x8a002000) &= ~5u;
    adi_update(CODEC + 0xac, 0x18, 0);
    adi_update(CODEC + 0x40, 0xfa, 0);
    adi_update(RAILS, 0x3f0, 0x3f0);    /* incl. the mic bias supply */
    bias_users = 0;
    REG32(0x8b0001c4) &= ~0x160u;
    adi_write(0x82001444, 7);           /* DAC paths + audio clock off */
    REG32(0x8b0000a4) = 1u << 28;
    initialized = false;
    cpufreq_release();
}

/* ---- the public side ---------------------------------------------------- */

static unsigned idle_seconds;

void audio_power(bool on)
{
    enter();
    if (on) {
        idle_seconds = 0;               /* its user starts in a moment */
        codec_init();
    } else if (!fm_routed && !recording_on && !pcm_busy() && !hs_powered) {
        codec_close();
    }
    leave();
}

/* Nothing else powers the codec off after an app's sound (the app API has
 * audio_play_stop only) or a pause: on the phone (2026-11-08) a game with sound
 * left it on until power-off, its rails, bias and DAC, with the 104 MHz floor
 * it holds, so the screen dark all night stayed at 104 MHz. Waking it again
 * is what the next audio_play() does anyway. */
void audio_idle_tick(void)
{
    if (!initialized || fm_routed || recording_on || pcm_busy() || hs_powered) {
        idle_seconds = 0;
        return;
    }
    if (++idle_seconds >= 5) {
        idle_seconds = 0;
        klog("audio: unused for 5 s: codec off");
        audio_power(false);
    }
}

/* The headset check needs the mic bias, which needs the codec powered
 * (Rockbox keeps it on from boot; here it is on while a 4-pole headset's
 * buttons are in use). */
static void hs_power(bool on)
{
    if (on == hs_powered)
        return;
    if (on) {
        enter();
        hs_powered = true;
        codec_init();
        leave();
    } else {
        hs_powered = false;
        audio_power(false);
    }
}

void audio_set_rate(unsigned rate)
{
    enter();
    if (audio_rate_ok(rate))
        set_rate(rate);
    leave();
}

void audio_set_volume(int db10)
{
    enter();
    if (db10 < -1000) db10 = -1000;
    if (db10 > 240) db10 = 240;
    codec_db10 = db10;
    if (initialized) {
        bool change = !speaker_enabled;
        bool muted = (VBC_DP_CTL & CODEC_MUTE) == CODEC_MUTE;

        if (change)
            audio_mute(true);
        apply_volume();
        if (change && !muted)
            audio_mute(false);
    } else {
        apply_volume();
    }
    leave();
}

int audio_get_volume(void)
{
    return codec_db10;
}

bool audio_speaker(void)
{
    return speaker_enabled;
}

/* Follow the jack (the headset tick calls it). */
static void follow_jack(bool inserted)
{
    bool speaker = force_speaker || !inserted;

    enter();
    if (initialized && speaker_enabled != speaker)
        select_output(speaker);
    leave();
}

/* The speaker even with headphones plugged in (the FM radio: the headset
 * cable stays in as the antenna); off: the output follows the jack again. */
void audio_force_speaker(bool on)
{
    enter();
    force_speaker = on;
    if (initialized && speaker_enabled != (on || !audio_headphones()))
        select_output(on || !audio_headphones());
    leave();
}

bool audio_forced_speaker(void)
{
    return force_speaker;
}

/* ---- recording routes --------------------------------------------------- */

static int clamp15(int v)
{
    return v < 0 ? 0 : v > 15 ? 15 : v;
}

/* Stock maps the headset mic onto a lower gain table (NV RecordHeadset)
 * and its cable picks up digital noise: run it at 2/3. */
static int mic_gain_for(int path)
{
    return path == 0x0c ? (mic_gain * 2 + 1) / 3 : mic_gain;
}

void audio_set_mic_gain(int gain)
{
    enter();
    mic_gain = clamp15(gain);
    if (initialized && rec_mic_path)
        adi_update(CODEC + 0x70, 0xf0, (uint16_t)(mic_gain_for(rec_mic_path) << 4));
    leave();
}

int audio_get_mic_gain(void)
{
    return mic_gain;
}

#define LINE_MUX_STEREO  0xac
#define LINE_MUX_MONO    0xfc           /* both FM channels into both PGAs */

/* source AUDIO_SRC_MIC: the main or, with a 4-pole headset in, the headset
 * mic, mono on ADC L. AUDIO_SRC_FM: the line-in mixed mono into ADC L
 * (ADC R's modulator does not work on the SC6530C), as stock does. */
static void rec_route_on(int source)
{
    REG32(0x8b0001c4) |= 0x18;          /* both AD channels ARM-owned */
    REG32(VBC_BASE + 0x80) &= ~0x30u;   /* never to the DSP (zeros on HW) */
    set_rate(current_rate);             /* ADC rate before the ADC is on */
    rec_mic_path = 0;
    if (source == AUDIO_SRC_MIC) {
        adi_update(CODEC + 0x70, 0xf0, (uint16_t)(mic_gain << 4));
        mic_bias(BIAS_REC, true);
        rec_mic_path = 0xc0;
        headset_mic_raw = -2;
        if (audio_headphones()) {
            /* A 4-pole headset's mic sits on the biased mic line, a
             * 3-pole plug grounds it: read the line on ADC channel 1. */
            uint32_t a, b;

            adi_update(CODEC + 0x64, 0xcc, 0x0c);
            thread_sleep(20);
            a = adc_read(1);
            b = adc_read(1);
            headset_mic_raw = a == 0xffffffffu || b == 0xffffffffu ? -1
                              : (int)((a + b) / 2);
            if (headset_mic_raw > 40)
                rec_mic_path = 0x0c;
        }
        adi_update(CODEC + 0x64, 0xcc, rec_mic_path);
        adi_update(CODEC + 0x70, 0xf0, (uint16_t)(mic_gain_for(rec_mic_path) << 4));
        thread_sleep(20);
    }
    rec_fm_mix = source == AUDIO_SRC_FM;
    if (rec_fm_mix)
        adi_update(CODEC + 0x6c, 0xfc, LINE_MUX_MONO);
    /* ADC core, stock power state 5 (0x820d4 -> 0x8153e, 0x80af8) */
    adi_write(0x82001440, 1);
    adi_write(0x82001440, 2);
    adi_update(CODEC + 0x5c, 0x80, 0);
    adi_update(CODEC + 0x60, 0xa0, 0);
    adi_update(CODEC + 0xac, 0x40, 0);
    adi_update(CODEC + 0x5c, 0x02, 0);  /* ADC R stays off */
    REG32(0x8a002000) |= 0x02;          /* digital ADC L */
    adi_update(CODEC + 0x00, 0x0a, 0x02);
    adi_update(CODEC + 0x5c, 0x28, 0x08);       /* ADC L power-down off */
    REG32(VBC_BASE + 0x10) = (REG32(VBC_BASE + 0x10) & ~0xffu) | (VBC_FRAMES - 1);
    recording_on = true;
}

static void rec_route_off(void)
{
    if (!recording_on)
        return;
    adi_update(CODEC + 0x5c, 0x28, 0x28);
    adi_update(CODEC + 0x00, 0x0a, 0);
    REG32(0x8a002000) &= ~0x0au;
    adi_update(CODEC + 0xac, 0x40, 0x40);
    if (!fm_routed) {
        adi_update(CODEC + 0x5c, 0x80, 0x80);
        adi_update(CODEC + 0x60, 0xa0, 0xa0);
    }
    if (rec_mic_path) {
        adi_update(CODEC + 0x64, 0xcc, 0);
        mic_bias(BIAS_REC, false);
    }
    if (rec_fm_mix) {
        adi_update(CODEC + 0x6c, 0xfc, fm_routed ? LINE_MUX_STEREO : 0);
        rec_fm_mix = false;
    }
    REG32(0x8b0001c4) &= ~0x18u;
    REG32(VBC_BASE + 0x80) &= ~0x30u;
    recording_on = false;
}

int audio_record_route(int source)
{
    enter();
    if (!recording_on) {
        codec_init();
        rec_route_on(source);
    }
    leave();
    return rec_mic_path;
}

void audio_record_route_off(void)
{
    enter();
    rec_route_off();
    leave();
}

bool audio_recording(void)
{
    return recording_on;
}

int audio_mic_path(void)
{
    return rec_mic_path;
}

int audio_headset_mic_raw(void)
{
    return headset_mic_raw;
}

/* ---- FM line-in bypass -------------------------------------------------- */

/* AIL/AIR -> ADC PGAs -> PGA output buffers -> headphones or speaker, no
 * ADC/DAC data path (stock sprd_codec v0 LINEIN). */
void audio_fm_route(bool on)
{
    enter();
    if (on != fm_routed) {
        if (on) {
            codec_init();
            adi_update(CODEC + 0x5c, 0x80, 0);
            adi_update(CODEC + 0x60, 0xa0, 0);
            adi_update(CODEC + 0x70, 0xff, (uint16_t)(line_gain[0] << 4 | line_gain[1]));
            adi_update(CODEC + 0x6c, 0xfc, rec_fm_mix ? LINE_MUX_MONO : LINE_MUX_STEREO);
            thread_sleep(250);          /* stock line-in settle */
            fm_routed = true;
        } else {
            fm_routed = false;
            adi_update(CODEC + 0x6c, 0xfc, 0);
            if (!recording_on) {
                adi_update(CODEC + 0x60, 0xa0, 0xa0);
                adi_update(CODEC + 0x5c, 0x80, 0x80);
            }
        }
        select_output(speaker_enabled);
        audio_mute(!on && !pcm_busy());
    }
    leave();
}

/* ---- headset buttons ---------------------------------------------------- */

/* A headset's buttons sit on the mic line: the centre one shorts it (EIC
 * channel 4 sees that), the side ones pull it partway down, which only
 * the ADC (channel 1) tells apart. A 3-pole plug grounds the mic contact
 * and would read as held, so the buttons are enabled only after a check
 * at plug-in: 300 ms to settle, bias on, 100 ms, then the line must read
 * like a mic (> 40 raw, ~508 on the phone) with the button released.
 * Bands relative to the idle level (phone, 2026-11-06: idle 509, centre
 * 0, + 40, - ~86). A class must hold 40 ms over two samples (a side
 * button's contacts short the line for a moment on first touch). Off
 * while recording: a press would disturb the line being recorded.
 * Polled every 20 ms by the keypad's END thread. The codec is powered for
 * the check and stays on while the buttons are enabled. */
#define HOOK_MIC_MIN_RAW   40
#define HOOK_RELEASE_PM    500
#define HOOK_CENTER_PM     40
#define HOOK_UP_PM         125
#define HOOK_STABLE_MS     40

static enum { HS_OUT, HS_SETTLE, HS_BIAS, HS_READY } hs_state;
static uint32_t hs_when, cand_since;
static bool hook_enabled;
static int hook_idle, hook_raw = -1;
static unsigned cand, cand_n, hook_btn;
static volatile bool hs_idle;           /* screen dark */
static bool hs_quiet;                   /* buttons off for it: codec, bias off */

/* With the screen dark and nothing to hear, the buttons are not worth the
 * codec, its bias, the ADC reads and the 104 MHz floor the codec holds. They
 * come back with the screen, or as soon as something plays. */
void audio_headset_idle(bool idle)
{
    hs_idle = idle;
}

static int headset_line(void)
{
    uint32_t a = adc_read(1), b = adc_read(1);

    return a == 0xffffffffu || b == 0xffffffffu ? -1 : (int)((a + b) / 2);
}

static unsigned classify(int raw, bool comparator)
{
    int pm = hook_idle > 0 ? raw * 1000 / hook_idle : 1000;

    if (pm >= HOOK_RELEASE_PM)
        return comparator ? AUDIO_HS_CENTER : 0;
    if (pm < HOOK_CENTER_PM)
        return AUDIO_HS_CENTER;
    if (pm < HOOK_UP_PM)
        return AUDIO_HS_PLUS;
    return AUDIO_HS_MINUS;
}

unsigned audio_headset_tick(bool hook, uint32_t now)
{
    bool inserted = audio_headphones();

    if (!inserted) {
        if (hs_state != HS_OUT) {
            hs_state = HS_OUT;
            hook_enabled = false;
            hs_quiet = false;
            hook_btn = cand = cand_n = 0;
            mic_bias(BIAS_HOOK, false);
            follow_jack(false);
            hs_power(false);
        }
        return 0;
    }
    switch (hs_state) {
    case HS_OUT:
        hs_state = HS_SETTLE;
        hs_when = now + 300;
        follow_jack(true);
        break;
    case HS_SETTLE:
        if ((int32_t)(now - hs_when) < 0)
            break;
        hs_power(true);
        mic_bias(BIAS_HOOK, true);
        hs_state = HS_BIAS;
        hs_when = now + 100;
        break;
    case HS_BIAS:
        if ((int32_t)(now - hs_when) < 0)
            break;
        hook_raw = headset_line();
        hook_enabled = hook_raw > HOOK_MIC_MIN_RAW && !hook;
        hook_idle = hook_raw;
        if (!hook_enabled) {
            mic_bias(BIAS_HOOK, false);
            hs_power(false);            /* 3-pole: no buttons */
        }
        hs_state = HS_READY;
        break;
    case HS_READY:
        if (!hook_enabled)
            break;
        if (recording_on) {
            hook_btn = cand = cand_n = 0;
            break;
        }
        if (hs_idle && !fm_routed && !pcm_busy()) {
            if (!hs_quiet) {
                hs_quiet = true;
                hook_btn = cand = cand_n = 0;
                mic_bias(BIAS_HOOK, false);
                hs_power(false);
            }
            break;
        }
        if (hs_quiet) {                 /* bias on again: 100 ms to settle */
            hs_quiet = false;
            hs_power(true);
            mic_bias(BIAS_HOOK, true);
            hs_when = now + 100;
            break;
        }
        if ((int32_t)(now - hs_when) < 0)
            break;
        {
            unsigned c;

            hook_raw = headset_line();
            c = classify(hook_raw, hook);
            if (c != cand) {
                cand = c;
                cand_since = now;
                cand_n = 0;
            }
            if (cand_n < 2)
                cand_n++;
            if (cand_n >= 2 && now - cand_since >= HOOK_STABLE_MS)
                hook_btn = c;
            if (!c && !hook)
                hook_idle = (hook_idle * 7 + hook_raw) / 8;    /* drift */
        }
        break;
    }
    return hook_enabled ? hook_btn : 0;
}

void audio_headset_info(audio_headset_info_t *out)
{
    out->inserted = hs_state != HS_OUT;
    out->buttons = hook_enabled;
    out->idle = hook_idle;
    out->raw = hook_raw;
    out->held = hook_btn;
}
