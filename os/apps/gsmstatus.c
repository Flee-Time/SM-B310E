/* B310E OS — Tools / Radio status: a read-only look at the GSM side.
 *
 * One short-lived thread, every step bounded:
 *  1. SIM: for each slot, whether a card answers and whether it is ready for
 *     GSM (READ BINARY of EF_IMSI answers 9000) or wants its PIN (98 04).
 *     The IMSI itself is read into a scratch buffer and dropped: nothing from
 *     the card is shown, logged or saved.
 *  2. DSP: boot from this phone's NOR, then layer 1 (parameter records).
 *  3. Frame clock: l1_run(), then 400 ms of idle frames kept fed; a running
 *     DSP takes one per 4.615 ms (about 86).
 *  4. Power scan: three sweeps over every arfcn index (971, five bands) with
 *     power-measurement frames (type 11), the stock's receive-only
 *     measurement. Nothing is transmitted: no TX buffer is written.
 *  5. The DSP is held in reset again, and the whole result goes to
 *     /b310e-os/radio-report.txt (a CSV of every channel that answered).
 *
 * Unknowns this screen exists to settle on the phone (docs/gsm-plan.md):
 * whether the DSP program runs at all, the band order of the arfcn index,
 * whether a bigger rxlev is stronger or weaker, and which saturation status
 * means "too strong". The report has the raw numbers; compare it with the
 * stock firmware's field-test screen (serving ARFCN and RXLEV) on the same
 * spot. The gain hint's start value and its moves on saturation are guesses.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/dsp.h"
#include "os/fs.h"
#include "os/kernel.h"
#include "os/lib.h"
#include "os/sim.h"
#include "os/ui.h"

screen_t *app_radio_status(void);

#define CHANNELS        971u            /* RF_TOTAL_CHANS */
#define SWEEPS          3u
#define QUEUE_AHEAD     8               /* frame instructions kept queued */
/* The stock's three settings (RAM 0x0422f030..32, read live from stock in
 * QEMU: 10, 25, 40). Every channel starts at the middle one (FUN_0005c0b8);
 * status 3 measures again at the low one, status 2 at the high one; status 3
 * at the low one counts as rxlev 0, nothing there (FUN_0005c3e2). */
#define GAIN_LOW        10u
#define GAIN_START      25u
#define GAIN_HIGH       40u
#define STALL_MS        300u            /* no frame taken for this long: stop */
#define SCAN_BUDGET_MS  15000u
#define TOP             5
#define FI_RING_FREE    25              /* l1_fi_free() with the ring empty */
#define MIN_FRAMES      60              /* of ~86 in 400 ms at 4.615 ms */

#define SYNC_CELLS      3               /* the strongest 900 channels tried */
#define SYNC_WINDOW     26              /* SEARCH_NUMBER_BEFORECAMPED */
#define SYNC_SCH        51              /* SCH reads after it: one multiframe */
#define SYNC_BUDGET_MS  1500u
#define SYNC_KEEP       24              /* raw blocks kept for the report */
#define FN_MOD          2715648u        /* 26 x 51 x 2048 */
#define BAND900_END     174u            /* arfcn indexes 0..173 are GSM 900 */

enum { R_IDLE, R_SIM, R_DSP, R_L1, R_CLOCK, R_SCAN, R_SYNC, R_BCCH, R_SAVE, R_DONE };
enum { SIM_NONE, SIM_READY, SIM_PIN, SIM_ERROR };

/* Words only, for os/tests/test-radio.py (ELF symbol radio_status). Append
 * only. */
typedef struct {
    uint32_t runs;                      /* finished runs */
    uint32_t busy;
    uint32_t phase;                     /* R_* */
    uint32_t sim[SIM_SLOTS];            /* SIM_* */
    int32_t sim_sw[SIM_SLOTS];          /* READ BINARY EF_IMSI: SW1SW2 or an error */
    uint32_t sim_mv[SIM_SLOTS];
    int32_t dsp_result;                 /* dsp_boot(); 1: not run */
    int32_t l1_result;                  /* l1_start(); 1: not run */
    uint32_t records;                   /* parameter records the DSP took */
    uint32_t frames_taken;              /* idle frames taken in 400 ms */
    uint32_t frame_us;
    uint32_t sweeps;                    /* power sweeps completed */
    uint32_t pm_frames;                 /* power-measurement frames posted */
    uint32_t pm_results;
    uint32_t pm_valid;                  /* status 1 */
    uint32_t pm_sat_hi, pm_sat_lo;      /* status 2, 3 */
    uint32_t pm_other;                  /* any other status */
    uint32_t pm_stray;                  /* an index outside 0..970 */
    uint32_t channels_heard;            /* indexes with a valid result */
    uint32_t stalled;                   /* the scan stopped: frames not taken */
    uint32_t top_index[TOP];            /* highest raw levels, 0x3ff none */
    uint32_t top_level[TOP];            /* bits 11:4 rxlev in 1/16 */
    uint32_t saved;                     /* the report is on the card */
    uint32_t elapsed_ms;
    uint32_t pm_floor;                  /* status 3 at GAIN_LOW: rxlev 0 */
    uint32_t sync_tried;                /* cells an FCCH search was posted for */
    uint32_t sync_rx;                   /* received blocks in the sync step */
    uint32_t sync_rx_types;             /* bit n: a block of type n came */
    uint32_t sync_msgs;                 /* DSP messages (asserts) during it */
    uint32_t sync_index[SYNC_CELLS];    /* arfcn index, 0x3ff none */
    uint32_t sync_sch[SYNC_CELLS];      /* good SCH blocks (type 1, bfi 0) */
    uint32_t sync_bsic[SYNC_CELLS];     /* from the first good SCH, 0xff none */
    uint32_t sync_fn[SYNC_CELLS];       /* its frame number */
    uint32_t sync_local[SYNC_CELLS];    /* our frame count of its burst */
    uint32_t sync_back[SYNC_CELLS];     /* frames taken off the reported count */
    uint32_t sync_exact[SYNC_CELLS];    /* 1: header +0x08 was 0, no correction */
    uint32_t sync_msg_last;             /* the last DSP message: code << 16 | param */
    uint32_t bcch_cell;                 /* sync cell read, 0xff none */
    uint32_t bcch_blocks;               /* 4-burst blocks posted */
    uint32_t bcch_rx;                   /* received blocks in the step */
    uint32_t bcch_resync;               /* good SCH in the sync just before it */
    uint32_t bcch_good[4];              /* type 7/8, bfi 0, by variant (bcch_tag/delta) */
    uint32_t bcch_bad[4];               /* bfi 1 */
    uint32_t bcch_rx_types;             /* bit n: a block of type n came */
    uint32_t si_seen;                   /* bit n: SI message type 0x18 + n */
    uint32_t mcc, mnc, mnc_digits, lac, cell_id;    /* from SI 3 / SI 4; 0 none */
    uint32_t bcch_msgs;                 /* DSP messages during the step */
    uint32_t bcch_consumed;             /* frames the DSP took in it */
    uint32_t bcch_last_move_ms;         /* when it took the last one, from the start */
    uint32_t bcch_ms;                   /* how long the step ran */
} radio_status_t;

