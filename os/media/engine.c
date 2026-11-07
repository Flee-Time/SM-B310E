/* B310E OS — the playback engine (os/media.h).
 *
 * A decoder thread, one priority below the UI's, runs Rockbox's decoders
 * from codec modules (os/codec.h, media/module.c) and writes 16-bit stereo
 * frames into a ring of 64K frames (1.49 s at 44.1 kHz). The audio DMA
 * interrupt only copies from the ring (fill()); when the ring has room
 * for what the decoder wants to write, the interrupt posts `room`, so
 * the decoder works one codec frame at a time as the audio drains and
 * its load spreads evenly over the governor's 50 ms windows (decoding
 * the ring full in bursts made the clock jump between 104 and 208 MHz).
 * Playback starts when 0.3 s are buffered.
 *
 * The codec_api the decoder gets:
 *   - file data: a window of the track's file (64 KB, grown to what a
 *     codec asks for, up to 512 KB) read through FatFs in the decoder
 *     thread: request_buffer, advance_buffer, read_filebuf, seek_buffer;
 *   - PCM: pcmbuf_insert converts the codec's samples (16-bit, or 32-bit
 *     with DSP_SET_SAMPLE_DEPTH fraction bits; interleaved, two planes or
 *     mono) to rounded, clipped 16-bit stereo, the only "DSP" there is.
 *     Rates the audio codec has no clock for are resampled linearly to
 *     44.1 or 48 kHz;
 *   - commands: get_command returns HALT for stop or a new track and
 *     SEEK_TIME for a seek; a blocked pcmbuf_insert returns early when
 *     one is waiting, so it is seen within one codec frame.
 *
 * Pause stops the DMA and keeps the ring. Another user of the audio (the
 * Sound screen's tone, the alarm) stopping our DMA or replacing our fill
 * callback ends the track as MEDIA_STOPPED.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/arch.h"
#include "os/audio.h"
#include "os/codec.h"
#include "os/fs.h"
#include "os/kernel.h"
#include "os/lib.h"
#include "os/media.h"
#include "media/media_internal.h"

#define RING_FRAMES     65536u
#define RING_MASK       (RING_FRAMES - 1)
#define PREBUFFER_MS    300
#define FBUF_MIN        (64u * 1024)
#define FBUF_MAX        (512u * 1024)
#define STACK_BYTES     (32u * 1024)
#define MAX_CODECS      16
#define CHUNK           256             /* frames converted at a time */
#define STOLEN_MS       500             /* our fill not called: someone else plays */
#define RESCAN_MS       5000
#define UNLOAD_MS       30000           /* idle: the codec module is freed */

/* What the QEMU tests read (tests/test-codecs.py). */
typedef struct {
    uint32_t state;
    uint32_t seq;                       /* tracks asked for */
    uint32_t position_ms;
    uint32_t length_ms;
    uint32_t rate, out_rate;
    uint32_t underruns;
    uint32_t decoded_ms;
    uint32_t decode_cycles;             /* 26 MHz cycles of the decoder thread */
    uint32_t wall_cycles;               /* 26 MHz cycles since the track started */
    uint32_t ring_frames;
    uint32_t cpu_mhz;
} media_status_t;
volatile media_status_t media_status;

/* ---- state shared with the UI ------------------------------------------- */
static thread_t *thread;
static mutex_t lock;
static sem_t wake, room;
static bool ready;

static char req_path[160];
static volatile bool req_play, req_stop;
static volatile int32_t req_seek = -1;
static volatile bool paused;
static uint32_t seq, track_seq;         /* requests; the one the thread runs */
static volatile media_state_t state = MEDIA_IDLE;
static media_info_t info;
static bool info_valid;
static const char *error_text = "";

static codec_entry_t codecs[MAX_CODECS];
static int n_codecs = -1;
static uint32_t scanned_at;
static char ext_list[MAX_CODECS * 24];

/* ---- the ring (thread writes head, interrupt reads tail) ---------------- */
static uint32_t *ring;                  /* L | R << 16 */
static volatile uint32_t head, tail;
static volatile uint32_t want_room;     /* frames the thread waits for; 0: none */
static volatile uint32_t underruns, played, last_fill;
static volatile bool codec_done;        /* nothing more comes: end at the empty ring */
static volatile bool audio_on;          /* our DMA runs */
static volatile bool stolen;
static uint32_t base_ms;                /* the position when played was 0 */
static volatile int32_t seek_target = -1;

/* ---- the decoder's side -------------------------------------------------- */
static struct codec_api api;
static codec_t *codec;                  /* the loaded module */
static bool codec_started;              /* entry(CODEC_LOAD) done */
static uint8_t *arena;
static FIL file;
static bool file_open;
static uint8_t *fbuf;
static size_t fcap, flen;
static long fpos, fread_pos;            /* file offset of fbuf[0], of the next read */
static unsigned sample_depth = 16, stereo = STEREO_NONINTERLEAVED;
static uint32_t in_rate, out_rate;
static uint32_t frames_in, last_elapsed;
static uint32_t rs_step, rs_phase;      /* resampler, Q16 */
static int32_t rs_prev[2];
static uint32_t cycles0, wall0, status_at;

