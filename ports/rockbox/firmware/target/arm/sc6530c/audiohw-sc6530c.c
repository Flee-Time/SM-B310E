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
#include "button.h"
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
static uint16_t rec_mic_path;  /* codec +0x64 bits used by this recording */
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

/* Mic bias (codec +0x40 bit2) and its supply (ANA 0x82001164 bit 0x200,
 * active low), shared by recording and the headset button: the button
 * comparator (EIC channel 4) watches the biased mic line. On while any
 * user holds it. Callable from the tick: ADI accesses mask IRQs. */
#define BIAS_REC   1u
#define BIAS_HOOK  2u
static unsigned bias_users;

static void mic_bias(unsigned user, bool on)
{
    int old = disable_irq_save();
    unsigned before = bias_users;
    bias_users = on ? before | user : before & ~user;
    if (!before != !bias_users)
    {
        if (bias_users)
        {
            adi_update(0x82001164, 0x200, 0);   /* supply on */
            adi_update(CODEC + 0x40, 0x04, 0x04);
        }
        else
        {
            adi_update(CODEC + 0x40, 0x04, 0);
            adi_update(0x82001164, 0x200, 0x200);
        }
    }
    restore_irq(old);
}

/* Headset buttons. A headset's buttons sit on the mic line: the centre one
 * shorts it (the EIC channel 4 comparator, stock EIC 20, sees that one),
 * the side ones pull it partway down through a resistor, which only the
 * ADC can tell apart. A 3-pole plug grounds the mic contact and would read
 * as held all the time, so the buttons are enabled only after a check when
 * headphones are plugged in (or found at boot): 300 ms for the plug to
 * settle, mic bias on, 100 ms, then the mic line on ADC channel 1 must read
 * like a mic (> 40 raw; ~508 on the phone) and the button line must read
 * released. Otherwise the bias goes off again and the buttons stay
 * disabled until the next plug-in.
 *
 * Enabled, the tick samples the mic line every 20 ms (every tick while
 * the comparator is high) and classifies it against the idle level from
 * the check, which then tracks slow drift:
 *   >= HOOK_RELEASE_PERMILLE of idle  released (comparator high: centre)
 *   <  HOOK_CENTER_PERMILLE           centre       -> Play
 *   <  HOOK_UP_PERMILLE               lower level  -> + button
 *   otherwise                         higher level -> - button
 * Keys sent to Rockbox (hook_output): centre = Play while held. The side
 * buttons wait: released before HOOK_HOLD_TICKS, they send a short tap
 * of Next (+) or Previous (-), which the music screen skips on; held past
 * it, they hold Volume up (+) or Volume down (-) until released, and
 * Rockbox's key repeat steps the volume.
 * A class must hold for HOOK_STABLE_TICKS (and two samples) before it is
 * reported: on the phone a side button paused playback, most likely its
 * contacts shorting the line for a moment on first touch, which the old
 * two-samples-10-ms rule took for the centre button. While a press is a
 * candidate the line is sampled every tick. 40 ms still covers the bounce
 * and lets a quick tap register within ~60 ms. Bands from the phone
 * (2026-11-06):
 * idle 509 raw; centre 0, volume up 40 (7.9%), volume down ~86 (17%),
 * from the press log with + pressed three times, then - three times.
 * Boundaries sit halfway between the readings. The I/O screen shows the
 * live raw value and the dump logs each press's raw value.
 * While recording from the headset mic the buttons are off: any press
 * shorts or pulls down the mic being recorded, the ADC would sample that
 * line, and the centre button (Play) would start a new file in the
 * recording screen. Recording anything else (FM, or the phone's mic) keeps
 * all of them. Runs from the 100 Hz button tick (sc6530_headset_tick). */
#define HOOK_MIC_MIN_RAW   40
#define HOOK_POLL_TICKS    2
#define HOOK_RELEASE_PERMILLE  500
#define HOOK_CENTER_PERMILLE   40
#define HOOK_UP_PERMILLE       125
#define HOOK_STABLE_TICKS      (HZ * 4 / 100)   /* 40 ms */
#define HOOK_HOLD_TICKS        (HZ * 4 / 10)    /* side: tap vs hold */
#define HOOK_TAP_TICKS         (HZ * 6 / 100)   /* Next/Prev tap length */
#define HOOK_LOG               6
bool headphones_inserted(void);
static enum { HS_OUT, HS_SETTLE, HS_BIAS, HS_READY } hs_state;
static long hs_when;
static bool hook_enabled;
static int hook_check_raw = -1;
static bool hook_check_level, hook_level_now;
static int hook_idle, hook_raw = -1, hook_press_raw = -1;
static unsigned hook_poll, hook_cand, hook_cand_n, hook_btn;
static long hook_cand_since;
/* Side-button press in progress (BUTTON_VOL_UP/DOWN class), whether it
 * became a hold, and a pending Next/Previous tap. */
