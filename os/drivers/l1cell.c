/* B310E OS — layer 1, a cell: sync and broadcast channel, receive only.
 * See os/l1cell.h and docs/gsm-plan.md, "BCCH decodes".
 *
 * System Information layouts: TS 44.018, 9.1.31 to 9.1.36, 10.5.2. Checked on
 * the phone against real blocks; os/modules/radiolab's self-check runs made-up ones.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/dsp.h"
#include "os/kernel.h"
#include "os/l1cell.h"
#include "os/lib.h"

#define FN_MOD      2715648u            /* 26 x 51 x 2048 */
#define RING_FREE   25                  /* l1_fi_free() with the ring empty */
#define AHEAD       8                   /* instructions kept queued */
#define WINDOW      26                  /* SEARCH_NUMBER_BEFORECAMPED */
#define BCCH_FIRST  2                   /* cell FN mod 51 of a BCCH block's burst 0 */
#define TAG_BCCH    7                   /* the stock's RxChannelTag */
#define RX_SCH      1
#define RX_BLOCK    7
#define SETTLE      10                  /* frames kept up after a search or a read */

/* ---- System Information --------------------------------------------------- */

/* The format from octet 1 (TS 44.018 Table 10.5.2.1b.2, the masks osmocom uses).
 * SI 1 (Cell Channel Description) and SI 2 (Neighbour Cell Description) share
 * these bits; SI 2's EXT-IND and BA-IND sit above them, which the caller reads. */
static uint8_t list_format(uint8_t o1)
{
    if ((o1 & 0xc0) == 0x00)
        return L1_LIST_BITMAP0;
    if ((o1 & 0xc8) == 0x80)
        return L1_LIST_R1024;
    if ((o1 & 0xce) == 0x88)
        return L1_LIST_R512;
    if ((o1 & 0xce) == 0x8a)
        return L1_LIST_R256;
    if ((o1 & 0xce) == 0x8c)
        return L1_LIST_R128;
    if ((o1 & 0xce) == 0x8e)
        return L1_LIST_VARBITMAP;
    return L1_LIST_UNKNOWN;
}

const char *l1_list_format(uint8_t format)
{
    static const char *const names[] = {
        "bit map 0", "range 1024", "range 512", "range 256", "range 128",
        "variable bit map", "unknown",
    };

    return names[format > L1_LIST_UNKNOWN ? L1_LIST_UNKNOWN : format];
}

/* Bit map 0: octet i (1..16) bit b (8..1) is ARFCN (16 - i) * 8 + b. The range
 * formats are named but not expanded (their tree decode is unverified here). */
static void arfcn_list(l1_arfcn_list_t *l, const uint8_t *d)
{
    l->count = 0;
    l->format = list_format(d[0]);
    l->decoded = l->format == L1_LIST_BITMAP0;
    if (!l->decoded)
        return;
    for (int arfcn = 124; arfcn >= 1; arfcn--) {
        int bit = arfcn - 1;

        if (d[15 - bit / 8] >> (bit % 8) & 1) {
            if (l->count < L1_CELL_ARFCNS)
                l->arfcn[l->count] = (uint16_t)arfcn;
            if (l->count < 0xff)
                l->count++;
        }
    }
}

static void lai(l1_cell_t *c, const uint8_t *p)
{
    uint32_t mnc3 = p[1] >> 4;

    c->mcc = (uint16_t)((p[0] & 0xf) * 100u + (p[0] >> 4) * 10u + (p[1] & 0xf));
    c->mnc = (uint16_t)((p[2] & 0xf) * 10u + (p[2] >> 4));
    c->mnc_digits = 2;
    if (mnc3 != 0xf) {
        c->mnc = (uint16_t)(c->mnc * 10u + mnc3);
        c->mnc_digits = 3;
    }
    c->lac = (uint16_t)(p[3] << 8 | p[4]);
}

static void cell_selection(l1_cell_t *c, const uint8_t *p)
{
    c->hysteresis = p[0] >> 5;
    c->ms_txpwr_max_cch = p[0] & 0x1f;
    c->acs = p[1] >> 7;
    c->neci = p[1] >> 6 & 1;
    c->rxlev_access_min = p[1] & 0x3f;
}