static bool start(void);

static void barrier(void)
{
    __asm__ volatile("" ::: "memory");
}

/* ---- the DMA interrupt ------------------------------------------------------ */
static unsigned fill(int16_t *lr, unsigned frames, void *ctx)
{
    uint32_t t = tail, avail = head - t, n = avail < frames ? avail : frames;

    last_fill = kernel_ticks();
    for (uint32_t i = 0; i < n; i++) {
        uint32_t v = ring[(t + i) & RING_MASK];

        lr[2 * i] = (int16_t)v;
        lr[2 * i + 1] = (int16_t)(v >> 16);
    }
    tail = t + n;
    played += n;
    if (want_room && RING_FRAMES - (head - tail) >= want_room) {
        want_room = 0;
        sem_post(&room);
    }
    if (n)
        return n;
    if (codec_done)
        return 0;                       /* the end: the DMA drains and stops */
    underruns++;
    memset(lr, 0, frames * 4);
    return frames;
}

/* ---- audio on and off (under lock) ---------------------------------------- */
static void audio_start_locked(void)
{
    if (audio_on || paused || !out_rate || stolen)
        return;
    last_fill = kernel_ticks();
    volume_use(VOL_MUSIC);              /* the music's own level */
    if (audio_play(out_rate, fill, NULL) == 0) {
        audio_on = true;
        if (state == MEDIA_LOADING && track_seq == seq)
            state = MEDIA_PLAYING;
    }
}

static void audio_stop_locked(void)
{
    if (audio_on) {
        audio_play_stop();
        audio_on = false;
    }
}

static void ring_reset(void)
{
    head = tail = 0;
    played = 0;
    want_room = 0;
    rs_phase = 0;
    rs_prev[0] = rs_prev[1] = 0;
}

/* Our DMA stopped without reaching the end, or someone else's fill runs. */
static bool audio_taken(void)
{
    if (stolen)
        return true;
    if (!audio_on || paused)
        return false;
    if (!audio_playing())
        stolen = !(codec_done && head == tail);
    else {
        /* last_fill first: the DMA interrupt's fill() can set it between
         * the two reads, a tick after "now", and the unsigned difference
         * of that was 2^32 - 1 ms: a playing track "taken" (test-codecs
         * and test-art, about one run in ten) */
        uint32_t fill_at = last_fill;

        stolen = (int32_t)(kernel_ticks() - fill_at) > STOLEN_MS;
    }
    if (stolen) {
        extern volatile int pcm_stop_why;
        extern volatile uint32_t pcm_stop_status;

        klog("media: the audio was taken by something else (pcm %s, status %08lx, %s)",
             audio_playing() ? "running" : pcm_stop_why == 2 ? "DMA error" :
             pcm_stop_why == 3 ? "drained" : pcm_stop_why == 1 ? "stopped" : "never ran",
             (unsigned long)pcm_stop_status,
             audio_playing() ? "no fill for 500 ms" : "not playing");
    }
    return stolen;
}

static bool aborting(void)
{
    return req_stop || req_play || req_seek >= 0 || audio_taken();
}

static void set_state(media_state_t s)
{
    mutex_lock(&lock, WAIT_FOREVER);
    if (track_seq == seq)
        state = s;
    mutex_unlock(&lock);
}

/* ---- PCM in ------------------------------------------------------------------ */
static uint32_t prebuffer_frames(void)
{
    return out_rate * PREBUFFER_MS / 1000;
}

/* Put n frames into the ring, waiting for room; false if a command came. */
static bool ring_put(const uint32_t *frames, uint32_t n)
{
    while (RING_FRAMES - (head - tail) < n) {
        if (aborting())
            return false;
        if (!audio_on && !paused) {     /* a full ring that does not play */
            mutex_lock(&lock, WAIT_FOREVER);
            audio_start_locked();
            mutex_unlock(&lock);
        }
        want_room = n;
        sem_wait(&room, 100);
    }
    for (uint32_t i = 0, h = head; i < n; i++)
        ring[(h + i) & RING_MASK] = frames[i];
    barrier();
    head += n;
    if (kernel_ticks() - status_at >= 100) {   /* for the tests and the System screen */
        media_stats_t st;

        status_at = kernel_ticks();
        media_stats(&st);
    }
    if (!audio_on && !paused && head - tail >= prebuffer_frames()) {
        mutex_lock(&lock, WAIT_FOREVER);
        audio_start_locked();
        mutex_unlock(&lock);
    }
    return true;
}

