/* B310E OS — PCM playback and recording on DMA through the VBC.
 *
 * Ported from the Rockbox port's pcm-sc6530c.c (GPLv2-or-later, the stock
 * e52q7a DMA/VBC path, verified on the phone). Playback: stock channels
 * 4 (DA0, left) and 3 (DA1, right), one-based, each reading a 160-frame
 * halfword plane; the VBC requests 15/16 are bound to them through
 * 0x20102038/3c. Each interrupt means a bank was taken while up to 160
 * frames still play from the VBC, so the next plane is filled then.
 * Recording: AD0 (ADC L) FIFO 0x8200300c, request 14 bound to channel 5
 * through 0x20102034, into two 160-sample banks. DMA interrupt: line 20,
 * shared by both.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/arch.h"
#include "os/audio.h"
#include "os/kernel.h"
#include "os/lib.h"
#include "drivers/audio_internal.h"

#define IRQ_DMA         20
#define DMA_LEFT        3               /* zero-based channel numbers */
#define DMA_RIGHT       2
#define DMA_REC         4
#define DMA_PLAY_MASK   ((1u << DMA_LEFT) | (1u << DMA_RIGHT))
#define DMA_REG(ch, off) REG32(0x20101000u + (ch) * 0x40u + (off))
#define DMA_PENDING     REG32(0x20100010u)
#define DMA_BLOCK_IRQ   2u
#define DMA_ERROR_IRQ   16u
#define DMA_ACK         0x1f000000u
#define REC_FIFO        (VBC_BASE + 0x0c)
#define REC_MAP         0x20102034u
#define VBC_REC         ((1u << 12) | (1u << 11))

static int16_t plane[2][VBC_FRAMES] __attribute__((aligned(32)));
/* aligned: fill callbacks may copy into it a word at a time */
static int16_t lr[VBC_FRAMES * 2] __attribute__((aligned(4)));
static int16_t rec_bank[2][VBC_FRAMES] __attribute__((aligned(32)));

static audio_fill_fn fill;
static void *fill_ctx;
static audio_sink_fn sink;
static void *sink_ctx;
static volatile bool running, rec_running;
static volatile bool starting;          /* audio_play: the codec is ours already */
static volatile bool rec_failed;
static unsigned rec_skip;               /* frames still to throw away */
static bool eof, draining, irq_ready;
static unsigned completed, rec_next;
static int32_t gain_q15 = 32768;
static volatile unsigned peak;

/* ---- gain --------------------------------------------------------------- */

/* tenths of a dB (0 or below) to Q15: halve per 6 dB, a table per dB in
 * between (10^(-n/20) for n = 0..5). */
void pcm_set_attenuation(int db10)
{
    static const uint16_t per_db[6] = { 32768, 29205, 26029, 23198, 20675, 18427 };
    int32_t g;

    if (db10 <= -1000) {
        gain_q15 = 0;
        return;
    }
    if (db10 > 0)
        db10 = 0;
    db10 = -db10;
    g = per_db[(db10 % 60) / 10];
    g >>= db10 / 60;
    gain_q15 = g;
}

bool pcm_busy(void)
{
    return running || rec_running || starting;
}

bool audio_record_failed(void)
{
    return rec_failed;
}

/* An app going away: nothing it started may call into its freed code. */
void audio_drop_code(const void *base, size_t size)
{
    uintptr_t lo = (uintptr_t)base;

    if (running && (uintptr_t)fill - lo < size)
        audio_play_stop();
    /* also after a DMA error ended it: the route is still on (audio_record_stop) */
    if ((rec_running || rec_failed) && (uintptr_t)sink - lo < size)
        audio_record_stop();
}

unsigned audio_peak(void)
{
    unsigned p = peak;

    peak = 0;
    return p;
}

/* ---- interrupt line ----------------------------------------------------- */
static void dma_isr(void);

static void irq_update(void)
{
    if (!irq_ready) {
        irq_register(IRQ_DMA, dma_isr);
        irq_ready = true;
    }
    sleep_keep_pll = running || rec_running;
    if (running || rec_running)
        irq_enable_line(IRQ_DMA);
    else
        irq_disable_line(IRQ_DMA);
}

/* ---- playback ----------------------------------------------------------- */

/* why the last playback stopped (the engine logs it when it thinks the
 * audio was taken): 1 stop asked, 2 a DMA error (status in pcm_stop_status),
 * 3 drained to the end */