volatile radio_status_t radio_status;

typedef struct {
    uint16_t best;                      /* highest level & 0xfff seen */
    uint8_t valid, sat, gain, floor;
} chan_t;

static chan_t *chan;

/* The inverse of l1_arfcn_index(): the same [G] band order. */
static const char *band_of(uint32_t idx, int *arfcn)
{
    if (idx < 125)
        return *arfcn = (int)idx, "900";
    if (idx < 174)
        return *arfcn = (int)(975 + idx - 125), "900";
    if (idx < 548)
        return *arfcn = (int)(512 + idx - 174), "1800";
    if (idx < 847)
        return *arfcn = (int)(512 + idx - 548), "1900";
    if (idx < CHANNELS)
        return *arfcn = (int)(128 + idx - 847), "850";
    return *arfcn = -1, "?";
}

/* ---- 1. SIM -------------------------------------------------------------- */

static void sim_check(int slot)
{
    static const uint8_t sel[5] = { 0xa0, 0xa4, 0x00, 0x00, 0x02 };
    static const uint8_t mf[2] = { 0x3f, 0x00 }, df_gsm[2] = { 0x7f, 0x20 },
        ef_imsi[2] = { 0x6f, 0x07 };
    static const uint8_t read_imsi[5] = { 0xa0, 0xb0, 0x00, 0x00, 0x09 };
    uint8_t scratch[9];
    sim_atr_t atr;
    int sw;

    radio_status.sim[slot] = SIM_NONE;
    radio_status.sim_sw[slot] = 0;
    radio_status.sim_mv[slot] = 0;
    if (sim_activate(slot, &atr) != SIM_OK)
        return;
    radio_status.sim_mv[slot] = atr.mv;
    sw = sim_apdu(slot, sel, mf, NULL, NULL);
    if (sw > 0)
        sw = sim_apdu(slot, sel, df_gsm, NULL, NULL);
    if (sw > 0)
        sw = sim_apdu(slot, sel, ef_imsi, NULL, NULL);
    if (sw > 0)
        sw = sim_apdu(slot, read_imsi, NULL, scratch, NULL);
    memset(scratch, 0, sizeof(scratch));
    sim_deactivate(slot);
    radio_status.sim_sw[slot] = sw;
    radio_status.sim[slot] = sw == 0x9000 ? SIM_READY : sw == 0x9804 ? SIM_PIN : SIM_ERROR;
}

/* ---- 3. frame clock ------------------------------------------------------ */

/* Once l1_run() starts the frame clock the ring must never run dry: every
 * instruction posted is counted, so the DSP has taken posted - queued. */
static uint32_t posted;

static uint32_t queued(void)
{
    return (uint32_t)(FI_RING_FREE - l1_fi_free());
}

static uint32_t consumed(void)
{
    return posted - queued();
}

/* Tops the ring up to QUEUE_AHEAD with idle frames, the probe's way
 * (dsp.c watch_frames): the ask-more flag 3 back after each batch. */
static void feed_idle(void)
{
    int n = QUEUE_AHEAD - (int)queued();

    if (n > 0 && l1_idle_post(n) == DSP_OK) {
        posted += (uint32_t)n;
        l1_ask_more(3);
    }
}

static int clock_check(void)
{
    uint32_t first_t = 0, last_t = 0, first_c = 0, last_c = 0;
    int rc;

    posted = 0;
    if ((rc = l1_run()) != DSP_OK)
        return rc;
    posted = 4;                         /* l1_run()'s idle frames */
    for (uint32_t t = 0; t < 400; t++) {
        uint32_t c;

        thread_sleep(1);
        l1_poll();
        feed_idle();
        c = consumed();
        if (c != last_c) {
            if (!first_t)
                first_t = kernel_ticks(), first_c = c;
            last_t = kernel_ticks();
            last_c = c;
        }
    }
    radio_status.frames_taken = last_c;
    if (last_c > first_c)
        radio_status.frame_us = (last_t - first_t) * 1000 / (last_c - first_c);
    return DSP_OK;
}

/* ---- 4. power scan ------------------------------------------------------- */