static inline int32_t to16(int32_t v, int shift)
{
    if (shift > 0)
        v = (v + (1 << (shift - 1))) >> shift;
    return v > 32767 ? 32767 : v < -32768 ? -32768 : v;
}

static void pcmbuf_insert(const void *ch1, const void *ch2, int count)
{
    uint32_t out[CHUNK * 2 + 4];
    int shift = sample_depth > 16 ? (int)sample_depth - 15 : 0;
    bool wide = sample_depth > 16;
    int i = 0;

    frames_in += (uint32_t)count;
    while (i < count) {
        uint32_t n = 0;

        for (; i < count && n < CHUNK * 2; i++) {
            int32_t l, r;

            if (stereo == STEREO_INTERLEAVED) {
                l = wide ? ((const int32_t *)ch1)[2 * i] : ((const int16_t *)ch1)[2 * i];
                r = wide ? ((const int32_t *)ch1)[2 * i + 1] : ((const int16_t *)ch1)[2 * i + 1];
            } else if (stereo == STEREO_NONINTERLEAVED) {
                l = wide ? ((const int32_t *)ch1)[i] : ((const int16_t *)ch1)[i];
                r = wide ? ((const int32_t *)ch2)[i] : ((const int16_t *)ch2)[i];
            } else {
                l = r = wide ? ((const int32_t *)ch1)[i] : ((const int16_t *)ch1)[i];
            }
            l = to16(l, shift);
            r = to16(r, shift);
            if (!rs_step) {
                out[n++] = (uint16_t)l | (uint32_t)r << 16;
                continue;
            }
            /* linear interpolation between the last frame and this one */
            while (rs_phase < 65536) {
                int32_t a = rs_prev[0] + (int32_t)(((int64_t)(l - rs_prev[0]) * rs_phase) >> 16);
                int32_t b = rs_prev[1] + (int32_t)(((int64_t)(r - rs_prev[1]) * rs_phase) >> 16);

                out[n++] = (uint16_t)a | (uint32_t)b << 16;
                rs_phase += rs_step;
            }
            rs_phase -= 65536;
            rs_prev[0] = l;
            rs_prev[1] = r;
        }
        if (n && !ring_put(out, n))
            return;                     /* a command: get_command() tells the codec */
    }
}

/* ---- rates ------------------------------------------------------------------ */
static void set_rate(uint32_t rate)
{
    uint32_t out = rate;

    if (!rate || rate == in_rate)
        return;
    if (!audio_rate_ok(rate))
        out = rate % 11025 == 0 ? 44100 : 48000;
    if (out != out_rate && audio_on) {
        /* a new rate mid-stream: let the old one play out first */
        while (head != tail && !aborting())
            thread_sleep(10);
        mutex_lock(&lock, WAIT_FOREVER);
        audio_stop_locked();
        mutex_unlock(&lock);
    }
    in_rate = rate;
    out_rate = out;
    rs_step = out == rate ? 0 : (uint32_t)(((uint64_t)rate << 16) / out);
    rs_phase = 0;
    mutex_lock(&lock, WAIT_FOREVER);
    info.rate = rate;
    info.out_rate = out;
    mutex_unlock(&lock);
    media_status.rate = rate;
    media_status.out_rate = out;
}

static void configure(int setting, intptr_t value)
{
    switch (setting) {
    case DSP_SET_FREQUENCY:
        set_rate((uint32_t)value);
        break;
    case DSP_SET_SAMPLE_DEPTH:
        sample_depth = (unsigned)value;
        break;
    case DSP_SET_STEREO_MODE:
        stereo = (unsigned)value;
        break;
    default:
        break;
    }
}

/* ---- file data ----------------------------------------------------------- */
static bool fbuf_grow(size_t need)
{
    size_t cap = fcap ? fcap : FBUF_MIN;
    uint8_t *n;

    while (cap < need)
        cap *= 2;
    if (cap > FBUF_MAX)
        return false;
    if (cap == fcap && fbuf)
        return true;
    n = kmalloc(cap);
    if (!n)
        return false;
    if (fbuf) {
        memcpy(n, fbuf, flen);
        kfree(fbuf);
    }
    fbuf = n;
    fcap = cap;
    return true;
}

/* Make [curpos, curpos + want) readable (as far as the file goes). */
static void fbuf_fill(size_t want)
{
    long pos = api.curpos, end = api.filesize;
    UINT got = 0;

    if (want > (size_t)(end - pos))
        want = pos < end ? (size_t)(end - pos) : 0;
    if (pos >= fpos && pos + (long)want <= fpos + (long)flen)
        return;
    if (want > fcap && !fbuf_grow(want))
        want = fcap;
    if (pos >= fpos && pos < fpos + (long)flen) {
        size_t keep = (size_t)(fpos + (long)flen - pos);

        memmove(fbuf, fbuf + (pos - fpos), keep);
        flen = keep;
    } else {
        flen = 0;
    }
    fpos = pos;
    if (fread_pos != fpos + (long)flen) {
        if (f_lseek(&file, (FSIZE_t)(fpos + (long)flen)) != FR_OK)
            return;
        fread_pos = fpos + (long)flen;
    }
    if (f_read(&file, fbuf + flen, (UINT)(fcap - flen), &got) != FR_OK)
        got = 0;
    flen += got;
    fread_pos += got;
}

