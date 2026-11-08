/* Radio lab: layer-1 experiments run from the USB shell, so that a test
 * takes seconds instead of a trip with the SD card:
 *
 *   b310e-shell.py run build/os/modules/radiolab.app --arg "arfcn=56" --follow
 *
 * One run boots the DSP, starts layer 1 and the frame clock, then for each
 * variant: an FCCH search and SCH reads on the ARFCN (sync), and BCCH
 * reads with type 2 instructions (docs/gsm-plan.md). Everything goes to
 * the log and to /b310e-os/radiolab.txt.
 *
 * The argument is key=value words. '|' separates variants; each starts
 * from the first one's settings, so the first segment is also the common
 * part ("arfcn=56 | tag=8 | w6=0xfc36"). Keys:
 *   arfcn  GSM 900 ARFCN (56)       gain  gain hint for FCCH/SCH (25)
 *   cell   cell index, word 10 (0)  mf    blocks read per variant (4)
 *   tag    RxChannelTag (7)         tn    timeslot, word 3 (0)
 *   tsc    training sequence, word 4 (-1: the BCC from the SCH)
 *   m      cell FN mod 51 of burst 0 (2)   d  frames added to the offset
 *   sync   1: sync before this variant (1; the first always syncs)
 *   schmf  instead of BCCH: type 10 SCH reads only at the frames the SCH
 *          is due (cell FN mod 51 = 1, 11, 21, 31, 41), idle frames
 *          between, for this many multiframes (0: read BCCH)
 *   pre    command 2 with this word 1 before the frame clock starts (-1
 *          none; first segment only)
 *   c3     command 3 {3, cell, c3, 0, slot} after the sync: c3 = gain
 *          index 0..71 (-1 none); slot = word 4 (0x0101)
 *   wN     type-2 word N (3..10) after the stock build; xN extension word
 *          N (0..2). Numbers are decimal or 0x hex.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "app.h"

#define OUT_FILE    "/b310e-os/radiolab.txt"
#define FN_MOD      2715648u            /* 26 x 51 x 2048 */
#define RING_FREE   25                  /* fi_free() with the ring empty */
#define AHEAD       8                   /* instructions kept queued */
#define SYNC_WINDOW 26
#define SYNC_SCH    51
#define SYNC_MS     1500u
#define VARIANTS    8
#define OUT_SIZE    16384

typedef struct {
    int arfcn, gain, cell, mf, tag, tn, tsc, m, d, sync, pre, c3, slot, schmf;
    int32_t w[11], x[3];                /* -1: the stock value */
} params_t;

static const l1_api_t *L;
static volatile bool stop_asked, running;
static char status[48] = "starting";
static char *out;
static int out_len;
static uint32_t posted;

/* sync result */
static bool synced, exact;
static uint32_t sch_fn, sch_local, bsic;

static void say(const char *fmt, ...)
{
    char line[200];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if (n >= (int)sizeof(line))
        n = sizeof(line) - 1;
    klog("lab: %s", line);
    if (out && out_len + n + 1 < OUT_SIZE) {
        memcpy(out + out_len, line, (size_t)n);
        out_len += n;
        out[out_len++] = '\n';
    }
}

/* ---- the frame ring -------------------------------------------------------- */

static uint32_t queued(void)
{
    return (uint32_t)(RING_FREE - L->fi_free());
}

static void feed_idle(void)
{
    int n = AHEAD - (int)queued();

    if (n > 0 && L->idle_post(n) == DSP_OK) {
        posted += (uint32_t)n;
        L->ask_more(3);
    }
}

static void idle_for(uint32_t ms)
{
    for (uint32_t t0 = ticks(); ticks() - t0 < ms && !stop_asked;) {
        L->poll();
        feed_idle();
        thread_sleep(2);
    }
}

/* ---- sync ---------------------------------------------------------------- */

static void sch_decode(const uint8_t *d, uint32_t *b, uint32_t *fn)
{
    uint32_t t1 = (uint32_t)(d[0] & 3) << 9 | (uint32_t)d[1] << 1 | d[2] >> 7;
    uint32_t t2 = (d[2] >> 2) & 0x1f;
    uint32_t t3 = ((uint32_t)(d[2] & 3) << 1 | (d[3] & 1)) * 10 + 1;

    *b = d[0] >> 2;
    *fn = 51 * ((t3 + 26 - t2) % 26) + t3 + 51 * 26 * t1;
}

