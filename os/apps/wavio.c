/* B310E OS — WAV playback and recording through a ring buffer (wavio.h).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/app.h"
#include "os/audio.h"
#include "os/fs.h"
#include "os/kernel.h"
#include "os/lib.h"
#include "os/media.h"
#include "os/ui.h"
#include "apps/wavio.h"

#define RING_BYTES  (128u * 1024)       /* 0.7 s of 44.1 kHz stereo */
#define RING_MASK   (RING_BYTES - 1)

static uint8_t *ring;
static volatile uint32_t head, tail;    /* bytes; head written, tail read */
static FIL file;
static bool file_open, playing, recording, tone, file_eof, write_failed;
static uint32_t synced_at;
#define SYNC_MS 30000u
static unsigned channels, rate;
static uint32_t data_bytes, total_bytes, dropped;
static uint32_t tone_phase, tone_step;
static char name[64], last_rec[64];

static uint32_t used(void)
{
    return head - tail;
}

static bool ring_alloc(void)
{
    if (!ring)
        ring = kmalloc(RING_BYTES);
    head = tail = 0;
    return ring != NULL;
}

/* ---- playback ----------------------------------------------------------- */

/* sine in Q15, one quarter wave in 64 steps */
static int16_t sine(uint32_t phase)
{
    static const int16_t q[65] = {
        0, 804, 1608, 2410, 3212, 4011, 4808, 5602, 6393, 7179, 7962, 8739,
        9512, 10278, 11039, 11793, 12539, 13279, 14010, 14732, 15446, 16151,
        16846, 17530, 18204, 18868, 19519, 20159, 20787, 21403, 22005, 22594,
        23170, 23731, 24279, 24811, 25329, 25832, 26319, 26790, 27245, 27683,
        28105, 28510, 28898, 29268, 29621, 29956, 30273, 30571, 30852, 31113,
        31356, 31580, 31785, 31971, 32137, 32285, 32412, 32521, 32609, 32678,
        32728, 32757, 32767,
    };
    uint32_t i = phase >> 24;           /* 256 steps a cycle */
    uint32_t k = i & 63;

    switch (i >> 6) {
    case 0: return q[k];
    case 1: return q[64 - k];
    case 2: return (int16_t)-q[k];
    default: return (int16_t)-q[64 - k];
    }
}

static unsigned fill(int16_t *lr, unsigned frames, void *ctx)
{
    unsigned n = 0;

    if (tone) {
        for (; n < frames; n++) {
            int16_t v = (int16_t)(sine(tone_phase) / 4);   /* -12 dB */

            tone_phase += tone_step;
            lr[2 * n] = lr[2 * n + 1] = v;
        }
        total_bytes += frames * 4;
        return n;
    }
    for (; n < frames; n++) {
        unsigned bytes = channels * 2;
        int16_t l, r;

        if (used() < bytes)
            break;
        l = (int16_t)(ring[tail & RING_MASK] | ring[(tail + 1) & RING_MASK] << 8);
        if (channels == 2)
            r = (int16_t)(ring[(tail + 2) & RING_MASK] | ring[(tail + 3) & RING_MASK] << 8);
        else
            r = l;
        tail += bytes;
        lr[2 * n] = l;
        lr[2 * n + 1] = r;
    }
    total_bytes += n * channels * 2;
    if (!n && !file_eof)                /* a gap: play silence meanwhile */
        for (; n < frames; n++)
            lr[2 * n] = lr[2 * n + 1] = 0;
    return n;
}