volatile int pcm_stop_why;
volatile uint32_t pcm_stop_status;

static void play_hw_stop(void)
{
    /* VB enable (bit 15) is shared with capture */
    VBC_CTRL &= rec_running ? ~(VBC_PLAY & ~0x8000u) : ~VBC_PLAY;
    DMA_REG(DMA_LEFT, 8) = 0;
    DMA_REG(DMA_RIGHT, 8) = 0;
    DMA_REG(DMA_LEFT, 12) = DMA_ACK;
    DMA_REG(DMA_RIGHT, 12) = DMA_ACK;
    audio_mute(true);
    running = false;
    completed = 0;
    irq_update();
}

typedef uint32_t __attribute__((may_alias)) frame_t;     /* L | R << 16, as lr holds it */

/* A new peak is rare: out of line, so the compiler keeps the branch (made
 * conditional, the abs ran for every sample) */
__attribute__((target("arm"), noinline))
static uint32_t magnitude(int32_t x)
{
    return (uint32_t)(x < 0 ? -x : x);
}

/* |x| above pk: x + pk, unsigned, is above 2 pk (below -pk it wraps), one
 * add and a compare with a shifted operand */
#define PEAK(x, pk) do {                                                    \
        if ((uint32_t)(x) + (pk) > (pk) << 1)                               \
            (pk) = magnitude(x);                                            \
    } while (0)

/* n frames times g (Q15) into the two planes; pk raised to the largest
 * magnitude among them. ARM -O2: the Thumb -Os loop it replaces took
 * about 40 instructions a frame (a bounds test per frame, the gain and
 * the peak spilled to the stack). */
__attribute__((target("arm"), optimize("O2"), noinline))
static unsigned split_frames(const frame_t *f, int16_t *left, int16_t *right, unsigned n,
                             int32_t g, unsigned pk)
{
    const frame_t *end = f + n;

    if (g == 32768) {                   /* x * 32768 >> 15 is x: full volume, no multiply */
        const int16_t *s = (const int16_t *)(const void *)f, *e = (const int16_t *)(const void *)end;

        for (; s < e; s += 2) {
            int32_t l = s[0], r = s[1];

            PEAK(l, pk);
            PEAK(r, pk);
            *left++ = (int16_t)l;
            *right++ = (int16_t)r;
        }
    } else {                            /* below 32768 g fits 16 bits: smulbb, smultb */
        int16_t g16 = (int16_t)g;

        for (; f < end; f++) {
            uint32_t w = *f;
            int32_t l = (int16_t)w * g16 >> 15, r = (int16_t)(w >> 16) * g16 >> 15;

            PEAK(l, pk);
            PEAK(r, pk);
            *left++ = (int16_t)l;
            *right++ = (int16_t)r;
        }
    }
    return pk;
}

/* the recording's bank, the same way (its loop had the same abs and max) */
__attribute__((target("arm"), optimize("O2"), noinline))
static unsigned bank_peak(const int16_t *s, unsigned n, unsigned pk)
{
    for (const int16_t *e = s + n; s < e; s++) {
        int32_t x = *s;

        PEAK(x, pk);
    }
    return pk;
}

static unsigned fill_plane(void)
{
    unsigned n = 0, got, pk = peak;

    while (n < VBC_FRAMES && !eof) {
        got = fill(lr + n * 2, VBC_FRAMES - n, fill_ctx);
        if (!got)
            eof = true;
        n += got;
    }
    peak = split_frames((const frame_t *)(const void *)lr, plane[0], plane[1], n, gain_q15, pk);
    if (n < VBC_FRAMES) {               /* the end: silence after it (adds nothing to the peak) */
        memset(plane[0] + n, 0, (VBC_FRAMES - n) * sizeof(plane[0][0]));
        memset(plane[1] + n, 0, (VBC_FRAMES - n) * sizeof(plane[1][0]));
    }
    dcache_clean_range(plane, sizeof(plane));
    return n;
}

static void arm_channel(unsigned ch, const int16_t *samples, uint32_t port, uint32_t irqs)
{
    DMA_REG(ch, 8) = 0;
    DMA_REG(ch, 12) = DMA_ACK | irqs;
    DMA_REG(ch, 16) = (uint32_t)samples;
    DMA_REG(ch, 20) = port;
    /* halfword widths [31:30]/[29:28], destination fixed [21:20],
     * 320-byte fragment and block */
    DMA_REG(ch, 24) = 0x50300140u;
    DMA_REG(ch, 28) = VBC_FRAMES * 2;
    DMA_REG(ch, 8) = 0x3001u;           /* priority 3, normal request, on */
}