/* Header +0x08 (hdr[4]) hi - lo - 1: frames between the burst and the
 * reported frame (0 when +0x08 is 0, which needs no correction). */
static void take_sch(const l1_rx_t *rx)
{
    bool ex = rx->hdr[4] == 0;
    int back = ex ? 0 : (int)(rx->hdr[4] >> 8) - (int)(rx->hdr[4] & 0xff) - 1;
    uint32_t local = rx->hdr[2] | (uint32_t)(rx->hdr[3] & 0xff) << 16, b, fn;

    if (back < 0)
        back = 0;
    sch_decode(rx->data, &b, &fn);
    say("sch: our %lu, +08 %04x (%s, %d back), BSIC %lu, fn %lu (mod 51 = %lu)",
        (unsigned long)local, rx->hdr[4], ex ? "exact" : "corrected", back,
        (unsigned long)b, (unsigned long)fn, (unsigned long)(fn % 51));
    if (!synced || (ex && !exact)) {
        synced = true;
        exact = ex;
        bsic = b;
        sch_fn = fn;
        sch_local = (local + FN_MOD - (uint32_t)back) % FN_MOD;
    }
}

static void sync(const params_t *p, uint16_t index)
{
    uint32_t t0 = ticks(), end = 0;
    int sch = 0;
    l1_rx_t rx;

    synced = exact = false;
    while (queued() > 2 && ticks() - t0 < 200) {
        L->poll();
        thread_sleep(2);
    }
    while (L->rx_read(&rx) == 1)        /* nothing old */
        ;
    if (L->fcch_post(index, (uint8_t)p->gain, SYNC_WINDOW, p->cell) != DSP_OK) {
        say("sync: FCCH post failed");
        return;
    }
    posted++;
    while (ticks() - t0 < SYNC_MS && !stop_asked) {
        bool batch = false;

        while (sch < SYNC_SCH && queued() < AHEAD) {
            if (L->sch_post(index, (uint8_t)p->gain, p->cell) != DSP_OK)
                break;
            posted++;
            batch = true;
            if (++sch == SYNC_SCH)
                end = posted;
        }
        if (batch)
            L->ask_more(3);
        if (sch == SYNC_SCH)
            feed_idle();
        L->poll();
        while (L->rx_read(&rx) == 1)
            if (rx.hdr[0] == 1 && rx.hdr[1] == 0)
                take_sch(&rx);
        if (end && posted - queued() >= end + 10)
            break;
        thread_sleep(2);
    }
    if (synced)
        say("sync: ARFCN %d BSIC %lu (BCC %lu), offset %lu (%s)", p->arfcn,
            (unsigned long)bsic, (unsigned long)(bsic & 7),
            (unsigned long)((sch_fn + FN_MOD - sch_local) % FN_MOD),
            exact ? "exact" : "corrected");
    else
        say("sync: ARFCN %d, no good SCH", p->arfcn);
}

/* ---- BCCH ------------------------------------------------------------------ */

static void si_note(const uint8_t *d)
{
    if (d[1] != 0x06)
        return;
    if (d[2] == 0x1b)                   /* SI 3: cell id, then the LAI */
        say("SI 3: cell id %u, MCC %u%u%u MNC %u%u, LAC %u", d[3] << 8 | d[4],
            d[5] & 0xf, d[5] >> 4, d[6] & 0xf, d[7] & 0xf, d[7] >> 4, d[8] << 8 | d[9]);
    else if (d[2] == 0x1c)
        say("SI 4: MCC %u%u%u MNC %u%u, LAC %u", d[3] & 0xf, d[3] >> 4, d[4] & 0xf,
            d[5] & 0xf, d[5] >> 4, d[6] << 8 | d[7]);
    else
        say("RR message type %02x", d[2]);
}

static void take_blocks(int *good, int *bad)
{
    l1_rx_t rx;

    while (L->rx_read(&rx) == 1) {
        char hex[23 * 2 + 1];

        for (int i = 0; i < 23; i++)
            snprintf(hex + i * 2, 3, "%02x", rx.data[i]);
        say("rx type %u bfi %u fn %lu (cell %lu) hdr %04x %04x %04x %04x %04x: %s",
            rx.hdr[0], rx.hdr[1], (unsigned long)(rx.hdr[2] | (uint32_t)(rx.hdr[3] & 0xff) << 16),
            (unsigned long)((rx.hdr[3] >> 8) & 0x1f), rx.hdr[4], rx.hdr[5], rx.hdr[6],
            rx.hdr[7], rx.hdr[8], hex);
        if (rx.hdr[0] == 1)
            continue;
        if (rx.hdr[1] == 0) {
            (*good)++;
            si_note(rx.data);
        } else {
            (*bad)++;
        }
    }
}