static void take(const l1_pm_t *r)
{
    chan_t *c;

    radio_status.pm_results++;
    if (r->index >= CHANNELS) {
        radio_status.pm_stray++;
        return;
    }
    c = &chan[r->index];
    switch (r->status) {
    case 1:
        radio_status.pm_valid++;
        if (!c->valid++)
            radio_status.channels_heard++;
        if ((r->level & 0xfffu) > c->best)
            c->best = r->level & 0xfffu;
        break;
    case 2:                             /* too strong: measure at the high one */
        radio_status.pm_sat_hi++;
        c->sat++;
        c->gain = GAIN_HIGH;
        break;
    case 3:                             /* too weak */
        if (c->gain == GAIN_LOW) {
            radio_status.pm_floor++;
            c->floor++;
        } else {
            radio_status.pm_sat_lo++;
            c->sat++;
            c->gain = GAIN_LOW;
        }
        break;
    default:
        radio_status.pm_other++;
        break;
    }
}

static void scan(void)
{
    uint32_t next = 0, total = SWEEPS * CHANNELS, t0 = kernel_ticks(), moved_at = t0;
    uint32_t last = consumed(), end = 0;
    l1_pm_t res[8];

    for (;;) {
        uint32_t now = kernel_ticks();
        bool batch = false;
        int n;

        while (next < total && queued() < QUEUE_AHEAD) {
            uint16_t idx[L1_PM_PER_FRAME];
            uint8_t gain[L1_PM_PER_FRAME];
            int k = 0;

            do {                        /* a frame never spans two sweeps */
                uint32_t c = next++ % CHANNELS;

                idx[k] = (uint16_t)c;
                gain[k++] = chan[c].gain;
            } while (k < L1_PM_PER_FRAME && next < total && next % CHANNELS);
            if (l1_pm_post(idx, gain, k) != DSP_OK)
                break;
            posted++;
            batch = true;
            radio_status.pm_frames++;
            if (next >= total)
                end = posted;
        }
        if (batch)
            l1_ask_more(3);
        if (next >= total)
            feed_idle();                /* the clock keeps running */
        l1_poll();
        while ((n = l1_pm_read(res, 8)) > 0)
            for (int i = 0; i < n; i++)
                take(&res[i]);
        radio_status.sweeps = next / CHANNELS;
        if (consumed() != last) {
            last = consumed();
            moved_at = now;
        }
        /* the last measurement frame taken, and a few frames for its result */
        if (end && last >= end + 4)
            break;
        if (now - moved_at > STALL_MS || now - t0 > SCAN_BUDGET_MS) {
            radio_status.stalled = 1;
            break;
        }
        thread_sleep(2);
    }
    /* results of the last frames */
    for (int i = 0; i < 10; i++) {
        thread_sleep(2);
        feed_idle();
    }
    l1_poll();
    {
        int n;

        while ((n = l1_pm_read(res, 8)) > 0)
            for (int i = 0; i < n; i++)
                take(&res[i]);
    }
    radio_status.sweeps = radio_status.stalled ? next / CHANNELS : SWEEPS;

    for (int k = 0; k < TOP; k++) {
        uint32_t best = 0, bi = L1_NO_INDEX;

        for (uint32_t i = 0; i < CHANNELS; i++) {
            bool taken = false;

            for (int j = 0; j < k; j++)
                taken |= radio_status.top_index[j] == i;
            if (chan[i].valid && !taken && (bi == L1_NO_INDEX || chan[i].best > best))
                best = chan[i].best, bi = i;
        }
        radio_status.top_index[k] = bi;
        radio_status.top_level[k] = bi == L1_NO_INDEX ? 0 : best;
    }
}

/* ---- 5. sync ------------------------------------------------------------- */

/* The first try at the FCCH/SCH stage, receive only. For each of the
 * strongest 900 channels: one FCCH search (type 9) over 26 frames, then an
 * SCH read (type 10) in each of the next 51 frames, so one of them falls on
 * the cell's SCH whatever its timing. Every block that comes back is counted
 * and the first few kept raw: what the DSP returns after a type 9 is not
 * known from the stock binary (docs/gsm-plan.md). */
static struct {
    uint8_t cell;
    l1_rx_t rx;
} kept[SYNC_KEEP];
static uint32_t kept_n;

/* GSM 05.02 SCH information bits, the order OsmocomBB decodes them in. */
static void sch_decode(const uint8_t *d, uint32_t *bsic, uint32_t *fn)
{
    uint32_t t1 = (uint32_t)(d[0] & 3) << 9 | (uint32_t)d[1] << 1 | d[2] >> 7;
    uint32_t t2 = (d[2] >> 2) & 0x1f;
    uint32_t t3 = ((uint32_t)(d[2] & 3) << 1 | (d[3] & 1)) * 10 + 1;

    *bsic = d[0] >> 2;
    *fn = 51 * ((t3 + 26 - t2) % 26) + t3 + 51 * 26 * t1;
}

