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
 *   arfcn  GSM 900 ARFCN (56)       gain  gain setting, all reads (25)
 *   cell   cell index, word 10 (0)  mf    blocks read per variant (4)
 *   tag    RxChannelTag (7)         tn    timeslot, word 3 (0)
 *   tsc    training sequence, word 4 (-1: the BCC from the SCH)
 *   m      cell FN mod 51 of burst 0 (2)   d  frames added to the offset
 *   sync   1: sync before this variant (1; the first always syncs)
 *   sch    type 10 reads after the FCCH window (0). A type 10 that fails
 *          sets the DSP's frequency back to its centre value when it is not
 *          camped, and BCCH then fails too; 0 uses the SCH the search returns
 *   schmf  instead of BCCH: type 10 SCH reads only at the frames the SCH
 *          is due (cell FN mod 51 = 1, 11, 21, 31, 41), idle frames
 *          between, for this many multiframes (0: read BCCH)
 *   pre    command 2 with this word 1 before the frame clock starts (-1
 *          none; first segment only)
 *   c3     command 3 {3, cell, c3m, c3, slot} after the sync (-1 none): the
 *          serving cell. c3 is word 3, the stock's rxlev for the cell; the
 *          DSP takes its gain from this, not from word 6 of type 2
 *   c3m    word 2 of command 3 (0); slot = word 4 (0x0101: bit 0 starts
 *          tracking, bit 8 is the card)
 *   q      0: log each block; n: only a line per n blocks (for long runs)
 *   idx    the raw arfcn index, instead of arfcn (any band; -1 none)
 *   read   n: after the sync, the driver's read of up to n blocks, and the cell's
 *          parameters from System Information, instead of the raw BCCH reads
 *   si     1: only check the System Information parser on made-up blocks (no DSP)
 *   sel    n: this cell and up to n neighbours from its SI 2 as the candidates of
 *          the driver's selection (mcc, mnc: only that network; 0 any)
 *   stay   after sel: seconds on the cell it chose
 *   cch    seconds reading the common control channel of the synced cell: paging
 *          and access grants, counted by kind (identities never stored)
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
#define RESP_OFF    0x390u              /* share memory: 8 x 10 bytes */
#define RESP_IN_OFF 0x3e0u
#define RESP_SLOTS  8u

typedef struct {
    int arfcn, gain, cell, mf, tag, tn, tsc, m, d, sync, pre, c3, c3m, slot, schmf, sch, q, idx;
    int read, si, sel, stay, mcc, mnc, cch, tx, fire;
    int32_t w[11], x[3];                /* -1: the stock value */
} params_t;

static const l1_api_t *L;
static volatile bool stop_asked, running;
static char status[48] = "starting";
static char *out;
static int out_len;
static uint32_t posted;
static const char *list_name(uint8_t f);

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

/* ---- DSP responses ---------------------------------------------------------- */

/* The driver's interrupt empties the response ring, but the slots keep their
 * words. Everything except the frame replies (type 4, word 1 = 0x40 or 0x41)
 * goes to the log: the stock handles about 20 types (FUN_0004ee54). */
static uint32_t resp_seen;

static void resp_log(void)
{
    uint32_t in = L->share_r16(RESP_IN_OFF);

    if (in >= RESP_SLOTS)
        return;
    while (resp_seen != in) {
        uint16_t r[5];

        resp_seen = (resp_seen + 1) & (RESP_SLOTS - 1);
        for (int i = 0; i < 5; i++)
            r[i] = L->share_r16(RESP_OFF + resp_seen * 10 + (uint32_t)i * 2);
        /* also not 1 (rx block ready) and 0x27 (BEP report) */
        if ((r[0] & 0xff) != 1 && (r[0] & 0xff) != 0x27 &&
            ((r[0] & 0xff) != 4 || (r[1] & ~1u) != 0x40))
            say("resp: %04x %04x %04x %04x %04x", r[0], r[1], r[2], r[3], r[4]);
    }
}

/* ---- the frame ring -------------------------------------------------------- */