static void build(const params_t *p, uint16_t index, int burst, int tsc,
                  uint16_t fi[11], uint16_t ext[3])
{
    for (int i = 0; i < 11; i++)
        fi[i] = i < 6 ? 0 : 0xffffu;
    for (int i = 0; i < 3; i++)
        ext[i] = 0xffffu;
    fi[0] = 2;
    fi[1] = (uint16_t)((p->tag & 0xf) << 4 | (burst & 3));
    fi[3] = (uint16_t)((p->tn & 7) << 8);
    fi[4] = (uint16_t)((tsc & 7) << 8);
    fi[6] = (uint16_t)(0xfc00u | (index & 0x3ffu));
    fi[10] = (uint16_t)((p->cell & 0x3f) << 10 | 0x100u | 1u);
    for (int i = 3; i < 11; i++)
        if (p->w[i] >= 0)
            fi[i] = (uint16_t)p->w[i];
    for (int i = 0; i < 3; i++)
        if (p->x[i] >= 0)
            ext[i] = (uint16_t)p->x[i];
}

static void bcch(const params_t *p, uint16_t index)
{
    uint32_t off = (sch_fn + FN_MOD - sch_local) % FN_MOD;
    uint32_t delta = (uint32_t)((p->d % 51 + 51) % 51), t0 = ticks(), last_fn0 = 0;
    int tsc = p->tsc >= 0 ? p->tsc : (int)(bsic & 7), mf = 0, burst = -1, good = 0, bad = 0;
    uint32_t msgs = L->probe->msgs, beps = L->probe->beps;
    uint16_t fi[11], ext[3];

    say("bcch: tag %d tn %d tsc %d m %d d %d, %d blocks", p->tag, p->tn, tsc, p->m, p->d, p->mf);
    while (ticks() - t0 < (uint32_t)(p->mf * 240 + 1000) && !stop_asked) {
        bool batch = false;

        while (queued() < AHEAD && (mf < p->mf || burst >= 0)) {
            uint32_t local = L->next_fn();
            int rc;

            if (burst < 0 && (local + off + delta) % FN_MOD % 51 == (uint32_t)p->m) {
                burst = 0;
                last_fn0 = local;
            }
            if (burst >= 0) {
                build(p, index, burst, tsc, fi, ext);
                rc = L->fi_post(fi, ext);
                if (rc == DSP_OK && ++burst == 4) {
                    burst = -1;
                    mf++;
                }
            } else {
                rc = L->idle_post(1);
            }
            if (rc != DSP_OK)
                break;
            posted++;
            batch = true;
        }
        if (batch)
            L->ask_more(3);
        if (mf >= p->mf && burst < 0)
            feed_idle();
        L->poll();
        take_blocks(&good, &bad);
        if (mf >= p->mf && burst < 0 && (L->next_fn() + FN_MOD - last_fn0) % FN_MOD > 30)
            break;
        thread_sleep(2);
    }
    take_blocks(&good, &bad);
    say("bcch: good %d, bad %d; DSP messages %lu (last %08lx), BEP reports %lu",
        good, bad, (unsigned long)(L->probe->msgs - msgs), (unsigned long)L->probe->msg_last,
        (unsigned long)(L->probe->beps - beps));
}

/* SCH reads at the due frames only: do they decode once the FCCH search
 * is over? */
static void sch_check(const params_t *p, uint16_t index)
{
    uint32_t off = (sch_fn + FN_MOD - sch_local) % FN_MOD;
    uint32_t delta = (uint32_t)((p->d % 51 + 51) % 51), t0 = ticks(), end = 0;
    int posted_sch = 0, good = 0, bad = 0, other = 0, want = p->schmf * 5;
    l1_rx_t rx;

    while (ticks() - t0 < (uint32_t)(p->schmf * 240 + 1000) && !stop_asked) {
        bool batch = false;

        while (queued() < AHEAD && posted_sch < want) {
            uint32_t m = (L->next_fn() + off + delta) % FN_MOD % 51;
            int rc = m % 10 == 1 && m != 51 ? L->sch_post(index, (uint8_t)p->gain, p->cell)
                                            : L->idle_post(1);

            if (rc != DSP_OK)
                break;
            if (m % 10 == 1 && ++posted_sch == want)
                end = posted + 1;
            posted++;
            batch = true;
        }
        if (batch)
            L->ask_more(3);
        if (posted_sch == want)
            feed_idle();
        L->poll();
        while (L->rx_read(&rx) == 1) {
            if (rx.hdr[0] != 1) {
                other++;
            } else if (rx.hdr[1] == 0) {
                good++;
                take_sch(&rx);
            } else {
                bad++;
            }
        }
        if (end && posted - queued() >= end + 10)
            break;
        thread_sleep(2);
    }
    say("schcheck: %d SCH reads at the due frames: good %d, bad %d, other blocks %d",
        posted_sch, good, bad, other);
}