static void take_rx(int k)
{
    l1_rx_t rx;

    while (l1_rx_read(&rx) == 1) {
        radio_status.sync_rx++;
        if (rx.hdr[0] < 32)
            radio_status.sync_rx_types |= 1u << rx.hdr[0];
        if (kept_n < SYNC_KEEP) {
            kept[kept_n].cell = (uint8_t)k;
            kept[kept_n++].rx = rx;
        }
        /* Header +0x08 (hdr[4]) hi - lo: how far before the reported frame
         * the burst was (the stock's FUN_000461e4 takes hi - lo). On the phone
         * a block with +0x08 = 0 gave the exact offset, and one with 0x1a07
         * two frames earlier needed hi - lo - 1, so that is used, and a block
         * with +0x08 = 0 replaces a corrected one. */
        if (rx.hdr[0] == 1 && rx.hdr[1] == 0) {
            bool exact = rx.hdr[4] == 0;
            int back = exact ? 0 : (int)(rx.hdr[4] >> 8) - (int)(rx.hdr[4] & 0xff) - 1;
            uint32_t bsic, fn;

            if (back < 0)
                back = 0;
            if (!radio_status.sync_sch[k]++ || (exact && !radio_status.sync_exact[k])) {
                uint32_t local = rx.hdr[2] | (uint32_t)(rx.hdr[3] & 0xff) << 16;

                sch_decode(rx.data, &bsic, &fn);
                radio_status.sync_bsic[k] = bsic;
                radio_status.sync_fn[k] = fn;
                radio_status.sync_local[k] = (local + FN_MOD - (uint32_t)back) % FN_MOD;
                radio_status.sync_back[k] = (uint32_t)back;
                radio_status.sync_exact[k] = exact;
            }
        }
    }
}

static void sync_cell(int k, uint16_t index)
{
    uint32_t t0 = kernel_ticks(), end = 0;
    int sch = 0;

    while (queued() > 2 && kernel_ticks() - t0 < 200) {    /* the ring nearly drained */
        thread_sleep(2);
        take_rx(k);
    }
    if (l1_fcch_post(index, chan[index].gain, SYNC_WINDOW, k) != DSP_OK)
        return;
    posted++;
    radio_status.sync_tried++;
    while (kernel_ticks() - t0 < SYNC_BUDGET_MS) {
        bool batch = false;

        while (sch < SYNC_SCH && queued() < QUEUE_AHEAD) {
            if (l1_sch_post(index, chan[index].gain, k) != DSP_OK)
                break;
            posted++;
            batch = true;
            if (++sch == SYNC_SCH)
                end = posted;
        }
        if (batch)
            l1_ask_more(3);
        if (sch == SYNC_SCH)
            feed_idle();
        l1_poll();
        take_rx(k);
        if (end && consumed() >= end + 10)
            break;
        thread_sleep(2);
    }
}

static void sync(void)
{
    uint32_t msgs = dsp_probe_info.msgs;

    kept_n = 0;
    for (int k = 0; k < SYNC_CELLS; k++) {
        uint32_t best = 0, bi = L1_NO_INDEX;

        for (uint32_t i = 0; i < BAND900_END; i++) {
            bool taken = false;

            for (int j = 0; j < k; j++)
                taken |= radio_status.sync_index[j] == i;
            if (chan[i].valid && !taken && (bi == L1_NO_INDEX || chan[i].best > best))
                best = chan[i].best, bi = i;
        }
        radio_status.sync_index[k] = bi;
        if (bi != L1_NO_INDEX)
            sync_cell(k, (uint16_t)bi);
    }
    radio_status.sync_msgs = dsp_probe_info.msgs - msgs;
    radio_status.sync_msg_last = dsp_probe_info.msg_last;
}

/* ---- 6. broadcast channel ------------------------------------------------- */

/* On the first synced cell: the 4 BCCH bursts at cell FN mod 51 = 2..5 of
 * BCCH_MF multiframes, idle frames between, tag 7 (it came back as a
 * BCCH/CCCH block). Timeslot 0 and the BCC as the training sequence, as the
 * stock builds them (FUN_000504f8). The tag-7 multiframes cycle the frame
 * offset by 0, -1 and +1 (the SCH correction may be a frame out); the last
 * BCCH_MF8 use tag 8 at offset 0. Blocks are counted per variant.
 * System Information 3 and 4 carry the location area (MCC, MNC, LAC), SI 3
 * the cell identity too (TS 44.018). */
#define BCCH_MF         9
#define BCCH_MF8        3
#define BCCH_BUDGET_MS  4000u

static int bcch_tag(int v)
{
    return v == 3 ? 8 : 7;
}

static uint32_t bcch_delta(int v)
{
    return v == 1 ? FN_MOD - 1 : v == 2 ? 1 : 0;
}

static struct {
    uint32_t fn0;                       /* our count at burst 0 */
    uint8_t variant;                    /* tag 7 at 0 / -1 / +1, 3 tag 8 at 0 */
} bcch_plan[BCCH_MF];
static struct {
    int8_t variant;                     /* -1: matched no posted block */
    l1_rx_t rx;
} si_kept[12];
static uint32_t si_kept_n;

static void lai_decode(const uint8_t *p)
{
    uint32_t mnc3 = p[1] >> 4;

    radio_status.mcc = (p[0] & 0xf) * 100u + (p[0] >> 4) * 10u + (p[1] & 0xf);
    radio_status.mnc = (p[2] & 0xf) * 10u + (p[2] >> 4);
    radio_status.mnc_digits = 2;
    if (mnc3 != 0xf) {
        radio_status.mnc = radio_status.mnc * 10u + mnc3;
        radio_status.mnc_digits = 3;
    }
    radio_status.lac = (uint32_t)p[3] << 8 | p[4];
}

/* d[0] the L2 pseudo length, d[1] 0x06 (RR), d[2] the message type */
static void si_decode(const uint8_t *d)
{
    if (d[1] != 0x06 || d[2] < 0x18 || d[2] > 0x1f)
        return;
    radio_status.si_seen |= 1u << (d[2] - 0x18);
    if (d[2] == 0x1b) {                 /* SI 3: cell identity, then the LAI */
        radio_status.cell_id = (uint32_t)d[3] << 8 | d[4];
        lai_decode(d + 5);
    } else if (d[2] == 0x1c && !radio_status.mcc) {     /* SI 4: the LAI first */
        lai_decode(d + 3);
    }
}