static uint32_t queued(void)
{
    return (uint32_t)(RING_FREE - L->fi_free());
}

static void poll_all(void)
{
    L->poll();
    resp_log();
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
        poll_all();
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

/* Header +0x08 (hdr[4]) is set by a search: hi - lo frames between the burst
 * and the reported frame, as the stock has it (FUN_000461e4). A type 10 has
 * 0 there and reports the frame after its own. Checked on the phone: BCCH
 * passes its CRC with this offset, and with no other. */
static void take_sch(const l1_rx_t *rx)
{
    bool ex = rx->hdr[4] == 0;
    int back = ex ? 1 : (int)(rx->hdr[4] >> 8) - (int)(rx->hdr[4] & 0xff);
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

static l1_cell_t lab_cell;

static void sync(const params_t *p, uint16_t index)
{
    uint32_t t0 = ticks(), end = 0;
    int sch = 0;
    l1_rx_t rx;

    synced = exact = false;
    if (!p->sch) {                      /* the driver's sync */
        int rc = L->cell_sync(&lab_cell, index, (uint8_t)p->gain, p->cell, SYNC_MS);

        if (rc == 1) {
            synced = true;
            bsic = lab_cell.bsic;
            sch_fn = lab_cell.fn_offset;
            sch_local = 0;
            say("sync: index %u BSIC %lu (BCC %lu), offset %lu", index, (unsigned long)bsic,
                (unsigned long)(bsic & 7), (unsigned long)sch_fn);
        } else {
            say("sync: index %u, no SCH (%d)", index, rc);
        }
        return;
    }
    while (queued() > 2 && ticks() - t0 < 200) {
        poll_all();
        thread_sleep(2);
    }
    while (L->rx_read(&rx) == 1)        /* nothing old */
        ;
    if (L->fcch_post(index, (uint8_t)p->gain, SYNC_WINDOW, p->cell) != DSP_OK) {
        say("sync: FCCH post failed");
        return;
    }
    posted++;
    if (!p->sch)
        end = posted;
    while (ticks() - t0 < SYNC_MS && !stop_asked) {
        bool batch = false;

        while (sch < p->sch && queued() < AHEAD) {
            if (L->sch_post(index, (uint8_t)p->gain, p->cell) != DSP_OK)
                break;
            posted++;
            batch = true;
            if (++sch == p->sch)
                end = posted;
        }
        if (batch)
            L->ask_more(3);
        if (sch == p->sch)
            feed_idle();
        poll_all();
        while (L->rx_read(&rx) == 1)
            if (rx.hdr[0] == 1 && rx.hdr[1] == 0)
                take_sch(&rx);
        if (!p->sch && synced)          /* straight on: nothing may undo the search */
            break;
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

/* System Information only: it is broadcast for every phone. Anything else on
 * these channels (paging, assignments) is meant for someone else, and is
 * counted by type without keeping its contents. */
static bool is_si(const uint8_t *d)
{
    return d[1] == 0x06 && ((d[2] >= 0x18 && d[2] <= 0x1f) || d[2] <= 0x03 ||
                            (d[2] >= 0x05 && d[2] <= 0x07));
}

static void si_note(const uint8_t *d)
{
    if (d[2] == 0x1b)                   /* SI 3: cell id, then the LAI */
        say("SI 3: cell id %u, MCC %u%u%u MNC %u%u, LAC %u", d[3] << 8 | d[4],
            d[5] & 0xf, d[5] >> 4, d[6] & 0xf, d[7] & 0xf, d[7] >> 4, d[8] << 8 | d[9]);
    else if (d[2] == 0x1c)
        say("SI 4: MCC %u%u%u MNC %u%u, LAC %u", d[3] & 0xf, d[3] >> 4, d[4] & 0xf,
            d[5] & 0xf, d[5] >> 4, d[6] << 8 | d[7]);
}

static int quiet, seen_blocks, part_bad;

static void take_blocks(int *good, int *bad)
{
    l1_rx_t rx;

    while (L->rx_read(&rx) == 1) {
        char hex[23 * 2 + 1];
        uint32_t fn = rx.hdr[2] | (uint32_t)(rx.hdr[3] & 0xff) << 16;

        if (rx.hdr[0] == 1)
            continue;
        if (quiet) {
            if (rx.hdr[1] != 0) {
                (*bad)++;
                part_bad++;
            } else {
                (*good)++;
            }
            if (++seen_blocks % quiet == 0) {
                say("block %d, fn %lu: %d bad of the last %d; BEP %04x %04x", seen_blocks,
                    (unsigned long)fn, part_bad, quiet, (unsigned)L->probe->bep_last[0],
                    (unsigned)L->probe->bep_last[1]);
                part_bad = 0;
            }
            continue;
        }
        if (rx.hdr[1] != 0) {
            (*bad)++;
            say("rx type %u bfi %u fn %lu", rx.hdr[0], rx.hdr[1], (unsigned long)fn);
        } else if (is_si(rx.data)) {
            (*good)++;
            for (int i = 0; i < 23; i++)
                snprintf(hex + i * 2, 3, "%02x", rx.data[i]);
            say("rx type %u good fn %lu: %s", rx.hdr[0], (unsigned long)fn, hex);
            si_note(rx.data);
        } else {
            (*good)++;
            say("rx type %u good fn %lu: not System Information, not kept",
                rx.hdr[0], (unsigned long)fn);
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
    fi[6] = (uint16_t)((p->gain & 0x3f) << 10 | (index & 0x3ffu));
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
    quiet = p->q;
    seen_blocks = part_bad = 0;
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
        poll_all();
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
        poll_all();
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

/* ---- a cell, through the driver ------------------------------------------- */

static void cell_read(const params_t *p)
{
    l1_cell_t *c = &lab_cell;
    int rc = L->cell_read(c, p->read, L1_SI_IDLE, (uint32_t)p->read * 260u + 1000u);

    say("cell read: %d; good %lu, bad %lu, not SI %lu, SI seen %08lx", rc, (unsigned long)c->good,
        (unsigned long)c->bad, (unsigned long)c->other, (unsigned long)c->si_seen);
    if (c->si_seen & (L1_SI3 | L1_SI4))
        say("network: MCC %03u MNC %0*u, LAC %u, cell id %u", c->mcc, c->mnc_digits, c->mnc,
            c->lac, c->ci);
    if (c->si_seen & L1_SI3)
        say("cell: CCCH conf %u, AG blocks %u, paging multiframes %u, T3212 %u, attach %u",
            c->ccch_conf, c->bs_ag_blks_res, c->bs_pa_mfrms, c->t3212, c->att);
    if (c->si_seen & (L1_SI3 | L1_SI4))
        say("selection: rxlev access min %u, TX power max %u, hysteresis %u, barred %u, NECI %u",
            c->rxlev_access_min, c->ms_txpwr_max_cch, c->hysteresis, c->barred, c->neci);
    if (c->si_seen & L1_SI2) {
        char line[120];
        int n = snprintf(line, sizeof(line), "neighbours (%u, %s, NCC permitted %02x):",
                         c->neighbours.count, list_name(c->neighbours.format), c->ncc_permitted);

        for (int i = 0; i < c->neighbours.count && i < L1_CELL_ARFCNS && n < 110; i++)
            n += snprintf(line + n, sizeof(line) - (size_t)n, " %u", c->neighbours.arfcn[i]);
        say("%s", line);
    }
    if (c->si_seen & L1_SI1) {
        char line[120];
        int n = snprintf(line, sizeof(line), "channels (%u, %s):", c->channels.count,
                         list_name(c->channels.format));

        for (int i = 0; i < c->channels.count && i < L1_CELL_ARFCNS && n < 110; i++)
            n += snprintf(line + n, sizeof(line) - (size_t)n, " %u", c->channels.arfcn[i]);
        say("%s", line);
    }
}

#define SEL_MAX 9
static l1_cell_t sel_cells[SEL_MAX];

/* Candidates named by the cell itself: it and its SI 2 list. No scan. */
static void select_cell(const params_t *p, uint16_t first)
{
    uint16_t index[SEL_MAX];
    int n = 1, best;
    l1_cell_t *c = &lab_cell;

    L->cell_read(c, 8, L1_SI2, 3000);
    index[0] = first;
    for (int i = 0; i < c->neighbours.count && i < L1_CELL_ARFCNS && n < SEL_MAX && n <= p->sel; i++)
        if (c->neighbours.arfcn[i] != first)
            index[n++] = c->neighbours.arfcn[i];
    say("select: %d candidates", n);
    best = L->cell_select(index, n, (uint16_t)p->mcc, (uint16_t)p->mnc, sel_cells);
    for (int i = 0; i < n && !stop_asked; i++) {
        const l1_cell_t *s = &sel_cells[i];

        if (s->synced)
            say("  index %u: rxlev %d, BSIC %u, %03u-%0*u, C1 %d, barred %u, %s%s", s->index, s->rxlev,
                s->bsic, s->mcc, s->mnc_digits, s->mnc, s->c1, s->barred,
                s->suitable ? "suitable" : "not suitable", i == best ? ", CHOSEN" : "");
        else
            say("  index %u: rxlev %d, no SCH", index[i], s->rxlev);
    }
    if (best < 0) {
        say("select: no suitable cell");
        return;
    }
    if (p->stay) {
        l1_stay_t st;
        l1_cell_t *b = &sel_cells[best];
        uint32_t t0 = ticks();
        int rc;

        rc = L->cell_sync(b, b->index, 25, best, SYNC_MS);
        if (rc != 1) {
            say("stay: no SCH on index %u (%d)", index[best], rc);
            return;
        }
        rc = L->cell_stay(b, (uint32_t)p->stay * 1000u, &st);
        say("stay: %d after %lu ms; good %lu, bad %lu, searches again %lu, offset changes %lu, "
            "lost %u", rc, (unsigned long)(ticks() - t0), (unsigned long)st.good,
            (unsigned long)st.bad, (unsigned long)st.resyncs, (unsigned long)st.slips, st.lost);
        say("stay: %lu measures, rxlev min %d max %d last %d", (unsigned long)st.measures,
            st.rxlev_min, st.rxlev_max, st.rxlev_last);
    }
}

/* The parser on made-up blocks: a test network (MCC 001), nothing real. */
static int si_failed, si_checked;

static void si_expect(const char *what, bool ok)
{
    si_checked++;
    if (!ok) {
        si_failed++;
        say("si check failed: %s", what);
    }
}

static const char *list_name(uint8_t f)
{
    static const char *const n[] = { "bit map 0", "range 1024", "range 512",
                                     "range 256", "range 128", "variable bit map", "unknown" };

    return n[f > L1_LIST_UNKNOWN ? L1_LIST_UNKNOWN : f];
}

static void si_check(void)
{
    static const uint8_t si1[23] = {
        0x55, 0x06, 0x19, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0x53, 0x00, 0x00, 0x2b };                       /* a range format; barred */
    static const uint8_t si2[23] = {
        0x59, 0x06, 0x1a, 0x18, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01,
        0xff, 0x51, 0x00, 0x00 };                       /* ARFCN 124, 120, 1 */
    static const uint8_t si3[23] = {
        0x49, 0x06, 0x1b, 0x00, 0x01, 0x00, 0xf1, 0x10, 0x00, 0x01, 0xc8, 0x01, 0x3c, 0x17,
        0x65, 0x40, 0x45, 0x0f, 0x80, 0x00, 0x00, 0x03, 0x1b };  /* RACH 45 0f 80 at d[16] */
    static const uint8_t si4[23] = {
        0x31, 0x06, 0x1c, 0x13, 0x00, 0x62, 0x00, 0x02, 0x48, 0x85, 0x51, 0x00, 0x00, 0x2b,
        0x2b, 0x2b, 0x2b, 0x2b, 0x2b, 0x2b, 0x2b, 0x2b, 0x2b };     /* a 3-digit MNC */
    static const uint8_t si13[23] = { 0x01, 0x06, 0x00, 0x80 };
    static const uint8_t si2q[23] = { 0x05, 0x06, 0x07, 0x80 };
    static const uint8_t paging[23] = {
        0x15, 0x06, 0x21, 0x00, 0x01, 0xf0, 0x2b, 0x2b, 0x2b, 0x2b, 0x2b, 0x2b, 0x2b, 0x2b,
        0x2b, 0x2b, 0x2b, 0x2b, 0x2b, 0x2b, 0x2b, 0x2b, 0x2b };
    static const uint8_t not_rr[23] = { 0x49, 0x05, 0x1b };
    static const uint8_t pag1[23] = { 0x15, 0x06, 0x21, 0x00, 0x01, 0xf0 };
    static const uint8_t ia[23] = { 0x2d, 0x06, 0x3f, 0x03 };
    static const uint8_t filler[23] = { 0x03, 0x2b, 0x2b, 0x2b };
    static const uint8_t bad_len[23] = { 0x48, 0x06, 0x1b };
    l1_cell_t *c = &lab_cell;
    static const l1_cell_t empty;

    memset(c, 0, sizeof(*c));
    si_expect("paging refused", L->si_parse(c, paging) == -1);
    si_expect("not RR refused", L->si_parse(c, not_rr) == -1);
    si_expect("bad length refused", L->si_parse(c, bad_len) == -1);
    si_expect("refused blocks leave nothing", !memcmp(c, &empty, sizeof(*c)));

    si_expect("SI 3 type", L->si_parse(c, si3) == 0x1b);
    si_expect("SI 3 MCC, MNC", c->mcc == 1 && c->mnc == 1 && c->mnc_digits == 2);
    si_expect("SI 3 LAC, CI", c->lac == 1 && c->ci == 1);
    si_expect("SI 3 CCCH", c->att && c->bs_ag_blks_res == 1 && c->ccch_conf == 0 &&
              c->bs_pa_mfrms == 3 && c->t3212 == 0x3c);
    si_expect("SI 3 selection", c->hysteresis == 3 && c->ms_txpwr_max_cch == 5 && c->neci &&
              !c->acs && c->rxlev_access_min == 0);
    si_expect("SI 3 not barred", !c->barred && c->reestablish_off);
    /* RACH 0x45 = 01 0001 0 1: max_retrans 1, tx_integer 1, not barred, RE set */
    si_expect("SI 3 RACH", c->max_retrans == 1 && c->tx_integer == 1 && c->ac == 0x0f80);
    /* C1: level 5 is 33 dBm, this phone's maximum, so nothing comes off */
    si_expect("C1 at 33 dBm", L->cell_c1(c, 38, false) == 38);
    c->ms_txpwr_max_cch = 0;            /* 39 dBm: 6 dB over */
    si_expect("C1 at 39 dBm", L->cell_c1(c, 38, false) == 32);
    c->ms_txpwr_max_cch = 15;           /* 13 dBm, below the maximum */
    c->rxlev_access_min = 10;
    si_expect("C1 with a minimum", L->cell_c1(c, 38, false) == 28);
    si_expect("C1 below the minimum", L->cell_c1(c, 5, false) == -5);
    c->ms_txpwr_max_cch = 0;            /* DCS: 30 dBm, its maximum */
    si_expect("C1 on DCS", L->cell_c1(c, 38, true) == 28);
    c->ms_txpwr_max_cch = 5;
    c->rxlev_access_min = 0;

    si_expect("SI 2 type", L->si_parse(c, si2) == 0x1a);
    si_expect("SI 2 list", c->neighbours.decoded && c->neighbours.count == 3 &&
              c->neighbours.arfcn[0] == 124 && c->neighbours.arfcn[1] == 120 &&
              c->neighbours.arfcn[2] == 1);
    si_expect("SI 2 BA, NCC", c->ba_ind && c->ncc_permitted == 0xff);

    si_expect("SI 1 type", L->si_parse(c, si1) == 0x19);
    /* si1's channel list octet d[3] = 0x80: range 1024, named not decoded */
    si_expect("SI 1 range format", !c->channels.decoded &&
              c->channels.format == L1_LIST_R1024);
    si_expect("SI 1 barred", c->barred);

    si_expect("SI 4 type", L->si_parse(c, si4) == 0x1c);
    si_expect("SI 4 3-digit MNC", c->mcc == 310 && c->mnc == 260 && c->mnc_digits == 3);
    si_expect("SI 4 LAC", c->lac == 2);
    si_expect("SI 4 selection", c->hysteresis == 2 && c->ms_txpwr_max_cch == 8 && c->acs &&
              !c->neci && c->rxlev_access_min == 5);

    /* the CCCH classifier: type byte only */
    si_expect("CCCH paging 1", L->cch_classify(pag1) == 0x21);
    si_expect("CCCH imm assign", L->cch_classify(ia) == 0x3f);
    si_expect("CCCH filler", L->cch_classify(filler) == -1);
    si_expect("SI 13 type", L->si_parse(c, si13) == 0);
    si_expect("SI 2quater type", L->si_parse(c, si2q) == 7);
    si_expect("seen", c->si_seen == (L1_SI_IDLE | L1_SI13 | L1_SI2QUATER));
    say("si check: %d of %d passed", si_checked - si_failed, si_checked);
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
        { "c3m", offsetof(params_t, c3m) }, { "slot", offsetof(params_t, slot) },
        { "schmf", offsetof(params_t, schmf) }, { "sch", offsetof(params_t, sch) },
        { "q", offsetof(params_t, q) }, { "idx", offsetof(params_t, idx) },
        { "read", offsetof(params_t, read) }, { "si", offsetof(params_t, si) },
        { "sel", offsetof(params_t, sel) }, { "stay", offsetof(params_t, stay) },
        { "mcc", offsetof(params_t, mcc) }, { "mnc", offsetof(params_t, mnc) },
        { "cch", offsetof(params_t, cch) }, { "tx", offsetof(params_t, tx) },
        { "fire", offsetof(params_t, fire) },
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

/* The minimum-power transmit test. First prove the clamp reached the DSP by
 * reading back the setpoints actually sent (every band must be the minimum);
 * only then, and only with fire=1, key one access burst on the uplink of the
 * given ARFCN. Watch it on the SDR at 890 + 0.2*arfcn MHz (an unused channel). */
static void tx_test(const params_t *p)
{
    uint16_t apc[L1_APC_BANDS][L1_APC_N];
    int bands, bad = 0, ukhz;
    uint16_t index;

    bands = L->cal_readback(apc);
    if (bands != L1_APC_BANDS) {
        say("tx: clamp NOT active (readback %d) - refusing to key", bands);
        return;
    }
    for (int b = 0; b < L1_APC_BANDS; b++)
        for (int k = 0; k < L1_APC_N; k++)
            if (apc[b][k] != 10)
                bad++;
    say("tx: APC verify - band0 %u..%u, band3 %u..%u; %d of %d != min",
        apc[0][0], apc[0][L1_APC_N - 1], apc[3][0], apc[3][L1_APC_N - 1],
        bad, L1_APC_BANDS * L1_APC_N);
    if (bad) {
        say("tx: clamp verify FAILED - refusing to key");
        return;
    }
    index = L->arfcn_index(L1_BAND_GSM900, p->tx);
    ukhz = 890000 + 200 * p->tx;
    say("tx: clamp verified. uplink arfcn %d = %d kHz, index %u", p->tx, ukhz, index);
    if (p->fire <= 0) {
        say("tx: DRY RUN, not keying. add fire=N to transmit N bursts (fire=1 = one).");
        return;
    }
    {
        int n = p->fire > 500 ? 500 : p->fire;      /* hard cap */
        int ok = 0;

        /* A single access burst is ~350 us - a one-frame flash, easy to miss on
         * an SDR. fire=N keys N bursts ~20 ms apart (a visible train, and a
         * measurable current draw) all at the calibrated minimum. */
        say("tx: FIRING %d burst(s) at the calibrated minimum, ~20 ms apart...", n);
        for (int i = 0; i < n && !stop_asked; i++) {
            if (L->access_post(index, 0, 0, 0x3f) == 0)  /* cal 0, channel request 0x3f */
                ok++;
            idle_for(20);                           /* space them; keep the clock alive */
        }
        say("tx: fired %d of %d bursts on %d kHz. watch the SDR.", ok, n, ukhz);
    }
}

static void worker(void *arg)
{
    const char *a = app_arg();
    char *buf = NULL, *seg[VARIANTS];
    int nseg = 0, rc;
    params_t base = { .arfcn = 56, .gain = 25, .cell = 0, .mf = 4, .tag = 7, .tn = 0,
                      .tsc = -1, .m = 2, .d = 0, .sync = 1, .pre = -1, .c3 = -1,
                      .slot = 0x0101, .sch = 0, .idx = -1, .tx = -1 };

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
    if (base.si) {
        si_check();
        goto done;
    }
    if (L->info->state == DSP_BOOTING || L->info->state == DSP_RUNNING) {
        say("the DSP is in use (Radio status?)");
        goto done;
    }
    if (base.tx >= 0) {                 /* arm the clamp before start() loads the cal */
        L->tx_min_power(1);
        say("tx: min-power clamp ARMED (APC setpoints forced to the calibrated minimum)");
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
    resp_seen = L->share_r16(RESP_IN_OFF);
    if ((rc = L->run()) != DSP_OK) {
        say("frame clock failed: %d", rc);
        goto stop;
    }
    posted = 4;
    idle_for(300);
    say("frame clock: %lu frames taken in 300 ms", (unsigned long)(posted - queued()));
    if (base.tx >= 0) {                 /* the transmit test runs instead of the RX variants */
        tx_test(&base);
        goto stop;
    }
    for (int v = 0; v < (nseg ? nseg : 1) && !stop_asked; v++) {
        params_t p = base;
        uint16_t index;

        if (v > 0)
            apply(&p, seg[v]);
        index = p.idx >= 0 ? (uint16_t)p.idx : L->arfcn_index(L1_BAND_GSM900, p.arfcn);
        say("== variant %d: arfcn %d (index %u)", v, p.arfcn, index);
        snprintf(status, sizeof(status), "variant %d: sync", v);
        if (v == 0 || p.sync || !synced)
            sync(&p, index);
        if (!synced)
            continue;
        if (p.c3 >= 0) {
            uint16_t c[5] = { 3, (uint16_t)p.cell, (uint16_t)p.c3m, (uint16_t)p.c3,
                              (uint16_t)p.slot };

            say("command 3 cell %d, word 2 %d, rxlev %d, slot %04x: %d", p.cell, p.c3m, p.c3,
                p.slot, L->cmd_post(c));
            idle_for(20);
        }
        if (p.sel || p.stay) {
            snprintf(status, sizeof(status), "variant %d: select", v);
            select_cell(&p, index);
            continue;
        }
        if (p.cch) {
            l1_cch_t st;

            snprintf(status, sizeof(status), "variant %d: cch", v);
            if (L->cch_read(&lab_cell, (uint32_t)p.cch * 1000u, &st) == DSP_OK)
                say("cch: %lu blocks, %lu bad; paging %lu, assign %lu, filler %lu, other %lu; "
                    "last type %02x", (unsigned long)st.blocks, (unsigned long)st.bad,
                    (unsigned long)st.paging, (unsigned long)st.imm_assign,
                    (unsigned long)st.filler, (unsigned long)st.other, st.last_type),
                say("cch: last other type %02x", st.last_other);
            else
                say("cch: not synced");
            continue;
        }
        if (p.read) {
            snprintf(status, sizeof(status), "variant %d: cell", v);
            cell_read(&p);
            continue;
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
    L->tx_min_power(0);                 /* never leave the clamp armed for a later run */
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
        ui_pop();
    }
}

/* Closed from anywhere (the shell too): the thread stops the DSP and ends. */
static void leave(screen_t *s)
{
    stop_asked = true;
}

static screen_t screen = {
    .title = "Radio lab", .right = "Close", .draw = draw, .key = key, .leave = leave,
    .tick_ms = 250,
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