/* ---- the argument ---------------------------------------------------------- */

static bool number(const char *s, int32_t *v)
{
    bool neg = *s == '-';
    int32_t n = 0;
    int base = 10;

    if (neg)
        s++;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        base = 16, s += 2;
    if (!*s)
        return false;
    for (; *s; s++) {
        int d = *s >= '0' && *s <= '9' ? *s - '0' :
                *s >= 'a' && *s <= 'f' ? *s - 'a' + 10 :
                *s >= 'A' && *s <= 'F' ? *s - 'A' + 10 : 99;

        if (d >= base)
            return false;
        n = n * base + d;
    }
    *v = neg ? -n : n;
    return true;
}

/* key=value words of one segment (s is changed) */
static void apply(params_t *p, char *s)
{
    static const struct { const char *k; size_t o; } keys[] = {
        { "arfcn", offsetof(params_t, arfcn) }, { "gain", offsetof(params_t, gain) },
        { "cell", offsetof(params_t, cell) }, { "mf", offsetof(params_t, mf) },
        { "tag", offsetof(params_t, tag) }, { "tn", offsetof(params_t, tn) },
        { "tsc", offsetof(params_t, tsc) }, { "m", offsetof(params_t, m) },
        { "d", offsetof(params_t, d) }, { "sync", offsetof(params_t, sync) },
        { "pre", offsetof(params_t, pre) }, { "c3", offsetof(params_t, c3) },
        { "slot", offsetof(params_t, slot) }, { "schmf", offsetof(params_t, schmf) },
    };

    while (*s) {
        char *w, *eq;
        int32_t v;
        bool known = false;

        while (*s == ' ')
            s++;
        if (!*s)
            break;
        w = s;
        while (*s && *s != ' ')
            s++;
        if (*s)
            *s++ = 0;
        if (!(eq = strchr(w, '=')) || !number(eq + 1, &v)) {
            say("arg: %s ignored", w);
            continue;
        }
        *eq = 0;
        if ((w[0] == 'w' || w[0] == 'x') && w[1] >= '0' && w[1] <= '9') {
            int32_t i;

            if (number(w + 1, &i) && w[0] == 'w' && i >= 3 && i <= 10)
                p->w[i] = v & 0xffff, known = true;
            else if (number(w + 1, &i) && w[0] == 'x' && i >= 0 && i <= 2)
                p->x[i] = v & 0xffff, known = true;
        }
        for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]) && !known; i++)
            if (!strcmp(w, keys[i].k)) {
                *(int *)((char *)p + keys[i].o) = (int)v;
                known = true;
            }
        if (!known)
            say("arg: %s ignored", w);
    }
}

/* ---- the run --------------------------------------------------------------- */

static void save(void)
{
    FIL f;
    UINT n;

    if (out && f_open(&f, OUT_FILE, FA_WRITE | FA_CREATE_ALWAYS) == FR_OK) {
        f_write(&f, out, (UINT)out_len, &n);
        f_close(&f);
    }
}