static void take_bcch(void)
{
    l1_rx_t rx;

    while (l1_rx_read(&rx) == 1) {
        uint32_t fn = rx.hdr[2] | (uint32_t)(rx.hdr[3] & 0xff) << 16;
        int v = -1;

        radio_status.bcch_rx++;
        if (rx.hdr[0] < 32)
            radio_status.bcch_rx_types |= 1u << rx.hdr[0];
        for (int i = 0; i < BCCH_MF; i++)
            if (bcch_plan[i].fn0 != ~0u && (fn + FN_MOD - bcch_plan[i].fn0) % FN_MOD <= 4)
                v = bcch_plan[i].variant;
        if (si_kept_n < 12) {
            si_kept[si_kept_n].variant = (int8_t)v;
            si_kept[si_kept_n++].rx = rx;
        }
        if ((rx.hdr[0] != 7 && rx.hdr[0] != 8) || v < 0)
            continue;
        if (rx.hdr[1] == 0) {
            radio_status.bcch_good[v]++;
            si_decode(rx.data);
        } else {
            radio_status.bcch_bad[v]++;
        }
    }
}

static void bcch(void)
{
    int k = -1;
    uint32_t off, t0 = kernel_ticks(), msgs = dsp_probe_info.msgs, c0 = consumed(), last;
    int mf = 0, burst = -1;
    uint16_t index;
    int bcc;

    si_kept_n = 0;
    for (int i = 0; i < BCCH_MF; i++)
        bcch_plan[i].fn0 = ~0u;
    last = c0;
    for (int i = 0; i < SYNC_CELLS && k < 0; i++)
        if (radio_status.sync_sch[i])
            k = i;
    if (k < 0)
        return;
    radio_status.bcch_cell = (uint32_t)k;
    index = (uint16_t)radio_status.sync_index[k];
    /* The other cells were searched after this one, and the DSP may keep the
     * slot timing and frequency of the last search: sync this cell again. The
     * first sync's result stays if this one finds no SCH. */
    {
        uint32_t sch = radio_status.sync_sch[k], fn = radio_status.sync_fn[k];
        uint32_t local = radio_status.sync_local[k], back = radio_status.sync_back[k];
        uint32_t exact = radio_status.sync_exact[k];

        radio_status.sync_sch[k] = 0;
        radio_status.sync_exact[k] = 0;
        sync_cell(k, index);
        radio_status.bcch_resync = radio_status.sync_sch[k];
        if (!radio_status.sync_sch[k]) {
            radio_status.sync_fn[k] = fn;
            radio_status.sync_local[k] = local;
            radio_status.sync_back[k] = back;
            radio_status.sync_exact[k] = exact;
        }
        radio_status.sync_sch[k] += sch;
    }
    t0 = kernel_ticks();
    c0 = last = consumed();
    bcc = (int)(radio_status.sync_bsic[k] & 7);
    off = (radio_status.sync_fn[k] + FN_MOD - radio_status.sync_local[k] % FN_MOD) % FN_MOD;

    while (kernel_ticks() - t0 < BCCH_BUDGET_MS) {
        bool batch = false;

        while (queued() < QUEUE_AHEAD && (mf < BCCH_MF || burst >= 0)) {
            uint32_t local = l1_next_fn();
            int nv = mf >= BCCH_MF - BCCH_MF8 ? 3 : mf % 3;
            uint32_t m = (local + off + bcch_delta(nv)) % FN_MOD % 51;
            int rc;

            if (burst < 0 && m == 2 && mf < BCCH_MF) {
                burst = 0;
                bcch_plan[mf].fn0 = local;
                bcch_plan[mf].variant = (uint8_t)nv;
            }
            if (burst >= 0) {
                int v = bcch_plan[mf].variant;

                rc = l1_burst_post(index, bcch_tag(v), burst, 0, bcc, k);
                if (rc == DSP_OK && ++burst == 4) {
                    burst = -1;
                    mf++;
                    radio_status.bcch_blocks++;
                }
            } else {
                rc = l1_idle_post(1);
            }
            if (rc != DSP_OK)
                break;
            posted++;
            batch = true;
        }
        if (batch)
            l1_ask_more(3);
        if (mf >= BCCH_MF && burst < 0)
            feed_idle();
        l1_poll();
        take_bcch();
        if (consumed() != last) {
            last = consumed();
            radio_status.bcch_last_move_ms = kernel_ticks() - t0;
        }
        if (mf >= BCCH_MF && burst < 0 && queued() <= QUEUE_AHEAD &&
            (l1_next_fn() + FN_MOD - bcch_plan[BCCH_MF - 1].fn0) % FN_MOD > 30)
            break;
        thread_sleep(2);
    }
    take_bcch();
    radio_status.bcch_consumed = consumed() - c0;
    radio_status.bcch_msgs = dsp_probe_info.msgs - msgs;
    radio_status.bcch_ms = kernel_ticks() - t0;
}

/* ---- 7. report ----------------------------------------------------------- */