/* RACH Control Parameters (TS 44.018 10.5.2.29), the 3 octets inline in the SI.
 * cell_bar_access (barred) and re (reestablish_off) decide whether a phone may
 * use the cell; the rest a RACH would need, which nothing here sends. */
static void rach_control(l1_cell_t *c, const uint8_t *p)
{
    c->max_retrans = p[0] >> 6;
    c->tx_integer = (p[0] >> 2) & 0xf;
    c->barred = p[0] >> 1 & 1;
    c->reestablish_off = p[0] & 1;
    c->ac = (uint16_t)(p[1] << 8 | p[2]);
}

int si_parse(l1_cell_t *c, const uint8_t d[23])
{
    uint8_t type = d[2];

    /* the L2 pseudo length (low bits 01), then RR with skip indicator 0 */
    if ((d[0] & 3) != 1 || d[1] != 0x06)
        return -1;
    switch (type) {
    case 0x19:                          /* SI 1 */
        arfcn_list(&c->channels, d + 3);
        rach_control(c, d + 19);
        break;
    case 0x1a:                          /* SI 2 */
        arfcn_list(&c->neighbours, d + 3);
        c->ba_ind = d[3] >> 4 & 1;
        c->ncc_permitted = d[19];
        rach_control(c, d + 20);
        break;
    case 0x1b:                          /* SI 3 */
        c->ci = (uint16_t)(d[3] << 8 | d[4]);
        lai(c, d + 5);
        c->att = d[10] >> 6 & 1;
        c->bs_ag_blks_res = d[10] >> 3 & 7;
        c->ccch_conf = d[10] & 7;
        c->bs_pa_mfrms = (uint8_t)((d[11] & 7) + 2);
        c->t3212 = d[12];
        cell_selection(c, d + 14);
        rach_control(c, d + 16);
        break;
    case 0x1c:                          /* SI 4 */
        lai(c, d + 3);
        cell_selection(c, d + 8);
        rach_control(c, d + 10);
        break;
    case 0x00:                          /* SI 13: GPRS */
    case 0x02:                          /* SI 2bis */
    case 0x03:                          /* SI 2ter */
    case 0x07:                          /* SI 2quater: 2 of the 4 cells here send it */
        break;                          /* seen, not used yet */
    default:
        return -1;
    }
    c->si_seen |= 1u << type;
    return type;
}

/* MS_TXPWR_MAX_CCH as dBm (TS 45.005 4.1.1), and this phone's maximum (GSM 900
 * class 4, DCS 1800 class 1) */
#define P_900   33
#define P_1800  30

int l1_cell_c1(const l1_cell_t *c, int rxlev, bool dcs)
{
    int level = c->ms_txpwr_max_cch, dbm, over;

    if (dcs)
        dbm = level <= 15 ? 30 - 2 * level : 36 - 2 * (level - 29);
    else
        dbm = level <= 2 ? 39 : 39 - 2 * (level - 2);
    over = dbm - (dcs ? P_1800 : P_900);
    return rxlev - c->rxlev_access_min - (over > 0 ? over : 0);
}

/* ---- the frame ring -------------------------------------------------------- */

static int queued(void)
{
    return RING_FREE - l1_fi_free();
}

static void feed_idle(void)
{
    int n = AHEAD - queued();

    if (n > 0 && l1_idle_post(n) == DSP_OK)
        l1_ask_more(3);
}

void l1_cell_idle(uint32_t ms)
{
    for (uint32_t t0 = kernel_ticks(); kernel_ticks() - t0 < ms;) {
        l1_poll();
        feed_idle();
        thread_sleep(2);
    }
}

/* ---- the level of a channel ------------------------------------------------ */

#define GAIN_LOW    10u                 /* the stock's three settings (docs/gsm-plan.md) */
#define GAIN_START  25u
#define GAIN_HIGH   40u
#define RXLEV_MAX   63