static void *request_buffer(size_t *realsize, size_t reqsize)
{
    long avail;

    fbuf_fill(reqsize);
    avail = fpos + (long)flen - api.curpos;
    if (avail > api.filesize - api.curpos)
        avail = api.filesize - api.curpos;
    if (avail < 0)
        avail = 0;
    *realsize = (size_t)avail < reqsize ? (size_t)avail : reqsize;
    return *realsize ? fbuf + (api.curpos - fpos) : NULL;
}

static void advance_buffer(size_t amount)
{
    api.curpos += (long)amount;
    if (api.curpos > api.filesize)
        api.curpos = api.filesize;
}

static size_t read_filebuf(void *ptr, size_t size)
{
    size_t done = 0;

    while (done < size) {
        size_t n, want = size - done;
        void *p = request_buffer(&n, want < fcap ? want : fcap);

        if (!n)
            break;
        memcpy((uint8_t *)ptr + done, p, n);
        advance_buffer(n);
        done += n;
    }
    return done;
}

static bool seek_buffer(size_t newpos)
{
    if ((long)newpos > api.filesize)
        return false;
    api.curpos = (long)newpos;
    return true;
}

static void set_offset(size_t value)
{
}

static void strip_filesize(long value)
{
    if (value > 0 && value < api.filesize)
        api.filesize = value;
}

/* the parser's view: plain reads on the open file */
static long fd_read(int fd, void *buf, size_t n)
{
    UINT got = 0;

    if (f_read(&file, buf, (UINT)n, &got) != FR_OK)
        return -1;
    return (long)got;
}

static long fd_lseek(int fd, long offset, int whence)
{
    long pos = whence == 1 ? (long)f_tell(&file) + offset :
               whence == 2 ? (long)f_size(&file) + offset : offset;

    if (pos < 0 || f_lseek(&file, (FSIZE_t)pos) != FR_OK)
        return -1;
    return pos;
}

static long fd_size(int fd)
{
    return (long)f_size(&file);
}

/* ---- commands and time ------------------------------------------------------ */
static long get_command(intptr_t *param)
{
    if (req_stop || req_play || audio_taken())
        return CODEC_ACTION_HALT;
    if (req_seek >= 0) {
        *param = req_seek;
        seek_target = req_seek;
        req_seek = -1;
        return CODEC_ACTION_SEEK_TIME;
    }
    return CODEC_ACTION_NULL;
}

static void set_elapsed(unsigned long ms)
{
    last_elapsed = (uint32_t)ms;
}

static void seek_complete(void)
{
    mutex_lock(&lock, WAIT_FOREVER);
    audio_stop_locked();
    ring_reset();
    base_ms = last_elapsed;
    seek_target = -1;
    codec_done = false;
    mutex_unlock(&lock);
}

static bool loop_track(void)
{
    return false;
}

static void *codec_get_buffer(size_t *size)
{
    uint32_t want = codec ? codec->mod->arena_size : 0;

    if (!arena && want)
        arena = kmalloc(want);
    *size = arena ? want : 0;
    return arena;
}

static unsigned rb_sleep(unsigned ticks)
{
    thread_sleep(ticks * 10);
    return 0;
}

static void rb_yield(void)
{
    thread_yield();
}

static void commit_dcache(void)
{
    dcache_clean_all();
}

static void commit_idcache(void)
{
    dcache_clean_all();
    icache_invalidate_all();
}

/* ---- the C library the codecs get -------------------------------------- */
static char *rb_strcpy(char *dst, const char *src)
{
    char *d = dst;

    while ((*d++ = *src++))
        ;
    return dst;
}

static char *rb_strcat(char *dst, const char *src)
{
    rb_strcpy(dst + strlen(dst), src);
    return dst;
}

static void *rb_memchr(const void *s, int c, size_t n)
{
    const uint8_t *p = s;

    for (; n; n--, p++)
        if (*p == (uint8_t)c)
            return (void *)p;
    return NULL;
}

static void swap_bytes(uint8_t *a, uint8_t *b, size_t size)
{
    while (size--) {
        uint8_t t = *a;

        *a++ = *b;
        *b++ = t;
    }
}