static unsigned hook_side, hook_tap;
static long hook_side_since, hook_tap_until;
static bool hook_side_held;
static unsigned hook_taps[2], hook_holds[2];    /* +, - */
static unsigned hook_muted;     /* ticks with the buttons off (recording) */
static unsigned hook_presses[3];        /* play, volume up, volume down */
/* Last presses, oldest first once full: class and raw ADC value. */
static char hook_log_cls[HOOK_LOG];
static int16_t hook_log_raw[HOOK_LOG];
static unsigned hook_log_n;

/* Recording from the headset mic (codec +0x64 bits 0x0c): buttons off. */
static bool hook_headset_recording(void)
{
#ifdef HAVE_RECORDING
    return recording_on && rec_mic_path == 0x0c;
#else
    return false;
#endif
}

static unsigned hook_classify(int raw, bool comparator)
{
    int pm = hook_idle > 0 ? raw * 1000 / hook_idle : 1000;

    if (pm >= HOOK_RELEASE_PERMILLE)
        return comparator ? BUTTON_PLAY : 0;
    if (pm < HOOK_CENTER_PERMILLE)
        return BUTTON_PLAY;
    if (pm < HOOK_UP_PERMILLE)
        return BUTTON_VOL_UP;
    return BUTTON_VOL_DOWN;
}

static void hook_set(unsigned btn, int raw)
{
    if (btn && btn != hook_btn)
    {
        hook_presses[btn == BUTTON_PLAY ? 0 : btn == BUTTON_VOL_UP ? 1 : 2]++;
        hook_press_raw = raw;
        hook_log_cls[hook_log_n % HOOK_LOG] =
            btn == BUTTON_PLAY ? 'C' : btn == BUTTON_VOL_UP ? '+' : '-';
        hook_log_raw[hook_log_n % HOOK_LOG] = raw;
        hook_log_n++;
    }
    hook_btn = btn;
}

static void hook_sample(bool hook)
{
    unsigned cls;

    if (hook_headset_recording())
    {
        /* Drop any press in progress without sending a tap. */
        hook_btn = hook_cand = hook_cand_n = 0;
        hook_side = hook_tap = 0;
        hook_poll = 0;
        hook_muted++;
        return;
    }
    /* Every 50 ms at rest; every tick while the comparator is high or a
     * class other than the one reported is settling. */
    if (!hook && hook_cand == hook_btn && ++hook_poll < HOOK_POLL_TICKS)
        return;
    hook_poll = 0;
    hook_raw = sc6530_adc_headset_raw();
    cls = hook_classify(hook_raw, hook);
    if (cls != hook_cand)
    {
        hook_cand = cls;
        hook_cand_since = current_tick;
        hook_cand_n = 0;
    }
    if (hook_cand_n < 2)
        hook_cand_n++;
    if (hook_cand_n >= 2 &&
        current_tick - hook_cand_since >= HOOK_STABLE_TICKS)
        hook_set(cls, hook_raw);
    if (!cls && !hook)
        hook_idle = (hook_idle * 7 + hook_raw) / 8;     /* slow drift */
}

/* Map the classified button to the keys Rockbox sees (see above). */
static unsigned hook_output(void)
{
    unsigned out;

    if (hook_btn == BUTTON_VOL_UP || hook_btn == BUTTON_VOL_DOWN)
    {
        if (hook_side != hook_btn)
        {
            hook_side = hook_btn;
            hook_side_since = current_tick;
            hook_side_held = false;
        }
        if (!hook_side_held &&
            current_tick - hook_side_since >= HOOK_HOLD_TICKS)
        {
            hook_side_held = true;
            hook_holds[hook_side == BUTTON_VOL_UP ? 0 : 1]++;
        }
        return hook_side_held ? hook_side : 0;
    }
    if (hook_side)                      /* side button released */
    {
        if (!hook_side_held)
        {
            hook_tap = hook_side == BUTTON_VOL_UP ? BUTTON_NEXT : BUTTON_PREV;
            hook_tap_until = current_tick + HOOK_TAP_TICKS;
            hook_taps[hook_side == BUTTON_VOL_UP ? 0 : 1]++;
        }
        hook_side = 0;
    }
    out = hook_btn;                     /* centre: Play, or nothing */
    if (hook_tap)
    {
        if (TIME_BEFORE(current_tick, hook_tap_until))
            out |= hook_tap;
        else
            hook_tap = 0;
    }
    return out;
}