int l1_rxlev_measure(uint16_t index, int samples, uint32_t budget_ms)
{
    uint32_t t0 = kernel_ticks();
    uint8_t gain = GAIN_START;
    int got = 0, sum = 0, want_more = samples;
    l1_pm_t res[4];

    while (got < samples && kernel_ticks() - t0 < budget_ms) {
        int n;

        /* The measurement frames go in the ring, not idle ones: feed_idle()
         * would fill it and leave no room, so nothing was ever measured (the
         * old bug). One at a time, so a gain a result moves counts for the next. */
        if (want_more > 0) {
            if (queued() < AHEAD) {
                int rc = l1_pm_post(&index, &gain, 1);

                if (rc != DSP_OK)
                    return rc;
                want_more--;
                l1_ask_more(3);
            }
        } else {
            feed_idle();                /* all posted: keep the clock alive */
        }
        l1_poll();
        while ((n = l1_pm_read(res, 4)) > 0) {
            for (int i = 0; i < n; i++) {
                int level = ((res[i].level & 0xfffu) + 8) >> 4;

                if (res[i].index != index)
                    continue;
                if (res[i].status == 1) {
                    sum += level > RXLEV_MAX ? RXLEV_MAX : level;
                    got++;
                } else if (res[i].status == 2) {
                    if (gain == GAIN_HIGH) {    /* too strong even at the highest */
                        sum += RXLEV_MAX;
                        got++;
                    }
                    gain = GAIN_HIGH;
                    want_more++;
                } else if (res[i].status == 3) {
                    if (gain == GAIN_LOW)       /* nothing there: rxlev 0 */
                        got++;
                    gain = GAIN_LOW;
                    want_more++;
                }
            }
        }
        if (n < 0)
            return n;
        thread_sleep(2);
    }
    return got ? (sum + got / 2) / got : 0;
}

/* FUN_0005cafe: the channel's stored level (BA entry +5, the averaged rxlev that
 * FUN_0005c7e4 also seeds the sweep table with) halved, capped at 45, or the
 * default 25 (0x0422f031) when there is none. It is the gain hint of the FCCH
 * search; before this the OS synced everything at 25 and lost weak cells. */
uint8_t l1_gain_for_rxlev(int rxlev)
{
    if (rxlev <= 0)
        return GAIN_START;
    rxlev /= 2;
    return (uint8_t)(rxlev > 45 ? 45 : rxlev);
}

/* ---- sync ------------------------------------------------------------------ */

/* GSM 05.02 SCH information bits, the order OsmocomBB decodes them in */
static void sch_decode(const uint8_t *d, uint32_t *bsic, uint32_t *fn)
{
    uint32_t t1 = (uint32_t)(d[0] & 3) << 9 | (uint32_t)d[1] << 1 | d[2] >> 7;
    uint32_t t2 = (d[2] >> 2) & 0x1f;
    uint32_t t3 = ((uint32_t)(d[2] & 3) << 1 | (d[3] & 1)) * 10 + 1;

    *bsic = d[0] >> 2;
    *fn = 51 * ((t3 + 26 - t2) % 26) + t3 + 51 * 26 * t1;
}