static uint32_t le32(const uint8_t *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static void refill(void)
{
    while (file_open && !file_eof && RING_BYTES - used() >= 4096) {
        uint8_t buf[4096];
        UINT got = 0;
        uint32_t want = data_bytes < sizeof(buf) ? data_bytes : sizeof(buf);

        if (!want || f_read(&file, buf, want, &got) != FR_OK || !got) {
            file_eof = true;
            break;
        }
        data_bytes -= got;
        for (UINT i = 0; i < got; i++)
            ring[(head + i) & RING_MASK] = buf[i];
        head += got;
    }
}

const char *wav_play_file(const char *path)
{
    uint8_t h[12], ck[8], fmt[16];
    UINT n;

    wav_stop();
    if (!ring_alloc())
        return "not enough memory";
    if (f_open(&file, path, FA_READ) != FR_OK)
        return "cannot open";
    file_open = true;
    if (f_read(&file, h, 12, &n) != FR_OK || n != 12 || memcmp(h, "RIFF", 4) ||
        memcmp(h + 8, "WAVE", 4))
        goto bad;
    channels = 0;
    for (;;) {                          /* find "fmt " and "data" */
        uint32_t len;

        if (f_read(&file, ck, 8, &n) != FR_OK || n != 8)
            goto bad;
        len = le32(ck + 4);
        if (!memcmp(ck, "fmt ", 4)) {
            if (len < 16 || f_read(&file, fmt, 16, &n) != FR_OK || n != 16)
                goto bad;
            if ((fmt[0] | fmt[1] << 8) != 1 || (fmt[14] | fmt[15] << 8) != 16)
                goto unsupported;
            channels = fmt[2] | fmt[3] << 8;
            rate = le32(fmt + 4);
            /* a length past the file could wrap, and read here for ever */
            if (len < 16 || len - 16 > f_size(&file) - f_tell(&file))
                goto unsupported;
            f_lseek(&file, f_tell(&file) + len - 16 + (len & 1));
        } else if (!memcmp(ck, "data", 4)) {
            data_bytes = len;
            break;
        } else {
            if (len > f_size(&file) - f_tell(&file))
                goto unsupported;
            f_lseek(&file, f_tell(&file) + len + (len & 1));
        }
    }
    if ((channels != 1 && channels != 2) || !audio_rate_ok(rate))
        goto unsupported;
    file_eof = false;
    tone = false;
    total_bytes = 0;
    snprintf(name, sizeof(name), "%s", path);
    refill();
    volume_use(app_volume_cat());       /* the Recorder's own level */
    if (audio_play(rate, fill, NULL)) {
        wav_stop();
        return "audio did not start";
    }
    playing = true;
    klog("audio: playing %s, %u Hz, %u ch, %s", path, rate, channels,
         audio_speaker() ? "speaker" : "headphones");
    return NULL;
bad:
    wav_stop();
    return "not a WAV file";
unsupported:
    wav_stop();
    return "only 16-bit PCM at 8-96 kHz";
}

const char *wav_play_tone(unsigned hz)
{
    wav_stop();
    tone = true;
    channels = 2;
    rate = 44100;
    tone_phase = 0;
    tone_step = (uint32_t)((uint64_t)hz * 0x100000000ull / rate);
    total_bytes = 0;
    snprintf(name, sizeof(name), "%u Hz tone", hz);
    if (audio_play(rate, fill, NULL))
        return "audio did not start";
    playing = true;
    return NULL;
}

/* ---- recording ---------------------------------------------------------- */

static void sink(const int16_t *mono, unsigned frames, void *ctx)
{
    if (RING_BYTES - used() < frames * 2) {
        dropped += frames;
        return;
    }
    for (unsigned i = 0; i < frames; i++) {
        ring[head & RING_MASK] = (uint8_t)mono[i];
        ring[(head + 1) & RING_MASK] = (uint8_t)(mono[i] >> 8);
        head += 2;
    }
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void header(uint8_t h[44], uint32_t bytes)
{
    memcpy(h, "RIFF\0\0\0\0WAVEfmt \x10\0\0\0\x01\0\x01\0", 24);
    put32(h + 4, 36 + bytes);
    put32(h + 24, rate);
    put32(h + 28, rate * 2);
    h[32] = 2; h[33] = 0; h[34] = 16; h[35] = 0;
    memcpy(h + 36, "data", 4);
    put32(h + 40, bytes);
}

static void drain(void)
{
    uint8_t buf[4096];

    while (file_open && used() >= (recording ? sizeof(buf) : 1)) {
        uint32_t n = used() < sizeof(buf) ? used() : sizeof(buf);
        UINT w;

        for (uint32_t i = 0; i < n; i++)
            buf[i] = ring[(tail + i) & RING_MASK];
        tail += n;
        if (f_write(&file, buf, n, &w) != FR_OK || w != n) {
            klog("audio: recording write failed");
            write_failed = true;
            break;
        }
        total_bytes += n;
    }
}

const char *wav_record(unsigned r, int source)
{
    uint8_t h[44];
    UINT w;
    FILINFO fi;

    if (media_state() == MEDIA_PLAYING || media_state() == MEDIA_LOADING)  /* one rate for
                                               both: it would play at ours */
        media_pause(true);
    wav_stop();
    if (!ring_alloc())
        return "not enough memory";
    f_mkdir(REC_DIR);
    for (int i = 1; i < 10000; i++) {
        snprintf(name, sizeof(name), REC_DIR "/REC%04d.WAV", i);
        if (f_stat(name, &fi) != FR_OK)
            break;
    }
    if (f_open(&file, name, FA_WRITE | FA_CREATE_NEW) != FR_OK)
        return "cannot create the file";
    file_open = true;
    rate = r;
    channels = 1;
    total_bytes = dropped = 0;
    header(h, 0);
    f_write(&file, h, sizeof(h), &w);
    if (audio_record(rate, source, sink, NULL)) {
        wav_stop();
        return "recording did not start";
    }
    recording = true;
    write_failed = false;
    synced_at = kernel_ticks();
    klog("audio: recording %s, %u Hz, %s", name, rate,
         source == AUDIO_SRC_FM ? "FM" : audio_mic_path() == 0x0c ? "headset mic" : "phone mic");
    return NULL;
}

/* ---- both --------------------------------------------------------------- */

void wav_pump(void)
{
    if (playing && !tone)
        refill();
    if (recording)
        drain();
    if (recording && audio_record_failed()) {
        klog("audio: recording stopped by a DMA error");
        wav_stop();                     /* what came is kept */
    }
    if (recording && write_failed) {    /* a full card, or a bad one */
        wav_stop();
        ui_wake();
        ui_message("Recording stopped", "The card is full or failed. What was recorded is kept.");
    }
    /* The directory and the header follow every so often, so that a battery pulled
     * or a crash costs half a minute, not the whole recording */
    if (recording && kernel_ticks() - synced_at >= SYNC_MS) {
        uint8_t h[44];
        UINT w;
        FSIZE_t at = f_tell(&file);

        synced_at = kernel_ticks();
        header(h, total_bytes);
        f_lseek(&file, 0);
        f_write(&file, h, sizeof(h), &w);
        f_lseek(&file, at);
        f_sync(&file);
    }
    if (playing && !audio_playing())
        wav_stop();                     /* played to the end */
}

void wav_stop(void)
{
    if (playing) {
        audio_play_stop();
        playing = false;
        klog("audio: stopped after %lu s", (unsigned long)wav_seconds());
    }
    if (recording) {
        uint8_t h[44];
        UINT w;

        audio_record_stop();
        recording = false;
        drain();                        /* the rest, partial blocks too */
        header(h, total_bytes);
        f_lseek(&file, 0);
        f_write(&file, h, sizeof(h), &w);
        snprintf(last_rec, sizeof(last_rec), "%s", name);
        klog("audio: recorded %lu s to %s, %lu samples dropped",
             (unsigned long)wav_seconds(), name, (unsigned long)dropped);
    }
    if (file_open) {
        f_close(&file);
        file_open = false;
    }
    tone = false;
    head = tail = 0;
    if (!fm_is_on())
        audio_power(false);
}

bool wav_playing(void)
{
    return playing;
}

bool wav_recording(void)
{
    return recording;
}

uint32_t wav_seconds(void)
{
    uint32_t per_s = rate * (channels ? channels : 1) * 2;

    return per_s ? total_bytes / per_s : 0;
}

const char *wav_name(void)
{
    return name;
}

uint32_t wav_dropped(void)
{
    return dropped;
}

const char *wav_last_recording(void)
{
    return last_rec;
}