static void save_report(void)
{
    FIL *f = kmalloc(sizeof(FIL));
    char *t = kmalloc(512);
    UINT w;

    if (!f || !t || f_open(f, OS_DIR "/radio-report.txt", FA_CREATE_ALWAYS | FA_WRITE) != FR_OK)
        goto out;
#define LINE(...) do { int n_ = snprintf(t, 512, __VA_ARGS__); \
        f_write(f, t, (UINT)(n_ < 512 ? n_ : 511), &w); } while (0)
    LINE("# B310E OS radio status (receive only)\n");
    for (int s = 0; s < SIM_SLOTS; s++)
        LINE("sim%d %s, %lu mV, imsi read sw %ld\n", s + 1,
             (const char *[]){ "none", "ready", "pin", "error" }[radio_status.sim[s]],
             (unsigned long)radio_status.sim_mv[s], (long)radio_status.sim_sw[s]);
    LINE("dsp_boot %ld (%s), blocks %lu, %lu ms\n", (long)radio_status.dsp_result,
         radio_status.dsp_result <= 0 ? dsp_strerror(radio_status.dsp_result) : "not run",
         (unsigned long)dsp_info.blocks, (unsigned long)dsp_info.boot_ms);
    LINE("l1_start %ld, records %lu, irqs %lu, replies %lu, messages %lu\n",
         (long)radio_status.l1_result, (unsigned long)radio_status.records,
         (unsigned long)dsp_info.irqs, (unsigned long)dsp_probe_info.replies,
         (unsigned long)dsp_probe_info.msgs);
    LINE("frames %lu in 400 ms at %lu us (expect ~86 at 4615)\n", (unsigned long)radio_status.frames_taken,
         (unsigned long)radio_status.frame_us);
    LINE("scan sweeps %lu/%u, frames %lu, results %lu: valid %lu, status2 %lu, status3 %lu, "
         "status3 at the low gain (rxlev 0) %lu, other %lu, stray %lu; heard %lu channels%s\n", (unsigned long)radio_status.sweeps,
         SWEEPS, (unsigned long)radio_status.pm_frames, (unsigned long)radio_status.pm_results,
         (unsigned long)radio_status.pm_valid, (unsigned long)radio_status.pm_sat_hi,
         (unsigned long)radio_status.pm_sat_lo, (unsigned long)radio_status.pm_floor,
         (unsigned long)radio_status.pm_other,
         (unsigned long)radio_status.pm_stray, (unsigned long)radio_status.channels_heard,
         radio_status.stalled ? ", STALLED" : "");
    LINE("elapsed %lu ms\n", (unsigned long)radio_status.elapsed_ms);
    LINE("sync: %lu cells tried, %lu blocks, types seen %08lx, DSP messages %lu\n",
         (unsigned long)radio_status.sync_tried, (unsigned long)radio_status.sync_rx,
         (unsigned long)radio_status.sync_rx_types, (unsigned long)radio_status.sync_msgs);
    for (int k = 0; k < SYNC_CELLS; k++) {
        int arfcn;
        const char *band;

        if (radio_status.sync_index[k] == L1_NO_INDEX)
            continue;
        band = band_of(radio_status.sync_index[k], &arfcn);
        if (radio_status.sync_sch[k])
            LINE("sync cell %d: %s %d, %lu good SCH, BSIC %lu (NCC %lu BCC %lu), fn %lu "
                 "at our %lu (%s, %lu back)\n", k,
                 band, arfcn, (unsigned long)radio_status.sync_sch[k],
                 (unsigned long)radio_status.sync_bsic[k],
                 (unsigned long)radio_status.sync_bsic[k] >> 3,
                 (unsigned long)radio_status.sync_bsic[k] & 7,
                 (unsigned long)radio_status.sync_fn[k],
                 (unsigned long)radio_status.sync_local[k],
                 radio_status.sync_exact[k] ? "exact" : "corrected",
                 (unsigned long)radio_status.sync_back[k]);
        else
            LINE("sync cell %d: %s %d, no good SCH\n", k, band, arfcn);
    }
    LINE("sync: last DSP message %08lx\n", (unsigned long)radio_status.sync_msg_last);
    if (radio_status.bcch_cell != 0xff) {
        LINE("bcch: BEP reports %lu in all, the last %04lx %04lx\n",
             (unsigned long)dsp_probe_info.beps, (unsigned long)dsp_probe_info.bep_last[0],
             (unsigned long)dsp_probe_info.bep_last[1]);
        LINE("bcch: %lu ms, frames taken %lu, the last at %lu ms; DSP messages %lu, the last 8:",
             (unsigned long)radio_status.bcch_ms, (unsigned long)radio_status.bcch_consumed,
             (unsigned long)radio_status.bcch_last_move_ms,
             (unsigned long)radio_status.bcch_msgs);
        for (uint32_t i = 0; i < 8; i++)
            LINE(" %08lx", (unsigned long)dsp_probe_info.msg_log[(dsp_probe_info.msgs + i) % 8]);
        LINE("\n");
        LINE("bcch: cell %lu, %lu blocks posted, %lu received (types %08lx); good/bad by "
             "variant: tag 7 at 0 %lu/%lu, -1 %lu/%lu, +1 %lu/%lu; tag 8 %lu/%lu; SI seen %02lx; "
             "resync %lu good SCH, fn %lu at our %lu (%s)\n",
             (unsigned long)radio_status.bcch_cell, (unsigned long)radio_status.bcch_blocks,
             (unsigned long)radio_status.bcch_rx, (unsigned long)radio_status.bcch_rx_types,
             (unsigned long)radio_status.bcch_good[0], (unsigned long)radio_status.bcch_bad[0],
             (unsigned long)radio_status.bcch_good[1], (unsigned long)radio_status.bcch_bad[1],
             (unsigned long)radio_status.bcch_good[2], (unsigned long)radio_status.bcch_bad[2],
             (unsigned long)radio_status.bcch_good[3], (unsigned long)radio_status.bcch_bad[3],
             (unsigned long)radio_status.si_seen, (unsigned long)radio_status.bcch_resync,
             (unsigned long)radio_status.sync_fn[radio_status.bcch_cell],
             (unsigned long)radio_status.sync_local[radio_status.bcch_cell],
             radio_status.sync_exact[radio_status.bcch_cell] ? "exact" : "corrected");
        if (radio_status.mcc)
            LINE("network: MCC %03lu MNC %0*lu, LAC %lu, cell id %lu\n",
                 (unsigned long)radio_status.mcc, (int)radio_status.mnc_digits,
                 (unsigned long)radio_status.mnc, (unsigned long)radio_status.lac,
                 (unsigned long)radio_status.cell_id);
        LINE("# bcch blocks: variant; hdr[0..8]; data[0..22]\n");
        for (uint32_t b = 0; b < si_kept_n; b++) {
            const l1_rx_t *x = &si_kept[b].rx;
            char d[23 * 3 + 1];

            for (int i = 0; i < 23; i++)
                snprintf(d + i * 3, 4, "%02x ", x->data[i]);
            LINE("%d; %04x %04x %04x %04x %04x %04x %04x %04x %04x; %s\n", si_kept[b].variant,
                 x->hdr[0], x->hdr[1], x->hdr[2], x->hdr[3], x->hdr[4], x->hdr[5],
                 x->hdr[6], x->hdr[7], x->hdr[8], d);
        }
    }
    LINE("# raw blocks: cell; hdr[0..8]; data[0..23]\n");
    for (uint32_t b = 0; b < kept_n; b++) {
        const l1_rx_t *x = &kept[b].rx;
        char d[24 * 3 + 1];

        for (int i = 0; i < 24; i++)
            snprintf(d + i * 3, 4, "%02x ", x->data[i]);
        LINE("%u; %04x %04x %04x %04x %04x %04x %04x %04x %04x; %s\n", kept[b].cell,
             x->hdr[0], x->hdr[1], x->hdr[2], x->hdr[3], x->hdr[4], x->hdr[5], x->hdr[6],
             x->hdr[7], x->hdr[8], d);
    }
    LINE("# index,band,arfcn,best_raw(1/16),rxlev,valid,saturated,floor,gain_hint\n");
    for (uint32_t i = 0; chan && i < CHANNELS; i++) {
        int arfcn;
        const char *band;

        if (!chan[i].valid && !chan[i].sat)
            continue;
        band = band_of(i, &arfcn);
        LINE("%lu,%s,%d,%u,%u,%u,%u,%u,%u\n", (unsigned long)i, band, arfcn, chan[i].best,
             (chan[i].best + 8u) >> 4, chan[i].valid, chan[i].sat, chan[i].floor,
             chan[i].gain);
    }