/* heapsort: no recursion, no extra memory */
static void rb_qsort(void *base, size_t nmemb, size_t size,
                     int (*cmp)(const void *, const void *))
{
    uint8_t *b = base;

    if (nmemb < 2)
        return;
    for (size_t start = nmemb / 2; start-- > 0; ) {
        for (size_t root = start, child; (child = 2 * root + 1) < nmemb; root = child) {
            if (child + 1 < nmemb && cmp(b + child * size, b + (child + 1) * size) < 0)
                child++;
            if (cmp(b + root * size, b + child * size) >= 0)
                break;
            swap_bytes(b + root * size, b + child * size, size);
        }
    }
    for (size_t end = nmemb - 1; end > 0; end--) {
        swap_bytes(b, b + end * size, size);
        for (size_t root = 0, child; (child = 2 * root + 1) < end; root = child) {
            if (child + 1 < end && cmp(b + child * size, b + (child + 1) * size) < 0)
                child++;
            if (cmp(b + root * size, b + child * size) >= 0)
                break;
            swap_bytes(b + root * size, b + child * size, size);
        }
    }
}

static void rb_debugf(const char *fmt, ...)
{
}

static void rb_panicf(const char *fmt, ...)
{
    char buf[96];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    klog("media: codec panic: %s", buf);
}

static void api_init(void)
{
    api = (struct codec_api){
        .codec_get_buffer = codec_get_buffer, .pcmbuf_insert = pcmbuf_insert,
        .set_elapsed = set_elapsed, .read_filebuf = read_filebuf,
        .request_buffer = request_buffer, .advance_buffer = advance_buffer,
        .seek_buffer = seek_buffer, .seek_complete = seek_complete,
        .set_offset = set_offset, .configure = configure,
        .get_command = get_command, .loop_track = loop_track,
        .strip_filesize = strip_filesize, .sleep = rb_sleep, .yield = rb_yield,
        .commit_dcache = commit_dcache, .commit_discard_dcache = commit_dcache,
        .commit_discard_idcache = commit_idcache,
        .strcpy = rb_strcpy, .strlen = strlen, .strcmp = strcmp,
        .strcat = rb_strcat, .memset = memset, .memcpy = memcpy,
        .memmove = memmove, .memcmp = memcmp, .memchr = rb_memchr,
        .debugf = rb_debugf, .logf = rb_debugf, .qsort = rb_qsort,
        .panicf = rb_panicf,
        .fd_read = fd_read, .fd_lseek = fd_lseek, .fd_size = fd_size,
        .vsnprintf = vsnprintf, .klog = klog,
    };
}

/* ---- codecs on the card ------------------------------------------------------ */
static bool has_word(const char *list, const char *word)
{
    size_t n = strlen(word);

    for (const char *p = list; *p; ) {
        const char *e = p;

        while (*e && *e != ' ')
            e++;
        if ((size_t)(e - p) == n && !memcmp(p, word, n))
            return true;
        p = *e ? e + 1 : e;
    }
    return false;
}

/* under lock */
static void scan_locked(bool force)
{
    if (n_codecs >= 0 && !force && kernel_ticks() - scanned_at < RESCAN_MS)
        return;
    n_codecs = codec_scan(codecs, MAX_CODECS);
    scanned_at = kernel_ticks();
    ext_list[0] = 0;
    for (int i = 0; i < n_codecs; i++) {
        size_t l = strlen(ext_list);

        snprintf(ext_list + l, sizeof(ext_list) - l, "%s%s", l ? " " : "", codecs[i].exts);
    }
}

static void lower_ext(const char *path, char *ext, size_t n)
{
    const char *dot = strrchr(path, '.'), *slash = strrchr(path, '/');
    size_t i = 0;

    ext[0] = 0;
    if (!dot || (slash && dot < slash))
        return;
    for (dot++; *dot && i + 1 < n; dot++)
        ext[i++] = *dot >= 'A' && *dot <= 'Z' ? (char)(*dot + 'a' - 'A') : *dot;
    ext[i] = 0;
}

bool media_supported(const char *path)
{
    char ext[8];
    bool ok = false;

    lower_ext(path, ext, sizeof(ext));
    if (!ext[0] || !start())
        return false;
    mutex_lock(&lock, WAIT_FOREVER);
    scan_locked(false);
    for (int i = 0; i < n_codecs && !ok; i++)
        ok = has_word(codecs[i].exts, ext);
    mutex_unlock(&lock);
    return ok;
}

const char *media_extensions(void)
{
    if (!start())
        return "";
    mutex_lock(&lock, WAIT_FOREVER);
    scan_locked(false);
    mutex_unlock(&lock);
    return ext_list;
}

/* The format by the first bytes, as an extension, for files named oddly;
 * hint: the module to try first among those for it. */