/* One block interrupt a plane, not two (power audit A6, 2026-11-18: both
 * channels raised one, 2 x 300 a second at 48 kHz): only irq_ch raises it.
 * The other one is done by then, which the interrupt checks by its raw
 * status (bit 9); if it is not, its block interrupt is enabled and finishes
 * the plane as before (pcm_late_channel counts that), and the interrupt moves
 * to that channel from the next plane: which channel ends last differed from
 * run to run on the phone (once late on every plane, once never). Errors
 * interrupt on both. */
uint32_t pcm_late_channel;
static unsigned irq_ch = DMA_RIGHT;

static void arm_stereo(void)
{
    completed = 0;
    arm_channel(DMA_LEFT, plane[0], VBC_BASE,
                DMA_ERROR_IRQ | (irq_ch == DMA_LEFT ? DMA_BLOCK_IRQ : 0));
    arm_channel(DMA_RIGHT, plane[1], VBC_BASE + 4,
                DMA_ERROR_IRQ | (irq_ch == DMA_RIGHT ? DMA_BLOCK_IRQ : 0));
}

/* the channels whose block is done without their interrupt (raw status) */
static void collect_quiet(void)
{
    for (unsigned ch = DMA_RIGHT; ch <= DMA_LEFT; ch++) {
        uint32_t bit = 1u << ch;

        if (completed & bit)
            continue;
        for (int i = 0; i < 16; i++) {
            if (DMA_REG(ch, 12) & (1u << 9)) {
                completed |= bit;
                DMA_REG(ch, 12) = DMA_ACK | DMA_ERROR_IRQ;
                break;
            }
        }
        if (!(completed & bit)) {       /* still running: its own interrupt ends it */
            pcm_late_channel++;
            irq_ch = ch;                /* it ends last: its interrupt from now */
            DMA_REG(ch, 12) = DMA_BLOCK_IRQ | DMA_ERROR_IRQ;
        }
    }
}

/* ---- recording ---------------------------------------------------------- */

static void rec_arm(void)
{
    int16_t *bank = rec_bank[rec_next];

    dcache_flush_range(bank, sizeof(rec_bank[0]));
    DMA_REG(DMA_REC, 8) = 0;
    DMA_REG(DMA_REC, 12) = DMA_ACK | DMA_BLOCK_IRQ | DMA_ERROR_IRQ;
    DMA_REG(DMA_REC, 16) = REC_FIFO;
    DMA_REG(DMA_REC, 20) = (uint32_t)bank;
    DMA_REG(DMA_REC, 24) = 0x50100140u; /* source fixed, 320 bytes */
    DMA_REG(DMA_REC, 28) = VBC_FRAMES * 2;
    DMA_REG(DMA_REC, 8) = 0x3001u;
}

static void rec_hw_stop(void)
{
    rec_running = false;
    DMA_REG(DMA_REC, 8) = 0;
    DMA_REG(DMA_REC, 12) = DMA_ACK;
    REG32(REC_MAP) = 0;
    VBC_CTRL &= running ? ~VBC_REC : ~(VBC_REC | 0x8000u);
    irq_update();
}

static void rec_irq(void)
{
    uint32_t status;
    unsigned done;
    const int16_t *bank;
    unsigned pk = peak;

    if (!(DMA_PENDING & (1u << DMA_REC)))
        return;
    status = DMA_REG(DMA_REC, 12);
    DMA_REG(DMA_REC, 12) = DMA_ACK | DMA_BLOCK_IRQ | DMA_ERROR_IRQ;
    if (!rec_running)
        return;
    if (status & (1u << 12)) {          /* DMA error: wav_pump ends the file */
        rec_failed = true;
        rec_hw_stop();
        return;
    }
    if (!(status & (1u << 9)))
        return;
    done = rec_next;
    rec_next ^= 1;
    rec_arm();                          /* keep the VBC drained first */
    bank = rec_bank[done];
    dcache_flush_range(rec_bank[done], sizeof(rec_bank[0]));
    if (rec_skip) {                     /* the mic path settling */
        rec_skip = rec_skip > VBC_FRAMES ? rec_skip - VBC_FRAMES : 0;
        return;
    }
    peak = bank_peak(bank, VBC_FRAMES, pk);
    sink(bank, VBC_FRAMES, sink_ctx);
}