int l1_cell_sync(l1_cell_t *c, uint16_t index, uint8_t gain, int slot, uint32_t budget_ms)
{
    uint32_t t0 = kernel_ticks(), end_fn, t_post, fail_ms;
    l1_rx_t rx;
    int rc;

    memset(c, 0, sizeof(*c));
    c->index = index;
    c->gain = gain;
    c->slot = (uint8_t)slot;
    l1_cell_gain_set(slot, gain);       /* its BCCH/CCCH/SDCCH reads run at it too */
    while (queued() > 2 && kernel_ticks() - t0 < 200) {     /* nearly drained */
        l1_poll();
        thread_sleep(2);
    }
    while (l1_rx_read(&rx) == 1)        /* nothing old */
        ;
    /* A channel with no BCCH gives up once the search has surely run: what was
     * queued, the window, SETTLE and a margin, at 4.615 ms a frame. The frame test
     * below hardly ever fired (feed_idle keeps AHEAD queued, never AHEAD / 2), so
     * every such channel took the whole budget: 1.5 s each in Register's search. */
    fail_ms = (uint32_t)(queued() + WINDOW + SETTLE + 20) * 5u;
    if ((rc = l1_fcch_post(index, gain, WINDOW, slot)) != DSP_OK)
        return rc;
    t_post = kernel_ticks();
    end_fn = (l1_next_fn() + SETTLE) % FN_MOD;
    /* No type 10 reads: one that fails sets the DSP's frequency back to its
     * centre value when it is not camped, and later reads fail (phone). The
     * search returns its own SCH. */
    while (kernel_ticks() - t0 < budget_ms) {
        feed_idle();
        l1_poll();
        while ((rc = l1_rx_read(&rx)) == 1) {
            uint32_t local, fn, bsic;
            int back = (int)(rx.hdr[4] >> 8) - (int)(rx.hdr[4] & 0xff);

            /* +0x08 is set by a search: hi - lo frames between the burst and
             * the reported frame (FUN_000461e4) */
            if (rx.hdr[0] != RX_SCH || rx.hdr[1] != 0 || !rx.hdr[4] || back < 0)
                continue;
            local = (rx.hdr[2] | (uint32_t)(rx.hdr[3] & 0xff) << 16) + FN_MOD - (uint32_t)back;
            sch_decode(rx.data, &bsic, &fn);
            c->bsic = (uint8_t)bsic;
            c->fn_offset = (fn + FN_MOD - local % FN_MOD) % FN_MOD;
            c->synced = true;
            return 1;
        }
        if (rc < 0)
            return rc;
        if ((l1_next_fn() + FN_MOD - end_fn) % FN_MOD < FN_MOD / 2 &&
            (queued() <= AHEAD / 2 || kernel_ticks() - t_post >= fail_ms))
            break;                      /* the window has gone by, and no SCH */
        thread_sleep(2);
    }
    return 0;
}

/* ---- the broadcast channel ------------------------------------------------- */

static void take_blocks(l1_cell_t *c)
{
    l1_rx_t rx;

    while (l1_rx_read(&rx) == 1) {
        if (rx.hdr[0] != RX_BLOCK)
            continue;
        if (rx.hdr[1] != 0)
            c->bad++;
        else if (si_parse(c, rx.data) >= 0)
            c->good++;
        else
            c->other++;                 /* someone else's: not kept */
    }
}

int l1_cell_read(l1_cell_t *c, int max_blocks, uint32_t want, uint32_t budget_ms)
{
    uint32_t t0 = kernel_ticks(), good0 = c->good, last_fn0 = 0;
    int posted = 0, burst = -1;
    bool stop = false;

    if (!c->synced)
        return DSP_ECMD;
    while (kernel_ticks() - t0 < budget_ms) {
        bool batch = false;

        while (queued() < AHEAD && !stop && (posted < max_blocks || burst >= 0)) {
            uint32_t local = l1_next_fn();
            int rc;

            if (burst < 0 && (local + c->fn_offset) % FN_MOD % 51 == BCCH_FIRST) {
                burst = 0;
                last_fn0 = local;
            }
            if (burst >= 0) {
                rc = l1_burst_post(c->index, TAG_BCCH, burst, 0, c->bsic & 7, c->slot);
                if (rc == DSP_OK && ++burst == 4) {
                    burst = -1;
                    posted++;
                }
            } else {
                rc = l1_idle_post(1);
            }
            if (rc != DSP_OK)
                return rc;
            batch = true;
        }
        if (batch)
            l1_ask_more(3);
        l1_poll();
        take_blocks(c);
        if (want && (c->si_seen & want) == want)
            stop = true;                /* and let what is posted come back */
        if ((stop || posted >= max_blocks) && burst < 0) {
            feed_idle();
            if (last_fn0 && (l1_next_fn() + FN_MOD - last_fn0) % FN_MOD > 30)
                break;
            if (!last_fn0)
                break;
        }
        thread_sleep(2);
    }
    take_blocks(c);
    return (int)(c->good - good0);
}

/* ---- the common control channel (paging, access grant) --------------------- */