unsigned sc6530_headset_tick(bool hook)
{
    hook_level_now = hook;
    if (!headphones_inserted())
    {
        if (hs_state != HS_OUT)
        {
            hs_state = HS_OUT;
            hook_enabled = false;
            hook_btn = hook_cand = hook_cand_n = 0;
            hook_side = hook_tap = 0;
            mic_bias(BIAS_HOOK, false);
        }
        return 0;
    }
    switch (hs_state)
    {
    case HS_OUT:
        hs_state = HS_SETTLE;
        hs_when = current_tick + HZ * 3 / 10;
        break;
    case HS_SETTLE:
        if (!initialized || TIME_BEFORE(current_tick, hs_when))
            break;
        mic_bias(BIAS_HOOK, true);
        hs_state = HS_BIAS;
        hs_when = current_tick + HZ / 10;
        break;
    case HS_BIAS:
        if (TIME_BEFORE(current_tick, hs_when))
            break;
        hook_check_raw = sc6530_adc_headset_raw();
        hook_check_level = hook;
        hook_enabled = hook_check_raw > HOOK_MIC_MIN_RAW && !hook;
        hook_idle = hook_raw = hook_check_raw;
        hook_poll = 0;
        if (!hook_enabled)
            mic_bias(BIAS_HOOK, false);
        hs_state = HS_READY;
        break;
    case HS_READY:
        if (hook_enabled)
            hook_sample(hook);
        break;
    }
    return hook_enabled ? hook_output() : 0;
}

