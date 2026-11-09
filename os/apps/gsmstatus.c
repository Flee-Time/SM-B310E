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
#define SYNC_BUDGET_MS  1500u
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
    uint32_t sync_fn[SYNC_CELLS];       /* cell FN = our frame number + this */
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
    uint32_t si_seen;                   /* L1_SI*, os/l1cell.h */
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

/* The strongest 900 channels, each with one FCCH search (drivers/l1cell.c). */
static l1_cell_t cells[SYNC_CELLS];
static int selected_n;

static void sync(void)
{
    uint32_t msgs = dsp_probe_info.msgs;

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
        if (bi == L1_NO_INDEX)
            continue;
        radio_status.sync_tried++;
        if (l1_cell_sync(&cells[k], (uint16_t)bi, chan[bi].gain, k, SYNC_BUDGET_MS) == 1) {
            radio_status.sync_sch[k] = 1;
            radio_status.sync_bsic[k] = cells[k].bsic;
            radio_status.sync_fn[k] = cells[k].fn_offset;
        }
    }
    radio_status.sync_msgs = dsp_probe_info.msgs - msgs;
    radio_status.sync_msg_last = dsp_probe_info.msg_last;
}

/* ---- 6. broadcast channel ------------------------------------------------- */

/* Selection among the channels that gave an SCH (drivers/l1cell.c). */
static int chosen = -1;