/* ---- the interrupt ------------------------------------------------------ */

static void dma_isr(void)
{
    uint32_t pending;
    bool error = false;

    rec_irq();
    pending = DMA_PENDING & DMA_PLAY_MASK;
    for (unsigned ch = DMA_RIGHT; ch <= DMA_LEFT; ch++) {
        uint32_t status;

        if (!(pending & (1u << ch)))
            continue;
        status = DMA_REG(ch, 12);
        DMA_REG(ch, 12) = DMA_ACK | DMA_BLOCK_IRQ | DMA_ERROR_IRQ;
        if (status & (1u << 12))
            pcm_stop_status = status | ch << 28;
        error |= (status & (1u << 12)) != 0;
        if (status & (1u << 9))
            completed |= 1u << ch;
    }
    if (!running)
        return;
    if (error) {
        play_hw_stop();
        pcm_stop_why = 2;
        return;
    }
    if (completed && completed != DMA_PLAY_MASK)
        collect_quiet();
    if (completed != DMA_PLAY_MASK)     /* both channels done with the plane */
        return;
    if (draining) {
        play_hw_stop();
        pcm_stop_why = 3;
        return;
    }
    draining = fill_plane() == 0 && eof;
    arm_stereo();
}

/* ---- the public side ---------------------------------------------------- */

int audio_play(unsigned rate, audio_fill_fn f, void *ctx)
{
    uint32_t s;

    if (!f || !audio_rate_ok(rate))
        return -1;
    audio_play_stop();
    /* All in the codec's mutex: a killed app thread dies at its release,
     * after starting is cleared, not at audio_power's or set_rate's. */
    audio_lock();
    /* until running is set, so no other thread closes the codec under the DMA */
    starting = true;
    audio_power(true);
    audio_set_rate(rate);
    s = arch_irq_save();
    fill = f;
    fill_ctx = ctx;
    eof = draining = false;
    peak = 0;
    fill_plane();
    /* Stock 0xa7b96: bind VBC DA0/DA1 requests 15/16 to channels 4/3 */
    REG32(0x20102038) = DMA_LEFT + 1;
    REG32(0x2010203c) = DMA_RIGHT + 1;
    arm_stereo();
    running = true;
    starting = false;
    audio_mute(false);
    VBC_CTRL = (VBC_CTRL & ~VBC_CPU_ACCESS) | VBC_PLAY;
    irq_update();
    arch_irq_restore(s);
    audio_unlock();
    return 0;
}

void audio_play_stop(void)
{
    uint32_t s = arch_irq_save();

    if (running) {
        play_hw_stop();
        pcm_stop_why = 1;
    }
    arch_irq_restore(s);
}

bool audio_playing(void)
{
    return running;
}

int audio_record(unsigned rate, int source, audio_sink_fn f, void *ctx)
{
    uint32_t s;

    if (!f || rate % 4000 || rate / 4000 > 15 || !audio_rate_ok(rate))
        return -1;                      /* the ADC runs at 4 kHz multiples, up to 60 kHz */
    audio_record_stop();
    /* All in the codec's mutex: a killed app thread dies at its release,
     * with rec_running set (audio_drop_code closes it), not with the route
     * on and nothing to close it. */
    audio_lock();
    audio_power(true);
    audio_set_rate(rate);
    audio_record_route(source);
    s = arch_irq_save();
    sink = f;
    sink_ctx = ctx;
    rec_next = 0;
    peak = 0;
    /* A phone-mic recording starts with 50 ms at full scale, dying away over about
     * 300 ms (phone, 2026-11-09): a loud click at the start of every file. */
    rec_skip = rate * 3u / 10u;
    /* Stock 0xa78e4 order: VBC AD enable, then the DMA channel */
    REG32(REC_MAP) = DMA_REC + 1;
    VBC_CTRL = (VBC_CTRL & ~VBC_CPU_ACCESS) | 0x8000u | VBC_REC;
    rec_arm();
    rec_failed = false;
    rec_running = true;
    irq_update();
    arch_irq_restore(s);
    audio_unlock();
    return 0;
}

void audio_record_stop(void)
{
    uint32_t s = arch_irq_save();
    bool was = rec_running;

    if (was)
        rec_hw_stop();
    arch_irq_restore(s);
    if (was || rec_failed)              /* a DMA error stopped it: the route is still on */
        audio_record_route_off();
    rec_failed = false;
}