static void sniff(char *ext, size_t n, char *hint, size_t hn)
{
    uint8_t h[40];
    UINT got = 0;

    hint[0] = 0;
    f_lseek(&file, 0);
    if (f_read(&file, h, sizeof(h), &got) != FR_OK || got < 12)
        return;
    if (!memcmp(h, "fLaC", 4))
        snprintf(ext, n, "flac");
    else if (!memcmp(h, "OggS", 4) && got >= 36) {
        snprintf(ext, n, "ogg");
        snprintf(hint, hn, "%s", !memcmp(h + 28, "OpusHead", 8) ? "opus" : "vorbis");
    } else if (!memcmp(h, "RIFF", 4) && !memcmp(h + 8, "WAVE", 4))
        snprintf(ext, n, "wav");
    else if (!memcmp(h, "wvpk", 4))
        snprintf(ext, n, "wv");
    else if (!memcmp(h + 4, "ftyp", 4))
        snprintf(ext, n, "m4a");
    else if (!memcmp(h, "ID3", 3) || (h[0] == 0xff && (h[1] & 0xe0) == 0xe0))
        snprintf(ext, n, "mp3");
    f_lseek(&file, 0);
}

/* ---- one track ------------------------------------------------------------- */
static void unload_codec(void)
{
    if (!codec)
        return;
    if (codec_started)
        codec->mod->entry(CODEC_UNLOAD);
    codec_started = false;
    codec_unload(codec);
    codec = NULL;
    kfree(arena);
    arena = NULL;
}

static bool try_codec(const char *name, codec_track_t *t)
{
    if (!codec || strcmp(codec->name, name)) {
        unload_codec();
        if (codec_load(name, &codec))
            return false;
    }
    memset(t, 0, sizeof(*t));
    return codec->mod->probe(&api, 0, t) == 0;
}

static const char *find_codec(const char *path, codec_track_t *t)
{
    char ext[8], sext[8], hint[16];
    codec_entry_t list[MAX_CODECS];
    int n, order[MAX_CODECS], k = 0;

    lower_ext(path, ext, sizeof(ext));
    sext[0] = 0;
    sniff(sext, sizeof(sext), hint, sizeof(hint));
    mutex_lock(&lock, WAIT_FOREVER);
    scan_locked(false);
    n = n_codecs;
    memcpy(list, codecs, sizeof(codecs[0]) * (size_t)(n > 0 ? n : 0));
    mutex_unlock(&lock);
    /* the sniffed module first, then the extension's, then the content's */
    for (int pass = 0; pass < 3; pass++)
        for (int i = 0; i < n; i++) {
            bool want = pass == 0 ? hint[0] && !strcmp(list[i].name, hint) :
                        pass == 1 ? has_word(list[i].exts, ext) :
                        sext[0] && has_word(list[i].exts, sext);
            bool dup = false;

            for (int j = 0; j < k; j++)
                dup |= order[j] == i;
            if (want && !dup)
                order[k++] = i;
        }
    if (!k)
        return n ? "no codec for this file" : "no codecs in " CODEC_DIR;
    if (codec)                          /* the loaded one first: no reload */
        for (int j = 1; j < k; j++)
            if (!strcmp(list[order[j]].name, codec->name) &&
                has_word(list[order[j]].exts, ext)) {
                int x = order[j];

                memmove(order + 1, order, sizeof(order[0]) * (size_t)j);
                order[0] = x;
            }
    for (int j = 0; j < k; j++) {
        if (req_play || req_stop)
            return "";
        if (try_codec(list[order[j]].name, t))
            return NULL;
    }
    return "not a file the codecs can play";
}

static void drain_track(void)
{
    /* the decoder is done; wait while the ring plays out, a seek runs it
     * again from there */
    for (;;) {
        if (req_stop || req_play || audio_taken())
            return;
        if (req_seek >= 0) {
            mutex_lock(&lock, WAIT_FOREVER);
            audio_stop_locked();
            ring_reset();
            base_ms = (uint32_t)req_seek;
            codec_done = false;
            mutex_unlock(&lock);
            if (codec->mod->start_at)
                codec->mod->start_at((unsigned long)req_seek);
            req_seek = -1;
            api.curpos = 0;
            frames_in = (uint32_t)((uint64_t)base_ms * in_rate / 1000);
            codec->mod->run();
            codec_done = true;
            continue;
        }
        if (!audio_on && !paused && head != tail) {
            mutex_lock(&lock, WAIT_FOREVER);
            audio_start_locked();
            mutex_unlock(&lock);
        }
        if (audio_on && !audio_playing() && head == tail)
            return;                     /* played out */
        if (!audio_on && !paused && head == tail)
            return;                     /* nothing was decoded */
        sem_wait(&room, 50);
    }
}

/* the decoder stack's high-water mark (thread_create fills it with a pattern) */
static size_t stack_used(const thread_t *t)
{
    size_t i = 0, words = t->stack_size / 4;

    while (i < words && t->stack[i] == 0x57a57a57u)
        i++;
    return (words - i) * 4;
}