static void worker(void *arg)
{
    const char *a = app_arg();
    char *buf = NULL, *seg[VARIANTS];
    int nseg = 0, rc;
    params_t base = { .arfcn = 56, .gain = 25, .cell = 0, .mf = 4, .tag = 7, .tn = 0,
                      .tsc = -1, .m = 2, .d = 0, .sync = 1, .pre = -1, .c3 = -1,
                      .slot = 0x0101 };

    for (int i = 0; i < 11; i++)
        base.w[i] = -1;
    base.x[0] = base.x[1] = base.x[2] = -1;
    out = malloc(OUT_SIZE);
    out_len = 0;
    say("radiolab: arg \"%s\"", a ? a : "");
    if (a && (buf = malloc(strlen(a) + 1))) {
        memcpy(buf, a, strlen(a) + 1);
        seg[nseg++] = buf;
        for (char *s = buf; *s && nseg < VARIANTS; s++)
            if (*s == '|') {
                *s = 0;
                seg[nseg++] = s + 1;
            }
    }
    if (nseg) {
        char *first = malloc(strlen(seg[0]) + 1);

        if (first) {                    /* the common part, kept for the others */
            memcpy(first, seg[0], strlen(seg[0]) + 1);
            apply(&base, first);
            free(first);
        }
    }
    if (L->info->state == DSP_BOOTING || L->info->state == DSP_RUNNING) {
        say("the DSP is in use (Radio status?)");
        goto done;
    }
    snprintf(status, sizeof(status), "DSP boot");
    if ((rc = L->dsp_boot()) != DSP_OK || (rc = L->start()) != DSP_OK) {
        say("DSP boot / layer 1 start failed: %d", rc);
        goto stop;
    }
    if (base.pre >= 0) {
        uint16_t c[5] = { 2, (uint16_t)base.pre, 0, 0, 0 };

        rc = L->cmd_post(c);
        idle_for(0);
        for (int i = 0; i < 20; i++) {
            L->poll();
            thread_sleep(1);
        }
        say("command 2 %04x: %d", base.pre, rc);
    }
    posted = 0;
    if ((rc = L->run()) != DSP_OK) {
        say("frame clock failed: %d", rc);
        goto stop;
    }
    posted = 4;
    idle_for(300);
    say("frame clock: %lu frames taken in 300 ms", (unsigned long)(posted - queued()));
    for (int v = 0; v < (nseg ? nseg : 1) && !stop_asked; v++) {
        params_t p = base;
        uint16_t index;

        if (v > 0)
            apply(&p, seg[v]);
        index = L->arfcn_index(L1_BAND_GSM900, p.arfcn);
        say("== variant %d: arfcn %d (index %u)", v, p.arfcn, index);
        snprintf(status, sizeof(status), "variant %d: sync", v);
        if (v == 0 || p.sync || !synced)
            sync(&p, index);
        if (!synced)
            continue;
        if (p.c3 >= 0) {
            uint16_t c[5] = { 3, (uint16_t)p.cell, (uint16_t)p.c3, 0, (uint16_t)p.slot };

            say("command 3 cell %d gain %d slot %04x: %d", p.cell, p.c3, p.slot, L->cmd_post(c));
            idle_for(20);
        }
        snprintf(status, sizeof(status), "variant %d: %s", v, p.schmf ? "sch" : "bcch");
        if (p.schmf) {
            sch_check(&p, index);
            continue;
        }
        bcch(&p, index);
        say("bep: last %04x %04x (word 0 bits 13:0 = arfcn index %u)",
            (unsigned)L->probe->bep_last[0], (unsigned)L->probe->bep_last[1],
            (unsigned)(L->probe->bep_last[0] & 0x3fff));
    }
stop:
    L->dsp_stop();
done:
    say("radiolab: done%s", stop_asked ? " (stopped)" : "");
    snprintf(status, sizeof(status), "done: %s", OUT_FILE);
    save();
    free(buf);
    free(out);
    out = NULL;
    running = false;
    ui_refresh();
}

/* ---- the screen ------------------------------------------------------------ */

static void draw(screen_t *s)
{
    gfx_text(4, UI_TOP + 8, "Radio lab", C_ACCENT, FONT_SMALL, 1, 0);
    gfx_text(4, UI_TOP + 24, status, C_TEXT, FONT_SMALL, 1, LCD_WIDTH - 8);
    gfx_text(4, UI_TOP + 40, "output: log, " OUT_FILE, C_DIM, FONT_SMALL, 1, LCD_WIDTH - 8);
}

static void key(screen_t *s, const key_event_t *ev)
{
    if (ev->code == KEY_RSOFT) {
        stop_asked = true;              /* the thread stops the DSP and ends */
        ui_pop();
    }
}

static screen_t screen = {
    .title = "Radio lab", .right = "Close", .draw = draw, .key = key, .tick_ms = 250,
};

static screen_t *start(void)
{
    L = l1_api;
    stop_asked = false;
    running = true;
    if (!thread_create("radiolab", PRIO_NORMAL - 1, 4096, worker, NULL)) {
        running = false;
        snprintf(status, sizeof(status), "no thread");
    }
    return &screen;
}

APP("Radio lab", start, NULL, NULL);