/* CCCH blocks of the non-combined configuration (CCCH_CONF 0, all the cells here):
 * nine 4-burst blocks per 51-multiframe, at these cell FN mod 51. */
static bool cch_start(uint32_t m)
{
    return m == 6 || m == 12 || m == 16 || m == 22 || m == 26 ||
           m == 32 || m == 36 || m == 42 || m == 46;
}

int l1_cch_classify(const uint8_t d[23])
{
    /* L2 pseudo length 0, or not RR: a fill or null frame, no one being called */
    if ((d[0] >> 2) == 0 || d[1] != 0x06)
        return -1;
    return d[2];                        /* the message type; identities not read */
}

/* Header +0x0e of the good CCCH blocks, summed: a signed halfword the stock
 * logs, SNR or frequency error [G]. Diagnostic only (l1_cch_hdr7). */
static int32_t hdr7_sum;
static uint32_t hdr7_n;

void l1_cch_hdr7(int32_t *sum, uint32_t *n)
{
    *sum = hdr7_sum;
    *n = hdr7_n;
    hdr7_sum = 0;
    hdr7_n = 0;
}

static void take_cch(l1_cch_t *st)
{
    l1_rx_t rx;

    while (l1_rx_read(&rx) == 1) {
        int type;

        if (rx.hdr[0] != RX_BLOCK && rx.hdr[0] != 8)
            continue;
        if (rx.hdr[1] != 0) {
            st->bad++;
            continue;
        }
        hdr7_sum += (int16_t)rx.hdr[7];
        hdr7_n++;
        st->blocks++;
        type = l1_cch_classify(rx.data);
        if (type < 0) {
            st->filler++;
            continue;
        }
        st->last_type = (uint8_t)type;
        if (type == 0x21 || type == 0x22 || type == 0x24)
            st->paging++;
        else if (type == 0x3f || type == 0x3a || type == 0x3b)
            st->imm_assign++;
        else {
            st->other++;
            st->last_other = (uint8_t)type;
        }
    }
}

int l1_cch_read(l1_cell_t *c, uint32_t ms, l1_cch_t *st)
{
    uint32_t t0 = kernel_ticks();
    int burst = -1;

    memset(st, 0, sizeof(*st));
    if (!c->synced)
        return DSP_ECMD;
    /* past ms, only the block already started is completed: the caller may post a
     * RACH next, which must not land inside a half-posted block */
    for (;;) {
        bool batch = false, over = kernel_ticks() - t0 >= ms;

        if (over && burst < 0)
            break;
        while (queued() < AHEAD && !(over && burst < 0)) {
            uint32_t m = (l1_next_fn() + c->fn_offset) % FN_MOD % 51;
            int rc;

            if (burst < 0 && !over && cch_start(m))
                burst = 0;
            if (burst >= 0) {
                rc = l1_burst_post(c->index, TAG_BCCH, burst, 0, c->bsic & 7, c->slot);
                if (rc == DSP_OK && ++burst == 4)
                    burst = -1;
            } else {
                rc = l1_idle_post(1);
            }
            if (rc != DSP_OK)
                return rc;
            batch = true;
        }
        if (batch)
            l1_ask_more(3);
        l1_poll();
        take_cch(st);
        thread_sleep(2);
    }
    take_cch(st);
    return DSP_OK;
}

/* ---- dedicated channel (SDCCH/8) ------------------------------------------- */

#define TAG_DCH     9                   /* dedicated-channel RxChannelTag (stock) */

/* SDCCH/8 schedule (GSM 05.02): subchannel k's downlink block occupies FN mod 51
 * in [k*4, k*4+3]; the uplink is the same pattern offset by 15 frames. SACCH/C8
 * (its 102-multiframe alternation) and fill frames on idle uplinks are a
 * refinement for registration bring-up. */
static int sdcch_burst(int base, uint32_t m)
{
    int b = (int)m - base;

    return b >= 0 && b < 4 ? b : -1;
}