static void play_track(const char *path)
{
    codec_track_t t;
    const char *err;
    enum codec_status st;
    thread_t *self = thread_self();

    memset(&t, 0, sizeof(t));           /* a 1.0 module leaves the art fields */

    if (f_open(&file, path, FA_READ) != FR_OK) {
        mutex_lock(&lock, WAIT_FOREVER);
        if (track_seq == seq) {
            state = MEDIA_ERROR;
            error_text = "cannot open";
        }
        mutex_unlock(&lock);
        return;
    }
    file_open = true;
    api.filesize = (long)f_size(&file);
    api.curpos = 0;
    err = find_codec(path, &t);
    if (!err && !codec_started) {
        /* the format a codec sets at CODEC_LOAD holds for all its tracks */
        sample_depth = 16;
        stereo = STEREO_NONINTERLEAVED;
        if (codec->mod->entry(CODEC_LOAD) != CODEC_OK)
            err = "the codec did not start";
        codec_started = !err;
    }
    if (err) {
        mutex_lock(&lock, WAIT_FOREVER);
        if (track_seq == seq && *err) {
            state = MEDIA_ERROR;
            error_text = err;
            klog("media: %s: %s", path, err);
        }
        mutex_unlock(&lock);
        f_close(&file);
        file_open = false;
        return;
    }

    mutex_lock(&lock, WAIT_FOREVER);
    snprintf(info.title, sizeof(info.title), "%s", t.title);
    snprintf(info.artist, sizeof(info.artist), "%s", t.artist);
    snprintf(info.album, sizeof(info.album), "%s", t.album);
    snprintf(info.codec, sizeof(info.codec), "%s", t.codec);
    info.length_ms = t.length_ms;
    info.bitrate = t.bitrate;
    info.art_type = t.art_type;
    info.art_flags = t.art_flags;
    info.art_pos = t.art_pos;
    info.art_size = t.art_size;
    info_valid = track_seq == seq;
    ring_reset();
    base_ms = 0;
    seek_target = -1;
    codec_done = false;
    stolen = false;
    underruns = 0;
    mutex_unlock(&lock);

    /* the file window starts fresh after the parser's reads */
    api.curpos = 0;
    api.filesize = (long)f_size(&file);
    fpos = 0;
    flen = 0;
    fread_pos = -1;
    if (!fbuf_grow(FBUF_MIN)) {
        set_state(MEDIA_ERROR);
        error_text = "not enough memory";
        f_close(&file);
        file_open = false;
        return;
    }
    in_rate = out_rate = rs_step = 0;
    frames_in = last_elapsed = 0;
    set_rate(t.frequency);
    media_status.length_ms = t.length_ms;
    cycles0 = self->cycles;
    wall0 = kernel_cycles();
    klog("media: playing %s (%s, %lu Hz, %lu kbit/s, %lu s) with %s: \"%s\" by \"%s\"",
         path, t.codec, (unsigned long)t.frequency, (unsigned long)t.bitrate,
         (unsigned long)(t.length_ms / 1000), codec->name, t.title, t.artist);

    st = codec->mod->run();
    codec_done = true;
    if (!(req_stop || req_play))
        drain_track();

    mutex_lock(&lock, WAIT_FOREVER);
    audio_stop_locked();
    if (track_seq == seq) {
        if (stolen)
            state = MEDIA_STOPPED;
        else if (req_stop || req_play)
            ;                           /* media_stop() / media_play() set it */
        else if (st != CODEC_OK && !frames_in) {
            state = MEDIA_ERROR;
            error_text = "decoding failed";
        } else
            state = MEDIA_ENDED;
    }
    mutex_unlock(&lock);
    {
        uint32_t cyc = self->cycles - cycles0, wall = kernel_cycles() - wall0;
        uint32_t ms = in_rate ? (uint32_t)((uint64_t)frames_in * 1000 / in_rate) : 0;

        klog("media: %s %s after %lu ms of audio: decoder %lu ms CPU in %lu ms (%lu.%lu%%), "
             "%lu underruns, %d MHz, stack %lu of %lu bytes used", path,
             st == CODEC_OK ? "done" : frames_in ? "done (the codec ended with an error)" :
             "failed", (unsigned long)ms,
             (unsigned long)(cyc / 26000), (unsigned long)(wall / 26000),
             (unsigned long)(wall ? (uint64_t)cyc * 1000 / wall / 10 : 0),
             (unsigned long)(wall ? (uint64_t)cyc * 1000 / wall % 10 : 0),
             (unsigned long)underruns, clock_mhz(), (unsigned long)stack_used(self),
             (unsigned long)self->stack_size);
    }
    f_close(&file);
    file_open = false;
    if (!req_play)
        audio_power(false);             /* only when nothing else uses it */
}

static void decoder_main(void *arg)
{
    char path[sizeof(req_path)];
    uint32_t idle_since = kernel_ticks();

    for (;;) {
        bool go;

        if (sem_wait(&wake, 1000) != WAIT_OK) {
            /* free the codec (and its arena) a while after the music */
            if (codec && kernel_ticks() - idle_since > UNLOAD_MS)
                unload_codec();
            continue;
        }
        mutex_lock(&lock, WAIT_FOREVER);
        go = req_play;
        if (go) {
            memcpy(path, req_path, sizeof(path));
            track_seq = seq;
            req_play = false;
            req_stop = false;
        }
        if (req_stop) {
            req_stop = false;
            req_seek = -1;
        }
        mutex_unlock(&lock);
        if (go)
            play_track(path);
        idle_since = kernel_ticks();
    }
}