#undef LINE
    f_close(f);
    radio_status.saved = 1;
out:
    kfree(t);
    kfree(f);
}

/* ---- the run ------------------------------------------------------------- */

static void worker(void *arg)
{
    uint32_t t0 = kernel_ticks();

    radio_status.phase = R_SIM;
    for (int s = 0; s < SIM_SLOTS; s++)
        sim_check(s);

    radio_status.phase = R_DSP;
    radio_status.dsp_result = dsp_boot();
    if (radio_status.dsp_result == DSP_OK) {
        radio_status.phase = R_L1;
        radio_status.l1_result = l1_start();
        radio_status.records = dsp_info.records;
        if (radio_status.l1_result == DSP_OK) {
            radio_status.phase = R_CLOCK;
            radio_status.l1_result = clock_check();
            chan = kmalloc(CHANNELS * sizeof(chan_t));
            if (chan)
                for (uint32_t i = 0; i < CHANNELS; i++)
                    chan[i] = (chan_t){ .gain = GAIN_START };
            if (chan && radio_status.frames_taken >= MIN_FRAMES) {
                radio_status.phase = R_SCAN;
                scan();
                if (!radio_status.stalled) {
                    radio_status.phase = R_SYNC;
                    sync();
                    radio_status.phase = R_BCCH;
                    bcch();
                }
            }
        }
    }
    dsp_stop();
    radio_status.elapsed_ms = kernel_ticks() - t0;

    radio_status.phase = R_SAVE;
    save_report();
    kfree(chan);
    chan = NULL;
    klog("radio: sim %lu/%lu, dsp %ld, l1 %ld, frames %lu at %lu us, sweeps %lu, "
         "results %lu (valid %lu), heard %lu channels%s",
         (unsigned long)radio_status.sim[0], (unsigned long)radio_status.sim[1],
         (long)radio_status.dsp_result, (long)radio_status.l1_result,
         (unsigned long)radio_status.frames_taken, (unsigned long)radio_status.frame_us,
         (unsigned long)radio_status.sweeps, (unsigned long)radio_status.pm_results,
         (unsigned long)radio_status.pm_valid, (unsigned long)radio_status.channels_heard,
         radio_status.stalled ? ", stalled" : "");
    radio_status.phase = R_DONE;
    radio_status.runs++;
    radio_status.busy = 0;
}

static void start(void)
{
    if (radio_status.busy)
        return;
    /* the DSP boot screen may have it */
    if (dsp_info.state == DSP_BOOTING || dsp_info.state == DSP_RUNNING)
        return;
    memset((void *)&radio_status.phase, 0,
           sizeof(radio_status_t) - offsetof(radio_status_t, phase));
    radio_status.dsp_result = radio_status.l1_result = 1;
    for (int k = 0; k < TOP; k++)
        radio_status.top_index[k] = L1_NO_INDEX;
    for (int k = 0; k < SYNC_CELLS; k++) {
        radio_status.sync_index[k] = L1_NO_INDEX;
        radio_status.sync_bsic[k] = 0xff;
    }
    radio_status.bcch_cell = 0xff;
    radio_status.busy = 1;
    if (!thread_create("radio", PRIO_NORMAL, 3072, worker, NULL))
        radio_status.busy = 0;
}