/* One dedicated-channel session of ms: post the subchannel's downlink rx bursts,
 * and when ul is given its uplink normal bursts, and collect the decoded downlink
 * blocks into st. The osmocom DATA_IND/DATA_REQ pump for SDCCH, on the verified rx
 * (l1_burst_post) and uplink (l1_uplink_post) primitives. The uplink block framing
 * (one TXBUFQ write vs per burst) and the dedicated tag are confirmed at the
 * cleared registration; it is HW-testable only there. */
int l1_dm_sched(l1_cell_t *c, int subch, int tn, int tsc, uint8_t bsic,
                uint32_t ms, const uint8_t *ul, l1_dm_t *st)
{
    uint32_t t0 = kernel_ticks();
    int dlbase = (subch & 7) * 4;
    int ulbase = (dlbase + 15) % 51;
    int ul_posted = 0;                  /* uplink bursts of this block sent, 0..4 */
    bool stop = false;

    memset(st, 0, sizeof(*st));
    if (!c->synced)
        return DSP_ECMD;
    /* One call sends at most one complete uplink block - 4 bursts, aligned to the
     * uplink window so the network sees a whole LAPDm frame - and returns as soon
     * as one downlink block has come, so the caller sees every block and drives
     * the SABM retransmission on its own timer. Uplink burst N goes only when the
     * window is at position N, so a late start waits a multiframe rather than
     * sending a partial frame. */
    while (!stop && kernel_ticks() - t0 < ms) {
        bool batch = false;

        /* Shallow ring (2) only while posting the uplink block - the TXBUFQ frees
         * a slot only when the DSP transmits a burst, so far-ahead posting fills
         * it at the second burst. When only receiving, a deep ring (AHEAD) keeps
         * the downlink timing steady so it decodes (a shallow ring jitters it). */
        while (queued() < (ul ? 2 : AHEAD)) {
            uint32_t m = (l1_next_fn() + c->fn_offset) % FN_MOD % 51;
            int db = sdcch_burst(dlbase, m), ub = sdcch_burst(ulbase, m);
            int rc;

            if (db >= 0) {
                rc = l1_burst_post(c->index, TAG_DCH, db, tn, tsc, c->slot);
                if (rc == DSP_OK)
                    st->dl_bursts++;
            } else if (ul && ul_posted < 4 && ub == ul_posted) {
                rc = l1_uplink_post(c->index, TAG_DCH, ub, tn, tsc, c->slot, bsic, ul);
                if (rc == DSP_OK) {
                    st->ul_bursts++;
                    ul_posted++;
                } else if (rc == DSP_EFULL) {
                    rc = l1_idle_post(1);   /* ring not drained: drop this block,
                                             * keep the clock fed, retry next window */
                    ul_posted = 0;
                }
            } else {
                rc = l1_idle_post(1);
            }
            if (rc != DSP_OK)
                return rc;
            batch = true;
        }
        if (batch)
            l1_ask_more(3);
        l1_poll();
        {
            l1_rx_t rx;

            while (l1_rx_read(&rx) == 1) {
                st->dl_seen++;
                st->dl_hdr0 = (uint8_t)rx.hdr[0];
                /* a dedicated block comes back tagged with the RxChannelTag we
                 * asked for (TAG_DCH = 9), not the BCCH 7/8 - accept it, or every
                 * SDCCH block (incl. the UA) is dropped. */
                if (rx.hdr[0] != RX_BLOCK && rx.hdr[0] != 8 && rx.hdr[0] != TAG_DCH)
                    continue;
                if (rx.hdr[1] != 0) {
                    st->dl_bad++;
                    st->dl_bad_fn = ((rx.hdr[2] | (uint32_t)(rx.hdr[3] & 0xff) << 16)
                                     + c->fn_offset) % FN_MOD % 51;
                    continue;
                }
                st->dl_blocks++;
                st->dl_fn = ((rx.hdr[2] | (uint32_t)(rx.hdr[3] & 0xff) << 16)
                             + c->fn_offset) % FN_MOD % 51;
                memcpy(st->last_dl, rx.data, sizeof(st->last_dl));
                st->has_dl = 1;
            }
        }
        if (st->has_dl && (!ul || ul_posted >= 4))
            stop = true;               /* a block is in and the uplink is complete */
        thread_sleep(2);
    }
    st->ul_done = ul && ul_posted >= 4;
    return DSP_OK;
}