static void bcch(void)
{
    uint32_t t0 = kernel_ticks(), msgs = dsp_probe_info.msgs;
    uint16_t index[SYNC_CELLS];
    uint8_t from[SYNC_CELLS];
    int n = 0;

    chosen = -1;
    for (int i = 0; i < SYNC_CELLS; i++)
        if (radio_status.sync_sch[i]) {
            from[n] = (uint8_t)i;
            index[n++] = (uint16_t)radio_status.sync_index[i];
        }
    selected_n = n;
    if (!n)
        return;
    chosen = l1_cell_select(index, n, 0, 0, cells);
    /* None suitable: still the first that read, for its parameters. l1_rxlev_measure
     * reads 0 on the phone for now, so no cell comes out suitable (docs/gsm-plan.md). */
    for (int i = 0; i < n && chosen < 0; i++)
        if (cells[i].si_seen)
            chosen = i;
    radio_status.bcch_cell = from[chosen >= 0 ? chosen : 0];
    if (chosen >= 0) {
        const l1_cell_t *c = &cells[chosen];

        radio_status.bcch_resync = 1;
        radio_status.bcch_good[0] = c->good;
        radio_status.bcch_bad[0] = c->bad;
        radio_status.si_seen = c->si_seen;
        radio_status.mcc = c->mcc;
        radio_status.mnc = c->mnc;
        radio_status.mnc_digits = c->mnc_digits;
        radio_status.lac = c->lac;
        radio_status.cell_id = c->ci;
    }
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
    LINE("sync: %lu cells tried, DSP messages %lu, the last %08lx\n",
         (unsigned long)radio_status.sync_tried, (unsigned long)radio_status.sync_msgs,
         (unsigned long)radio_status.sync_msg_last);
    for (int k = 0; k < SYNC_CELLS; k++) {
        int arfcn;
        const char *band;

        if (radio_status.sync_index[k] == L1_NO_INDEX)
            continue;
        band = band_of(radio_status.sync_index[k], &arfcn);
        if (radio_status.sync_sch[k])
            LINE("sync cell %d: %s %d, BSIC %lu (NCC %lu BCC %lu), frame offset %lu\n", k,
                 band, arfcn, (unsigned long)radio_status.sync_bsic[k],
                 (unsigned long)radio_status.sync_bsic[k] >> 3,
                 (unsigned long)radio_status.sync_bsic[k] & 7,
                 (unsigned long)radio_status.sync_fn[k]);
        else
            LINE("sync cell %d: %s %d, no SCH\n", k, band, arfcn);
    }
    for (int i = 0; i < selected_n; i++)
        LINE("select: index %u, rxlev %d, BSIC %u, C1 %d, barred %u, %s%s\n", cells[i].index,
             cells[i].rxlev, cells[i].bsic, cells[i].c1, cells[i].barred,
             cells[i].suitable ? "suitable" : "not suitable", i == chosen ? ", shown below" : "");
    if (chosen >= 0) {
        const l1_cell_t *c = &cells[chosen];

        LINE("bcch: %lu good, %lu bad, %lu not System Information, "
             "SI seen %08lx, %lu ms, DSP messages %lu; BEP last %04lx %04lx\n",
             (unsigned long)c->good, (unsigned long)c->bad, (unsigned long)c->other,
             (unsigned long)c->si_seen, (unsigned long)radio_status.bcch_ms,
             (unsigned long)radio_status.bcch_msgs, (unsigned long)dsp_probe_info.bep_last[0],
             (unsigned long)dsp_probe_info.bep_last[1]);
        if (radio_status.mcc)
            LINE("network: MCC %03lu MNC %0*lu, LAC %lu, cell id %lu\n",
                 (unsigned long)radio_status.mcc, (int)radio_status.mnc_digits,
                 (unsigned long)radio_status.mnc, (unsigned long)radio_status.lac,
                 (unsigned long)radio_status.cell_id);
        if (c->si_seen & L1_SI3)
            LINE("cell: CCCH conf %u, AG blocks %u, paging multiframes %u, T3212 %u, attach %u\n",
                 c->ccch_conf, c->bs_ag_blks_res, c->bs_pa_mfrms, c->t3212, c->att);
        if (c->si_seen & (L1_SI3 | L1_SI4))
            LINE("selection: rxlev access min %u, TX power max %u, hysteresis %u, barred %u\n",
                 c->rxlev_access_min, c->ms_txpwr_max_cch, c->hysteresis, c->barred);
        if (c->si_seen & L1_SI2) {
            LINE("neighbours (900, BA %u, NCC permitted %02x):", c->ba_ind, c->ncc_permitted);
            if (!c->neighbours.decoded)
                LINE(" %s, not decoded", l1_list_format(c->neighbours.format));
            for (int i = 0; i < c->neighbours.count && i < L1_CELL_ARFCNS; i++)
                LINE(" %u", c->neighbours.arfcn[i]);
            LINE("\n");
        }
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

/* The Turkish networks (MCC 286); other networks show the numeric code. */
static const char *operator_name(uint16_t mcc, uint16_t mnc)
{
    if (mcc == 286) {
        if (mnc == 1)
            return "Turkcell";
        if (mnc == 2)
            return "Vodafone";
        if (mnc == 3)
            return "Turk Telekom";
    }
    return NULL;
}

static const char *phase_text(void)
{
    switch (radio_status.phase) {
    case R_SIM:
        return "Reading SIM";
    case R_DSP:
    case R_L1:
        return "Starting DSP";
    case R_CLOCK:
        return "Frame clock";
    case R_SCAN:
        return "Scanning channels";
    case R_SYNC:
        return "Finding cells";
    case R_BCCH:
        return "Reading cells";
    case R_SAVE:
        return "Saving report";
    default:
        return "Working";
    }
}

/* One cell: operator name, ARFCN and BSIC, an RSSI bar and the level in dBm. The
 * serving (chosen) cell is drawn lit and in amber. Receive only: these are the
 * cells the phone found and read, not a sweep of the band. */
static void cell_card(int y, const l1_cell_t *c, bool serving)
{
    const char *name = operator_name(c->mcc, c->mnc);
    int arfcn, full = LCD_WIDTH - 2 * UI_PAD - 12, barw, rxlev = c->rxlev;
    const char *band = band_of(c->index, &arfcn);
    char buf[40];

    gfx_round(UI_PAD, y, LCD_WIDTH - 2 * UI_PAD, 26, UI_RADIUS, serving ? C_CARD_HI : C_CARD);
    if (name)
        snprintf(buf, sizeof(buf), "%s", name);
    else if (c->mcc)
        snprintf(buf, sizeof(buf), "%u-%0*u", c->mcc, (int)c->mnc_digits, c->mnc);
    else
        snprintf(buf, sizeof(buf), "cell");
    gfx_text(UI_PAD + 6, y + 3, buf, serving ? C_ACCENT : C_TEXT, FONT_SMALL, 1, 0);
    if (c->barred)
        gfx_text(UI_PAD + 6 + gfx_text_width(buf, FONT_SMALL, 1) + 4, y + 3, "barred",
                 C_BAD, FONT_SMALL, 1, 0);
    if (rxlev >= 0) {
        snprintf(buf, sizeof(buf), "%d dBm", rxlev - 110);
        gfx_text(LCD_WIDTH - UI_PAD - 6 - gfx_text_width(buf, FONT_SMALL, 1), y + 3, buf,
                 C_DIM, FONT_SMALL, 1, 0);
    }
    snprintf(buf, sizeof(buf), "%s %d  BSIC %u", band, arfcn, c->bsic);
    gfx_text(UI_PAD + 6, y + 13, buf, C_DIM, FONT_SMALL, 1, 0);
    if (rxlev < 0)
        rxlev = 0;
    barw = rxlev > 63 ? full : rxlev * full / 63;
    gfx_fill(UI_PAD + 6, y + 21, full, 3, C_CARD_HI);
    if (barw > 0)
        gfx_fill(UI_PAD + 6, y + 21, barw, 3,
                 rxlev >= 40 ? C_OK : rxlev >= 25 ? C_WARN : C_BAD);
}

static void draw(screen_t *s)
{
    int y = UI_TOP + 3;
    bool done = radio_status.runs && !radio_status.busy;

    if (!done) {
        const char *busy = dsp_info.state == DSP_BOOTING || dsp_info.state == DSP_RUNNING ?
                           "DSP in use: Run later" :
                           radio_status.busy ? phase_text() : "Press Run to scan";

        gfx_round(UI_PAD, y, LCD_WIDTH - 2 * UI_PAD, 22, UI_RADIUS, C_CARD);
        gfx_text_centered(y + 7, busy, C_ACCENT, FONT_SMALL, 1);
        gfx_text_centered(UI_BOTTOM - 12, "receive only", C_DIM, FONT_SMALL, 1);
        return;
    }
    gfx_text_centered(y, selected_n ? "Cells heard" : "No cell found",
                      selected_n ? C_TEXT : C_WARN, FONT_BODY, 1);
    y += 15;
    for (int i = 0; i < selected_n && i < 4; i++) {
        cell_card(y, &cells[i], i == chosen);
        y += 28;
    }
    gfx_text_centered(UI_BOTTOM - 12,
                      radio_status.saved ? "receive only, report saved" : "receive only",
                      C_DIM, FONT_SMALL, 1);
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