void sc6530_headset_debug(struct sc6530_headset_info *info)
{
    info->enabled = hook_enabled;
    info->check_raw = hook_check_raw;
    info->check_level = hook_check_level;
    info->level = hook_level_now;
    info->state = hs_state;
    info->idle = hook_idle;
    info->raw = hook_raw;
    info->press_raw = hook_press_raw;
    info->button = hook_btn == BUTTON_PLAY ? 'C' : hook_btn == BUTTON_VOL_UP ? '+' :
                   hook_btn == BUTTON_VOL_DOWN ? '-' : ' ';
    for (unsigned i = 0; i < 3; i++)
        info->presses[i] = hook_presses[i];
    info->muted = hook_muted;
    for (unsigned i = 0; i < 2; i++)
    {
        info->taps[i] = hook_taps[i];
        info->holds[i] = hook_holds[i];
    }
    info->log_n = hook_log_n < HOOK_LOG ? hook_log_n : HOOK_LOG;
    for (unsigned i = 0; i < info->log_n; i++)
    {
        unsigned slot = (hook_log_n - info->log_n + i) % HOOK_LOG;
        info->log_cls[i] = hook_log_cls[slot];
        info->log_raw[i] = hook_log_raw[slot];
    }
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

static bool rec_stereo;        /* line-in (FM): ADC L and R; mic: ADC L */
/* Headset mic line, raw ADC channel 1: about 0 with a 3-pole plug (mic
 * contact grounded). The threshold is a first guess, to be calibrated
 * from the dump's mic_adc readings. -1 = ADC failed, -2 = not measured. */
#define HEADSET_MIC_MIN_RAW 40
static int headset_mic_raw = -2;

/* ADC R power-up for stereo (FM) capture. Stock never powers ADC R (its
 * FM recording is mono from ADC L), and with the L recipe mirrored the
 * phone's ADC R sits at the negative rail. Variants, picked from Debug >
 * View I/O ports (key 7) until hardware settles it; applied at the next
 * stereo recording start:
 *   0: +0x5c bit1 clear, bit6 untouched (the build that railed)
 *   1: clear bit6 (likely ADC R's twin of the bit7 core power-down that
 *      fixed ADC L), set bit1 (the decompile's "R on" bit)
 *   2: clear bit6, bit1 clear
 *   3: as 1, plus a pulse on bit2 (a possible ADC R reset) */
static int adcr_variant = 1;

int sc6530_adcr_variant(void)
{
    return adcr_variant;
}

void sc6530_set_adcr_variant(int variant)
{
    adcr_variant = variant & 3;
}

/* Stock maps the headset mic onto the same gain register but with a lower
 * table (about 2..10 against 14 for the main mic, NV RecordHeadset), and
 * the headset cable picks up digital noise; run it at 2/3 of the setting. */
static int mic_gain_for_path(int path)
{
    return path == 0x0c ? (mic_gain * 2 + 1) / 3 : mic_gain;
}

int sc6530_headset_mic_raw(void)
{
    return headset_mic_raw;
}

/* Recording from the headset mic (codec +0x64 bits 0x0c). */
bool sc6530_headset_mic_recording(void)
{
    return recording_on && rec_mic_path == 0x0c;
}

int sc6530_rec_mic_path(void)
{
    return rec_mic_path;
}
static int line_gain[2] = { DEFAULT_REC_LEFT_GAIN, DEFAULT_REC_RIGHT_GAIN };

static int clamp_gain(int value)
{
    return value < 0 ? 0 : value > 15 ? 15 : value;
}

void audiohw_set_recvol(int left, int right, int type)
{
    if (type == AUDIO_GAIN_MIC)
    {
        mic_gain = clamp_gain(left);
        if (initialized && rec_mic_path)
            adi_update(CODEC + 0x70, 0xf0, mic_gain_for_path(rec_mic_path) << 4);
    }
    else if (type == AUDIO_GAIN_LINEIN)
    {
        line_gain[0] = clamp_gain(left);
        line_gain[1] = clamp_gain(right);
        if (initialized && fm_on)
            adi_update(CODEC + 0x70, 0xff, line_gain[0] << 4 | line_gain[1]);
    }
}

/* Line-in PGA gain for the FM route. */
static uint16_t line_pga_gain(void)
{
    return line_gain[0] << 4 | line_gain[1];
}


/* Line-in mux +0x6c (stock 0x80bcc): bit7 AIL->PGA L, bit6 AIR->PGA L,
 * bit4 AIL->PGA R, bit3 AIR->PGA R, bits 5/2 the PGA inputs' common mode.
 * Stereo 0xac; stock's mono FM recording 0xe0 (both into PGA L). */
#define LINE_MUX_STEREO  0xac
#define LINE_MUX_MONO    0xfc   /* both channels into both PGAs */
static bool rec_fm_mix;         /* FM recording switched the mux to mono */

/* source_mic: main or headset microphone, mono on ADC L. Otherwise the
 * line-in (FM): ADC R's analog modulator does not work on the SC6530C
 * (2026-11-06 sweep, docs/rockbox-recording.md), so FM is recorded as
 * stock does: both FM channels mixed into PGA L and captured mono on
 * ADC L. The mix also goes to PGA R, so the headphones carry the same
 * mono signal while recording; the stereo mux returns afterwards.
 * stereo (the ADC R sweep only): line-in on ADC L and R as routed.
 * Mic choice follows the jack: stock's headset-mic path 0x20 (+0x64 bits
 * 0x0c) with headphones in, the main mic (path 8, bits 0xc0) otherwise. */
static void rec_power_up(bool source_mic, bool stereo)
{
    if (!initialized)
        audiohw_init();
    rec_stereo = stereo;
    sc6530_rec_set_stereo(rec_stereo);
    /* Stock capture DAI modes are 0x30xxxxxx: both AD channels are
     * ARM-owned and enabled. */
    SC_AUDIO_REG(0x8b0001c4) |= 0x18;
    SC_AUDIO_REG(SC_VBC_BASE + 0x80) =
        (SC_AUDIO_REG(SC_VBC_BASE + 0x80) & ~0x30u) | (vbc_ad_route ? 0x30u : 0);
    audiohw_set_frequency(current_fsel);      /* ADC fs before ADC enable */
    rec_mic_path = 0;
    if (source_mic)
    {
        adi_update(CODEC + 0x70, 0xf0, mic_gain << 4);
        mic_bias(BIAS_REC, true);             /* mic bias + supply */
        rec_mic_path = 0xc0;                  /* phone's main mic */
        headset_mic_raw = -2;                 /* not measured */
        if (headphones_inserted())
        {
            /* A 4-pole headset's mic sits on the biased mic line; a
             * 3-pole plug grounds that contact. Stock reads the line on
             * ADC channel 1 (ADC_CheckHeadsetStatus); it also treats
             * < 121 mV as the in-line button being held. */
            adi_update(CODEC + 0x64, 0xcc, 0x0c);
            udelay(20000);
            headset_mic_raw = sc6530_adc_headset_raw();
            if (headset_mic_raw > HEADSET_MIC_MIN_RAW)
                rec_mic_path = 0x0c;          /* headset mic */
        }
        adi_update(CODEC + 0x64, 0xcc, rec_mic_path);
        adi_update(CODEC + 0x70, 0xf0, mic_gain_for_path(rec_mic_path) << 4);
        udelay(20000);                        /* stock has no wait */
    }
    rec_fm_mix = !source_mic && !stereo;
    if (rec_fm_mix)
        adi_update(CODEC + 0x6c, 0xfc, LINE_MUX_MONO);
    /* ADC core, stock power state 5 (0x820d4 -> 0x8153e, 0x80af8). Stock
     * will not enter the ADC state (0x82146) without it; skipping it left
     * the ADC pinned at full scale on hardware. */
    adi_write(0x82001440, 1);
    adi_write(0x82001440, 2);
    adi_update(CODEC + 0x5c, 0x80, 0);
    adi_update(CODEC + 0x60, 0xa0, 0);
    adi_update(CODEC + 0xac, 0x40, 0);
    /* ADC power, stock 0x80b4e(1, mask). The decompile suggested +0x5c
     * bit1 is set when the right channel is on, but on hardware ADC R then
     * sat at the negative rail while ADC L (bit1 clear) worked, so it
     * stays clear. */
    adi_update(CODEC + 0x5c, 0x02, 0);
    if (rec_stereo)
    {
        if (adcr_variant != 0)
            adi_update(CODEC + 0x5c, 0x40, 0);
        if (adcr_variant == 1 || adcr_variant == 3)
            adi_update(CODEC + 0x5c, 0x02, 0x02);
        if (adcr_variant == 3)
        {
            adi_update(CODEC + 0x5c, 0x04, 0x04);
            udelay(1000);
            adi_update(CODEC + 0x5c, 0x04, 0);
        }
    }
    SC_AUDIO_REG(0x8a002000) |= rec_stereo ? 0x0a : 0x02;  /* digital ADC */
    adi_update(CODEC + 0x00, 0x0a, rec_stereo ? 0x0a : 0x02);
    adi_update(CODEC + 0x5c, 0x28, rec_stereo ? 0 : 0x08); /* ADC PD off */
    SC_AUDIO_REG(SC_VBC_BASE + 0x10) =
        (SC_AUDIO_REG(SC_VBC_BASE + 0x10) & ~0xffu) | (SC_VBC_FRAMES - 1);
    rec_read(rec_snap);
    recording_on = true;
}

void audiohw_enable_recording(bool source_mic)
{
    if (recording_on)
        return;
    rec_power_up(source_mic, false);
}

void audiohw_disable_recording(void)
{
    if (!recording_on)
        return;
    adi_update(CODEC + 0x5c, 0x28, 0x28);
    if (rec_stereo)
        adi_update(CODEC + 0x5c, 0x42, 0x40);  /* back to the reset state */
    adi_update(CODEC + 0x00, 0x0a, 0);
    SC_AUDIO_REG(0x8a002000) &= ~0x0au;
    adi_update(CODEC + 0xac, 0x40, 0x40);     /* ADC core off */
    if (!fm_on)
    {
        /* The FM route keeps the ADC input buffer and PGAs powered. */
        adi_update(CODEC + 0x5c, 0x80, 0x80);
        adi_update(CODEC + 0x60, 0xa0, 0xa0);
    }
    if (rec_mic_path)
    {
        adi_update(CODEC + 0x64, 0xcc, 0);
        mic_bias(BIAS_REC, false);
    }
    if (rec_fm_mix)
    {
        adi_update(CODEC + 0x6c, 0xfc, fm_on ? LINE_MUX_STEREO : 0);
        rec_fm_mix = false;
    }
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
        adi_update(CODEC + 0x70, 0xff, line_pga_gain()); /* default 0x88 */
        adi_update(CODEC + 0x6c, 0xfc,           /* AIL->L, AIR->R, VCM */
                   rec_fm_mix ? LINE_MUX_MONO : LINE_MUX_STEREO);
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

#if defined(HAVE_RECORDING) && CONFIG_TUNER
/* ADC R bit sweep (Debug > View I/O ports, key 0). Stock never records in
 * stereo, and with every right-channel bit its codec helpers know set
 * (docs/rockbox-recording.md) ADC R still delivers a constant -32768. The
 * sweep powers stereo capture on the line-in route, then flips one bit at
 * a time in the codec's ADC-side registers, the digital codec and the VBC
 * routing register, measures both channels for 100 ms and restores the
 * bit. A live ADC shows noise even on a quiet input; a dead one stays at
 * the rail. Output registers (+0x74 and up, speaker) are left alone. */
static bool sweep_is_adi(uint32_t addr)
{
    return (addr & 0xfffff000u) == 0x82001000u;
}

static uint16_t sweep_read(uint32_t addr)
{
    return sweep_is_adi(addr) ? adi_read(addr) : (uint16_t)SC_AUDIO_REG(addr);
}

static void sweep_update(uint32_t addr, uint16_t mask, uint16_t value)
{
    if (sweep_is_adi(addr))
        adi_update(addr, mask, value);
    else
        SC_AUDIO_REG(addr) = (SC_AUDIO_REG(addr) & ~(uint32_t)mask) | value;
}

static void sweep_measure(struct sc6530_sweep *r)
{
    int16_t min[2], max[2];
    unsigned banks;

    sleep(HZ / 20);                             /* settle, then drop */
    sc6530_rec_probe_take(min, max, &banks);
    sleep(HZ / 10);
    sc6530_rec_probe_take(r->min, r->max, &r->banks);
}

static void sweep_one(struct sc6530_sweep *r, uint32_t addr,
                      uint16_t mask, uint16_t value)
{
    uint16_t old = sweep_read(addr) & mask;

    r->addr = addr;
    r->mask = mask;
    r->value = value;
    sweep_update(addr, mask, value);
    sweep_measure(r);
    sweep_update(addr, mask, old);
    sleep(HZ / 50);
}

static void sweep_bits(struct sc6530_sweep *out, int *n, int max,
                       uint32_t addr, unsigned first, unsigned last)
{
    for (unsigned b = first; b <= last && *n < max; b++)
    {
        uint16_t bit = 1u << b;
        sweep_one(&out[(*n)++], addr, bit, (sweep_read(addr) & bit) ^ bit);
    }
}

int sc6530_adcr_sweep(struct sc6530_sweep *out, int max)
{
    static const uint8_t codec_regs[] = {
        0x40, 0x44, 0x48, 0x4c, 0x50, 0x54, 0x58,
        0x5c, 0x60, 0x64, 0x68, 0x6c, 0xac,
    };
    static const struct { uint32_t addr; uint16_t mask, value; } special[] = {
        { CODEC + 0x6c, 0xfc, 0x1c },   /* stock mux: right only */
        { CODEC + 0x6c, 0xfc, 0x74 },   /* stock mux: both, mask bit 4 */
        { CODEC + 0x6c, 0xfc, 0xe0 },   /* stock mux: left only (mono mix) */
        { CODEC + 0x5c, 0x42, 0x40 },   /* ADC R variant 0 */
        { CODEC + 0x5c, 0x42, 0x00 },   /* ADC R variant 2 */
    };
    bool route = fm_on;
    int n = 0;

    if (recording_on || max < 1)
        return -1;
    uint32_t adc_ctl = SC_AUDIO_REG(0x8a002018);

    if (!route)
        audiohw_fm_route(true);
    rec_power_up(false, true);                  /* stereo, line-in */
    /* The playback default (44.1 kHz) is no ADC rate (rate / 4000), so
     * the field may still be 0 if nothing recorded since boot: use 48k. */
    SC_AUDIO_REG(0x8a002018) = (adc_ctl & ~15u) | 48000 / 4000;
    sc6530_rec_probe_start();
    sleep(HZ / 5);

    out[n].addr = 0;
    out[n].mask = out[n].value = 0;
    sweep_measure(&out[n++]);
    for (unsigned i = 0; i < ARRAYLEN(codec_regs); i++)
        sweep_bits(out, &n, max, CODEC + codec_regs[i], 0, 7);
    sweep_bits(out, &n, max, 0x8a002000u, 0, 7);        /* digital codec */
    sweep_bits(out, &n, max, 0x8a002018u, 4, 7);        /* above ADC fs */
    sweep_bits(out, &n, max, SC_VBC_BASE + 0x80, 0, 7); /* VBC AD routing */
    for (unsigned i = 0; i < ARRAYLEN(special) && n < max; i++)
        sweep_one(&out[n++], special[i].addr, special[i].mask, special[i].value);

    sc6530_rec_probe_stop();
    SC_AUDIO_REG(0x8a002018) = adc_ctl;
    audiohw_disable_recording();
    if (!route)
        audiohw_fm_route(false);
    return n;
}
#endif

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
    adi_update(0x82001164, 0x3f0, 0x3f0);     /* incl. mic bias supply */
    bias_users = 0;
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