/* ---- screen -------------------------------------------------------------- */

static void row(int y, const char *label, const char *value, uint16_t c)
{
    ui_kv(UI_PAD + 6, y, LCD_WIDTH - 2 * UI_PAD - 12, label, value, c);
}

static const char *verdict(uint16_t *c)
{
    static const char *const phases[] = {
        "-", "reading SIM...", "booting DSP...", "layer 1...", "frame clock...",
        "scanning...", "syncing...", "reading BCCH...", "saving...",
    };
    static char synced[24];

    *c = C_TEXT;
    if (!radio_status.busy && (dsp_info.state == DSP_BOOTING || dsp_info.state == DSP_RUNNING))
        return "DSP in use: Run later";
    if (radio_status.busy)
        return phases[radio_status.phase < R_DONE ? radio_status.phase : 0];
    if (!radio_status.runs)
        return "-";
    if (radio_status.dsp_result != DSP_OK || radio_status.l1_result != DSP_OK) {
        *c = C_BAD;
        return "DSP not answering";
    }
    if (radio_status.frames_taken < MIN_FRAMES) {
        *c = C_BAD;
        return "no frame clock";
    }
    if (radio_status.mcc && !radio_status.busy) {
        snprintf(synced, sizeof(synced), "%03lu-%0*lu cell %lu", (unsigned long)radio_status.mcc,
                 (int)radio_status.mnc_digits, (unsigned long)radio_status.mnc,
                 (unsigned long)radio_status.cell_id);
        *c = C_OK;
        return synced;
    }
    for (int k = 0; k < SYNC_CELLS; k++)
        if (radio_status.sync_sch[k]) {
            int arfcn;

            band_of(radio_status.sync_index[k], &arfcn);
            snprintf(synced, sizeof(synced), "cell %d: BSIC %lu", arfcn,
                     (unsigned long)radio_status.sync_bsic[k]);
            *c = C_OK;
            return synced;
        }
    if (radio_status.channels_heard) {
        *c = radio_status.stalled ? C_WARN : C_OK;
        return radio_status.stalled ? "scan stalled" : "radio receiving";
    }
    *c = C_WARN;
    return "nothing heard";
}

static void draw(screen_t *s)
{
    char buf[32];
    int y = UI_TOP + 3, w = LCD_WIDTH - 2 * UI_PAD;
    uint16_t c;
    const char *v = verdict(&c);
    bool done = radio_status.runs && !radio_status.busy;

    gfx_round(UI_PAD, y, w, 124, UI_RADIUS, C_CARD);
    gfx_text_centered(y + 5, v, c, FONT_SMALL, 1);
    for (int i = 0; i < SIM_SLOTS; i++) {
        static const char *const st[] = { "no card", "ready", "PIN", "error" };
        uint32_t k = radio_status.sim[i];
        bool known = done || radio_status.phase > R_SIM;

        snprintf(buf, sizeof(buf), "SIM %d", i + 1);
        row(y + 18 + i * 11, buf, known ? st[k] : "-",
            !known ? C_DIM : k == SIM_READY ? C_OK : k == SIM_NONE ? C_DIM : C_WARN);
    }
    if (radio_status.dsp_result == 1)
        snprintf(buf, sizeof(buf), "-");
    else if (radio_status.dsp_result != DSP_OK)
        snprintf(buf, sizeof(buf), "%s", dsp_strerror(radio_status.dsp_result));
    else
        snprintf(buf, sizeof(buf), "%lu records", (unsigned long)radio_status.records);
    row(y + 40, "DSP", buf, radio_status.l1_result == DSP_OK ? C_OK : C_TEXT);
    snprintf(buf, sizeof(buf), "%lu, %lu us", (unsigned long)radio_status.frames_taken,
             (unsigned long)radio_status.frame_us);
    row(y + 51, "Frames", buf, C_TEXT);
    snprintf(buf, sizeof(buf), "%lu/%lu, %lu ch", (unsigned long)radio_status.pm_valid,
             (unsigned long)radio_status.pm_results, (unsigned long)radio_status.channels_heard);
    row(y + 62, "Scan", buf, C_TEXT);
    for (int k = 0; k < 4; k++) {
        uint32_t i = radio_status.top_index[k];
        int arfcn;
        const char *band;

        if (i == L1_NO_INDEX)
            break;
        band = band_of(i, &arfcn);
        snprintf(buf, sizeof(buf), "%s %d", band, arfcn);
        {
            char lv[12];

            snprintf(lv, sizeof(lv), "%lu", (unsigned long)((radio_status.top_level[k] + 8) >> 4));
            row(y + 75 + k * 11, buf, lv, C_TEXT);
        }
    }
    gfx_text_centered(UI_BOTTOM - 12, radio_status.saved ? "report: radio-report.txt" :
                      "receive only", C_DIM, FONT_SMALL, 1);
}

static void enter(screen_t *s)
{
    start();
}

static void tick(screen_t *s)
{
}

static void key(screen_t *s, const key_event_t *ev)
{
    if (ev->code == KEY_CENTER || ev->code == KEY_LSOFT)
        start();
    else if (ev->code == KEY_RSOFT && !radio_status.busy)
        ui_pop();
}

static screen_t radio_screen = {
    .title = "Radio status", .center = "Run", .right = "Back",
    .enter = enter, .draw = draw, .key = key, .tick = tick, .tick_ms = 200,
};

screen_t *app_radio_status(void)
{
    return &radio_screen;
}