/* ---- selection -------------------------------------------------------------- */

#define SELECT_SAMPLES   8
#define SELECT_MEASURE_MS 400u
#define SELECT_SYNC_MS   1500u
#define SELECT_BLOCKS    12
#define SELECT_READ_MS   4000u

int l1_cell_select(const uint16_t *index, int n, uint16_t mcc, uint16_t mnc, l1_cell_t *cells)
{
    int best = -1;

    for (int i = 0; i < n; i++) {
        l1_cell_t *c = &cells[i];
        int rxlev = l1_rxlev_measure(index[i], SELECT_SAMPLES, SELECT_MEASURE_MS);
        int rc = l1_cell_sync(c, index[i], l1_gain_for_rxlev(rxlev), i, SELECT_SYNC_MS);

        c->rxlev = rxlev >= 0 ? (int16_t)rxlev : -1;
        c->suitable = false;
        if (rc != 1)
            continue;
        if (l1_cell_read(c, SELECT_BLOCKS, L1_SI3 | L1_SI4, SELECT_READ_MS) < 0)
            continue;
        if (!(c->si_seen & (L1_SI3 | L1_SI4)) || c->rxlev < 0)
            continue;
        c->c1 = (int16_t)l1_cell_c1(c, c->rxlev, false);
        c->suitable = !c->barred && c->c1 > 0 && (!mcc || (c->mcc == mcc && c->mnc == mnc));
        if (c->suitable && (best < 0 || c->c1 > cells[best].c1))
            best = i;
    }
    return best;
}

/* ---- staying on a cell ------------------------------------------------------ */

#define STAY_BLOCKS      4              /* about a second */
#define STAY_MEASURE_MS  5000u
#define STAY_BAD_ROUNDS  2              /* whole rounds bad: search again */

int l1_cell_stay(l1_cell_t *c, uint32_t ms, l1_stay_t *st)
{
    uint32_t t0 = kernel_ticks(), last_measure = t0 - STAY_MEASURE_MS;
    int bad_rounds = 0;

    memset(st, 0, sizeof(*st));
    st->rxlev_min = 0x7fff;
    st->rxlev_max = st->rxlev_last = -1;
    if (!c->synced)
        return DSP_ECMD;
    while (kernel_ticks() - t0 < ms) {
        uint32_t good = c->good, bad = c->bad;
        int rc;

        if (kernel_ticks() - last_measure >= STAY_MEASURE_MS) {
            int rxlev = l1_rxlev_measure(c->index, SELECT_SAMPLES, SELECT_MEASURE_MS);

            if (rxlev < 0)
                return rxlev;
            last_measure = kernel_ticks();
            st->measures++;
            st->rxlev_last = (int16_t)rxlev;
            if (rxlev < st->rxlev_min)
                st->rxlev_min = (int16_t)rxlev;
            if (rxlev > st->rxlev_max)
                st->rxlev_max = (int16_t)rxlev;
            /* the stock's camped AGC: the cell's reads follow its level */
            c->gain = l1_gain_for_rxlev(rxlev);
            l1_cell_gain_set(c->slot, c->gain);
        }
        if ((rc = l1_cell_read(c, STAY_BLOCKS, 0, 2000u)) < 0)
            return rc;
        st->good += c->good - good;
        st->bad += c->bad - bad;
        bad_rounds = c->good == good ? bad_rounds + 1 : 0;
        if (bad_rounds >= STAY_BAD_ROUNDS) {
            static l1_cell_t again;     /* one caller at a time, as for the DSP */

            st->resyncs++;
            bad_rounds = 0;
            if ((rc = l1_cell_sync(&again, c->index, c->gain, c->slot, SELECT_SYNC_MS)) != 1) {
                st->lost = true;
                return rc;
            }
            if (again.fn_offset != c->fn_offset)
                st->slips++;
            c->fn_offset = again.fn_offset;
            c->bsic = again.bsic;
        }
    }
    return DSP_OK;
}