static bool start(void)
{
    if (ready)
        return true;
    if (!ring)
        ring = kmalloc(RING_FRAMES * sizeof(uint32_t));
    if (!ring)
        return false;
    mutex_init(&lock);
    sem_init(&wake, 0);
    sem_init(&room, 0);
    api_init();
    thread = thread_create("decoder", PRIO_NORMAL - 1, STACK_BYTES, decoder_main, NULL);
    if (!thread)
        return false;
    ready = true;
    return true;
}

/* ---- the UI's side --------------------------------------------------------- */
int media_play(const char *path)
{
    if (!start())
        return -1;
    mutex_lock(&lock, WAIT_FOREVER);
    snprintf(req_path, sizeof(req_path), "%s", path);
    seq++;
    req_play = true;
    req_seek = -1;
    paused = false;
    audio_stop_locked();
    state = MEDIA_LOADING;
    info_valid = false;
    memset(&info, 0, sizeof(info));
    snprintf(info.path, sizeof(info.path), "%s", path);
    error_text = "";
    media_status.seq = seq;
    mutex_unlock(&lock);
    sem_post(&wake);
    sem_post(&room);
    return 0;
}

void media_stop(void)
{
    if (!ready)
        return;
    mutex_lock(&lock, WAIT_FOREVER);
    seq++;
    req_stop = true;
    req_play = false;
    paused = false;
    audio_stop_locked();
    state = MEDIA_IDLE;
    info_valid = false;
    mutex_unlock(&lock);
    sem_post(&wake);
    sem_post(&room);
}

void media_pause(bool pause)
{
    if (!ready)
        return;
    mutex_lock(&lock, WAIT_FOREVER);
    if (pause && !paused && (state == MEDIA_PLAYING || state == MEDIA_LOADING)) {
        paused = true;
        audio_stop_locked();
        state = MEDIA_PAUSED;
    } else if (!pause && paused) {
        paused = false;
        state = audio_on ? MEDIA_PLAYING : head != tail ? MEDIA_PLAYING : MEDIA_LOADING;
        if (head != tail)
            audio_start_locked();
    }
    klog("media: %s, now %d (ring %lu frames)", pause ? "pause" : "resume", (int)state,
         (unsigned long)(head - tail));
    mutex_unlock(&lock);
    sem_post(&room);
}

void media_seek(uint32_t ms)
{
    if (!ready)
        return;
    mutex_lock(&lock, WAIT_FOREVER);
    if (state == MEDIA_PLAYING || state == MEDIA_PAUSED || state == MEDIA_LOADING) {
        if (info.length_ms && ms >= info.length_ms)
            ms = info.length_ms > 1000 ? info.length_ms - 1000 : 0;
        req_seek = (int32_t)ms;
        seek_target = (int32_t)ms;
    }
    mutex_unlock(&lock);
    sem_post(&room);
}

media_state_t media_state(void)
{
    media_status.state = state;
    return state;
}

uint32_t media_position(void)
{
    uint32_t p;

    if (seek_target >= 0)
        p = (uint32_t)seek_target;
    else
        p = base_ms + (out_rate ? (uint32_t)((uint64_t)played * 1000 / out_rate) : 0);
    if (info.length_ms && p > info.length_ms)
        p = info.length_ms;
    media_status.position_ms = p;
    return p;
}

uint32_t media_duration(void)
{
    return info_valid ? info.length_ms : 0;
}

bool media_info(media_info_t *out)
{
    bool ok;

    if (!ready)
        return false;
    mutex_lock(&lock, WAIT_FOREVER);
    ok = info_valid;
    if (ok)
        *out = info;
    mutex_unlock(&lock);
    return ok;
}

const char *media_error(void)
{
    return error_text;
}

void media_stats(media_stats_t *out)
{
    uint32_t in_ring = head - tail;

    out->underruns = underruns;
    out->decoded_ms = in_rate ? (uint32_t)((uint64_t)frames_in * 1000 / in_rate) : 0;
    out->decode_cycles = thread ? thread->cycles - cycles0 : 0;
    out->ring_ms = out_rate ? in_ring * 1000 / out_rate : 0;
    media_status.underruns = out->underruns;
    media_status.decoded_ms = out->decoded_ms;
    media_status.decode_cycles = out->decode_cycles;
    media_status.wall_cycles = wall0 ? kernel_cycles() - wall0 : 0;
    media_status.ring_frames = in_ring;
    media_status.cpu_mhz = (uint32_t)clock_mhz();
}
