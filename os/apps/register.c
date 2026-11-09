/* B310E OS — Tools / Register (experimental, developer-mode only): a GSM location
 * update (registration) attempt on the owner's SIM, the end-to-end exercise of the
 * registration stack (L1 RACH + CCCH + SDCCH, LAPDm, RR, MM, the FSM, SIM auth).
 *
 * SAFETY. This TRANSMITS at NORMAL (calibrated) power on the live network with the
 * owner's SIM. It targets the SIM's home operator (from the IMSI) on the 900 band
 * (P-GSM or E-GSM), sweeping many channels so a weak home BCCH is still found, and
 * shows the operator on screen; nothing fires without pressing Register and
 * confirming. One bounded attempt per press; it shows the uplink frequency to
 * monitor and restores the TX-off default after.
 * The receive path (SIM read, DSP boot, scan, sync, read SI) is all safe; only the
 * final reg_lu_run() keys the transmitter.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/dsp.h"
#include "os/fs.h"                       /* klog */
#include "os/gsm_lu.h"
#include "os/gsm_reg.h"
#include "os/kernel.h"
#include "os/lib.h"
#include "os/sim.h"
#include "os/ui.h"

screen_t *app_register(void);
void reg_trigger(void);
int reg_is_busy(void);

/* The 900 band in the corrected index map (os/dsp.h, stock 0x5287): P-GSM at
 * indexes 0-124 and E-GSM at 499-547. 1800/1900/850 are left out - their RF path
 * is not selected yet (see docs; the register works on 900, same PA as P-GSM). */
#define IDX_MAX         548             /* best[] spans indexes 0..547 */
static const struct { uint16_t lo, hi; } band900[] = { { 0, 124 }, { 499, 547 } };
/* The stock's power sweep (FUN_0005c3e2): every channel starts at setting 25; a
 * "too weak" result (status 3) is measured again at 10, a saturated one (2) at
 * 40, and valid results are averaged. Status 3 at 10 means nothing is there. The
 * first version took one look at 25 and dropped both kinds, so a weak home cell
 * was in some scans and missing from others. */
#define GAIN_LOW        10u
#define GAIN_START      25u
#define GAIN_HIGH       40u
#define GAIN_DEAD       0xffu           /* status 3 at 10: not measured again */
#define SWEEPS          4u
/* Two home cells here differ a lot: one decodes 100% of its CCCH, the other loses
 * 15-50% (2026-11-09) - and a lost IMMEDIATE ASSIGNMENT is a failed attempt. So a
 * home cell's CCCH is probed (receive only) and the best of up to MAX_HOMES is
 * used; one at GOOD_CCCH_PCT or better is taken at once. */
#define CCCH_PROBE_MS   1500u
#define GOOD_CCCH_PCT   90
#define MAX_HOMES       3
#define HOME_CELL_FILE  OS_DIR "/gsm-home-cell.txt"     /* "mcc mnc index": the last home
                                         * cell found, tried first next time (the stock
                                         * keeps its last cell the same way) */
/* A home BCCH can be a weak carrier buried under stronger traffic (e.g. a
 * roaming-partner / less-dense operator), so sweep many channels in signal order,
 * not just the top few. l1_cell_sync fails a non-BCCH fast (one FCCH window), so
 * this stays ~30 s bounded by FIND_BUDGET_MS. */
#define CANDIDATES      130             /* channels to sweep, strongest first */
#define FIND_BUDGET_MS  60000u          /* cap on the whole find phase */
#define FI_RING_FREE    25              /* l1_fi_free() with the ring empty */
#define QUEUE_AHEAD     8
#define SYNC_BUDGET_MS  1500u
#define READ_BUDGET_MS  2500u
#define TX_BUDGET_MS    10000u
/* Classmark 1 (TS 24.008 10.5.1.6): phase 2, A5/1 NOT available (we do no
 * ciphering), RF power class 4 (GSM900 handheld). Honest for a real run. */
#define CLASSMARK1      0x2b
#define SIM_SLOT        0

enum { P_IDLE, P_SIM, P_DSP, P_SCAN, P_FIND, P_TX, P_DONE };
enum { RES_NONE, RES_NO_SIM, RES_RADIO_FAIL, RES_NO_CELL, RES_ACCEPTED,
       RES_REJECTED, RES_NO_RESPONSE };

static volatile struct {
    uint8_t phase, result, busy, runs;
    uint8_t reject_cause;
    uint16_t mcc, mnc, lac;
    uint8_t mnc_digits;
    uint16_t arfcn_index;               /* the cell tried, L1_NO_INDEX none */
    uint16_t home_mcc, home_mnc;        /* the SIM's PLMN, for the screen */
} reg_ui;

static uint8_t imsi_digits[16];
static int imsi_n;
static uint16_t home_mcc, home_mnc;     /* the SIM's own PLMN, from the IMSI */
static gsm_reg_t reg;
static l1_cell_t cell;
static l1_cell_t best_cell;             /* the best home cell so far (find phase) */
static reg_lu_progress_t reg_prog;      /* how far the last/current attempt got */
static int sweep_synced;                /* cells that synced in the find sweep (diag) */
static int sweep_pruned;                /* candidates skipped as another operator's */
static uint16_t stored_index = L1_NO_INDEX;     /* from HOME_CELL_FILE, if its PLMN fits */
static uint8_t stored_gain;             /* the gain it synced at, 0 unknown */
static uint8_t foreign[(IDX_MAX + 7) / 8];      /* channels another operator's cell lists */

/* ---- frame-ring feeding (the proven gsmstatus pattern) ------------------- */

static uint32_t posted;

static uint32_t queued(void)
{
    return (uint32_t)(FI_RING_FREE - l1_fi_free());
}

static void feed_idle(void)
{
    int n = QUEUE_AHEAD - (int)queued();

    if (n > 0 && l1_idle_post(n) == DSP_OK) {
        posted += (uint32_t)n;
        l1_ask_more(3);
    }
}

/* ---- 1. SIM: read EF_IMSI, decode the packed BCD into digits -------------- */

/* EF_IMSI (TS 51.011 10.3.2): byte 0 length, then packed BCD. The low nibble of
 * byte 1 is the parity/type indicator; its high nibble is the first digit, then
 * low-then-high nibbles of the rest; 0xf is filler. Returns the digit count. */
static int decode_imsi(const uint8_t *ef, int len, uint8_t out[16])
{
    int n = 0, used = ef[0];

    if (used < 1 || used > len - 1)
        used = len - 1;
    out[n++] = (uint8_t)(ef[1] >> 4);
    for (int i = 2; i <= used && n < 15; i++) {
        uint8_t lo = ef[i] & 0x0f, hi = (uint8_t)(ef[i] >> 4);

        if (lo <= 9)
            out[n++] = lo;
        if (hi <= 9 && n < 15)
            out[n++] = hi;
    }
    return n;
}

/* Activate slot 0 and read its IMSI into imsi_digits. The slot stays active (the
 * auth step needs it). SIM_OK, or an error (and the slot is powered down). */
static int read_imsi(void)
{
    static const uint8_t sel[5] = { 0xa0, 0xa4, 0x00, 0x00, 0x02 };
    static const uint8_t mf[2] = { 0x3f, 0x00 }, df_gsm[2] = { 0x7f, 0x20 },
        ef_imsi[2] = { 0x6f, 0x07 };
    static const uint8_t read9[5] = { 0xa0, 0xb0, 0x00, 0x00, 0x09 };
    uint8_t ef[9];
    sim_atr_t atr;
    int sw;

    if (sim_activate(SIM_SLOT, &atr) != SIM_OK)
        return -1;
    sw = sim_apdu(SIM_SLOT, sel, mf, NULL, NULL);
    if (sw > 0)
        sw = sim_apdu(SIM_SLOT, sel, df_gsm, NULL, NULL);
    if (sw > 0)
        sw = sim_apdu(SIM_SLOT, sel, ef_imsi, NULL, NULL);
    if (sw > 0)
        sw = sim_apdu(SIM_SLOT, read9, NULL, ef, NULL);
    if (sw != 0x9000) {
        sim_deactivate(SIM_SLOT);
        return -1;
    }
    imsi_n = decode_imsi(ef, (int)sizeof ef, imsi_digits);
    memset(ef, 0, sizeof ef);           /* the IMSI is not kept in cleartext scratch */
    return imsi_n >= 6 ? 0 : -1;
}

/* ---- 2. scan GSM900 for the strongest BCCH candidates -------------------- */

typedef struct {
    uint16_t sum;                       /* valid levels, 1/16 rxlev */
    uint8_t n;                          /* how many */
    uint8_t gain;                       /* the setting its next measurement uses */
} pm_chan_t;

/* The stock's rule per result (FUN_0005c3e2 / FUN_0005c8ba, docs/gsm-plan.md). */
static void take_pm(pm_chan_t *ch, const l1_pm_t *res, int got)
{
    for (int j = 0; j < got; j++) {
        pm_chan_t *c;

        if (res[j].index >= IDX_MAX)
            continue;
        c = &ch[res[j].index];
        if (res[j].status == 1) {
            c->sum = (uint16_t)(c->sum + (res[j].level & 0xfffu));
            c->n++;
        } else if (res[j].status == 2) {        /* too strong */
            if (c->gain == GAIN_HIGH) {
                c->sum = (uint16_t)(c->sum + (63u << 4));
                c->n++;
            }
            c->gain = GAIN_HIGH;
        } else if (res[j].status == 3) {        /* too weak */
            c->gain = c->gain == GAIN_LOW ? GAIN_DEAD : GAIN_LOW;
        }
    }
}

static void drain_pm(pm_chan_t *ch, int rounds)
{
    l1_pm_t res[8];
    int got;

    for (int i = 0; i < rounds; i++) {
        l1_poll();
        while ((got = l1_pm_read(res, 8)) > 0)
            take_pm(ch, res, got);
        feed_idle();
        thread_sleep(2);
    }
}

/* The 900 band measured SWEEPS times, each channel at its own setting, then the
 * strongest `max` by average level into cand/candrx (rxlev 0..110, the DSP's
 * scale). The number of candidates. */
static int scan_candidates(uint16_t *cand, uint8_t *candrx, int max)
{
    pm_chan_t *ch = kmalloc(IDX_MAX * sizeof(pm_chan_t));
    int n = 0;

    if (!ch)
        return 0;
    for (uint32_t i = 0; i < IDX_MAX; i++)
        ch[i] = (pm_chan_t){ .gain = GAIN_START };
    for (uint32_t sweep = 0; sweep < SWEEPS; sweep++) {
        for (uint32_t b = 0; b < sizeof band900 / sizeof band900[0]; b++) {
            uint32_t next = band900[b].lo;

            while (next <= band900[b].hi) {
                uint16_t idx[L1_PM_PER_FRAME];
                uint8_t gain[L1_PM_PER_FRAME];
                int k = 0;

                for (; k < L1_PM_PER_FRAME && next <= band900[b].hi; next++)
                    if (ch[next].gain != GAIN_DEAD) {
                        idx[k] = (uint16_t)next;
                        gain[k] = ch[next].gain;
                        k++;
                    }
                if (!k)
                    break;
                for (int w = 0; queued() >= QUEUE_AHEAD && w < 200; w++) {
                    l1_poll();
                    thread_sleep(1);    /* bounded: never hang if the DSP stalls */
                }
                if (l1_pm_post(idx, gain, k) == DSP_OK) {
                    posted++;
                    l1_ask_more(3);
                }
                drain_pm(ch, 1);
            }
        }
        drain_pm(ch, 25);               /* this sweep's gains moved before the next */
    }
    /* the top `max` by average level across both sub-bands. O(max*IDX_MAX), the
     * ring fed between picks: an O(max^2) version stalled long enough to break
     * the DSP frame clock before the sweep, so nothing synced. */
    for (int k = 0; k < max; k++) {
        uint16_t bi = L1_NO_INDEX, bv = 0;

        for (uint32_t i = 0; i < IDX_MAX; i++)
            if (ch[i].n) {
                uint16_t v = (uint16_t)((ch[i].sum / ch[i].n + 8u) >> 4);

                if (v > bv || bi == L1_NO_INDEX)
                    bv = v, bi = (uint16_t)i;
            }
        if (bi == L1_NO_INDEX)
            break;
        candrx[n] = (uint8_t)bv;
        cand[n++] = bi;
        ch[bi].n = 0;
        if (!(k & 15))
            feed_idle();
    }
    kfree(ch);
    return n;
}

/* ---- the last home cell, as the stock keeps its own (read before the DSP
 * starts and written after it stops: a card write must not starve the ring) -- */

static int parse_uint(const char **p)
{
    int v = -1;

    while (**p == ' ' || **p == '\t')
        (*p)++;
    while (**p >= '0' && **p <= '9') {
        v = (v < 0 ? 0 : v * 10) + (**p - '0');
        if (v > 100000)
            return -1;
        (*p)++;
    }
    return v;
}

static void load_home_cell(void)
{
    FIL *f = kmalloc(sizeof(FIL));
    char buf[32];
    UINT got = 0;

    stored_index = L1_NO_INDEX;
    stored_gain = 0;
    if (!f)
        return;
    if (f_open(f, HOME_CELL_FILE, FA_READ) == FR_OK) {
        if (f_read(f, buf, sizeof buf - 1, &got) == FR_OK) {
            const char *p = buf;
            int mcc, mnc, idx, gain;

            buf[got] = 0;
            mcc = parse_uint(&p);
            mnc = parse_uint(&p);
            idx = parse_uint(&p);
            gain = parse_uint(&p);      /* -1 in an older 3-field file */
            if (mcc == home_mcc && mnc == home_mnc && idx >= 0 && idx < IDX_MAX) {
                stored_index = (uint16_t)idx;
                stored_gain = (uint8_t)(gain > 0 && gain <= 45 ? gain : 0);
            }
        }
        f_close(f);
    }
    kfree(f);
}

static void save_home_cell(uint16_t index, uint8_t gain)
{
    FIL *f = kmalloc(sizeof(FIL));
    char buf[32];
    UINT w;
    int n;

    if (!f)
        return;
    n = snprintf(buf, sizeof buf, "%u %u %u %u\n", home_mcc, home_mnc, index, gain);
    if (f_open(f, HOME_CELL_FILE, FA_CREATE_ALWAYS | FA_WRITE) == FR_OK) {
        f_write(f, buf, (UINT)n, &w);
        f_close(f);
    }
    kfree(f);
}

/* A foreign operator's cell: its own channels (SI 1) and neighbours (SI 2) are
 * that operator's too, so they are not tried. */
static void mark_foreign(const l1_arfcn_list_t *l)
{
    if (!l->decoded)
        return;
    for (int i = 0; i < l->count && i < L1_CELL_ARFCNS; i++) {
        uint16_t idx = l1_arfcn_index(L1_BAND_GSM900, l->arfcn[i]);

        if (idx < IDX_MAX)
            foreign[idx / 8] |= (uint8_t)(1u << (idx % 8));
    }
}

static bool is_foreign(uint16_t idx)
{
    return idx < IDX_MAX && (foreign[idx / 8] & (1u << (idx % 8)));
}

/* ---- 3. sync a candidate and read its SI; is it the SIM's home network? --- */

/* Sync at the stock's gain for the measured level and read SI 2 + 3: SI 3 names
 * the operator, and a foreign cell's SI 1 / SI 2 lists rule out its siblings. */
static bool try_cell(uint16_t index, int rxlev)
{
    uint8_t gain = l1_gain_for_rxlev(rxlev);
    bool home;

    if (l1_cell_sync(&cell, index, gain, 0, SYNC_BUDGET_MS) != 1)
        return false;                   /* not a BCCH (fast fail) */
    sweep_synced++;
    l1_cell_read(&cell, 24, L1_SI2 | L1_SI3, READ_BUDGET_MS);
    home = cell.mcc == home_mcc && cell.mnc == home_mnc && (cell.si_seen & L1_SI3);
    klog("register: idx %u rx%d g%u -> %u-%02u si %08lx good %lu bad %lu", index, rxlev,
         gain, cell.mcc, cell.mnc, (unsigned long)cell.si_seen,
         (unsigned long)cell.good, (unsigned long)cell.bad);
    if (!home && cell.mcc && (cell.si_seen & (L1_SI3 | L1_SI4))) {
        mark_foreign(&cell.channels);
        mark_foreign(&cell.neighbours);
    }
    return home;
}

/* Receive only: the share of a synced cell's CCCH blocks that decode, percent. */
static int cch_quality(l1_cell_t *c)
{
    l1_cch_t st;
    uint32_t total;

    memset(&st, 0, sizeof st);
    if (l1_cch_read(c, CCCH_PROBE_MS, &st) != 0)
        return 0;
    total = st.blocks + st.bad;
    return total ? (int)(st.blocks * 100u / total) : 0;
}

/* ---- the run ------------------------------------------------------------- */

static void worker(void *arg)
{
    uint16_t cand[CANDIDATES];
    uint8_t candrx[CANDIDATES];          /* rxlev per candidate, to pick the sync gain */
    uint16_t found_index = L1_NO_INDEX;  /* the home cell, saved once the DSP is off */
    uint8_t found_gain = 0;
    int ncand, rc;

    reg_ui.phase = P_SIM;
    if (read_imsi() != 0 || imsi_n < 5) {
        reg_ui.result = RES_NO_SIM;
        goto done_nosim;
    }
    /* home PLMN from the IMSI: MCC = digits 0-2, MNC = digits 3-4 (2-digit, as in
     * Turkey/MCC 286); only the operator code is kept, never the full IMSI. */
    home_mcc = (uint16_t)(imsi_digits[0] * 100 + imsi_digits[1] * 10 + imsi_digits[2]);
    home_mnc = (uint16_t)(imsi_digits[3] * 10 + imsi_digits[4]);
    reg_ui.home_mcc = home_mcc;
    reg_ui.home_mnc = home_mnc;
    load_home_cell();                    /* before the DSP: no card I/O while it runs */
    klog("register: home PLMN %u-%02u, stored cell %u", home_mcc, home_mnc, stored_index);

    reg_ui.phase = P_DSP;
    if (dsp_boot() != DSP_OK) {
        reg_ui.result = RES_RADIO_FAIL;
        goto done;
    }
    /* the calibration tables as they are: the DSP picks the setpoint for the
     * level set before the RACH (l1_tx_level_set). L1_TX_MAX, every setpoint
     * overwritten with the top one, sent nothing usable (SDR, 2026-11-10). */
    l1_tx_power_set(L1_TX_NORMAL);
    if (l1_start() != DSP_OK || l1_run() != DSP_OK) {
        reg_ui.result = RES_RADIO_FAIL;
        goto done;
    }
    posted = 4;                          /* l1_run()'s idle frames */

    reg_ui.phase = P_SCAN;
    /* fast path for iterating the SDCCH: with a known home cell, skip the full
     * 900-band sweep and go straight to it (the find still verifies its CCCH). */
    if (stored_index != L1_NO_INDEX) {
        ncand = 1;
        cand[0] = stored_index;
        candrx[0] = (uint8_t)(stored_gain ? 2 * stored_gain : 0);
    } else {
        ncand = scan_candidates(cand, candrx, CANDIDATES);
    }
    klog("register: scan ncand=%d, top %u/%u %u/%u %u/%u", ncand,
         ncand > 0 ? cand[0] : 0xffffu, ncand > 0 ? candrx[0] : 0,
         ncand > 1 ? cand[1] : 0xffffu, ncand > 1 ? candrx[1] : 0,
         ncand > 2 ? cand[2] : 0xffffu, ncand > 2 ? candrx[2] : 0);
    for (int i = 0, shown = 0; i < ncand && shown < 4; i++)  /* E-GSM: where TT lives */
        if (cand[i] >= 499)
            klog("register: scan E-GSM idx %u rx%u rank %d", cand[i], candrx[i], i), shown++;

    reg_ui.phase = P_FIND;
    reg_ui.result = RES_NO_CELL;
    sweep_synced = sweep_pruned = 0;
    memset(foreign, 0, sizeof foreign);
    /* the stored home cell first (the stock's stored-list selection), at its
     * level from this scan, or at the gain it last synced at if unmeasured */
    if (stored_index != L1_NO_INDEX) {
        int at = -1;

        for (int i = 0; i < ncand; i++)
            if (cand[i] == stored_index)
                at = i;
        if (at < 0) {                   /* not measured this time: the last slot, at
                                         * the gain that worked (rxlev = 2 x gain) */
            at = ncand < CANDIDATES ? ncand++ : CANDIDATES - 1;
            cand[at] = stored_index;
            candrx[at] = (uint8_t)(stored_gain ? 2 * stored_gain : 0);
        }
        if (at > 0) {
            uint8_t rx = candrx[at];

            for (int i = at; i > 0; i--)
                cand[i] = cand[i - 1], candrx[i] = candrx[i - 1];
            cand[0] = stored_index;
            candrx[0] = rx;
        }
    }
    {
        uint32_t find_t0 = kernel_ticks();
        int swept = 0, homes = 0, best_q = -1;

    for (int i = 0; i < ncand && kernel_ticks() - find_t0 < FIND_BUDGET_MS; i++) {
        int q;

        if (is_foreign(cand[i])) {
            sweep_pruned++;
            continue;
        }
        swept++;
        reg_ui.arfcn_index = cand[i];
        if (!try_cell(cand[i], candrx[i]))
            continue;
        q = cch_quality(&cell);
        klog("register: home idx %u CCCH %d%% decoded", cand[i], q);
        if (q > best_q) {
            best_q = q;
            best_cell = cell;
        }
        if (q >= GOOD_CCCH_PCT || ++homes >= MAX_HOMES)
            break;
    }
    klog("register: find swept=%d synced=%d pruned=%d in %lums", swept, sweep_synced,
         sweep_pruned, (unsigned long)(kernel_ticks() - find_t0));
    if (best_q >= 0) {
        /* the best one synced again if another was synced after it: the DSP keeps
         * one timing per cell slot, and the last sync took slot 0 */
        if (best_cell.index != cell.index) {
            if (l1_cell_sync(&cell, best_cell.index, best_cell.gain, 0, SYNC_BUDGET_MS) == 1) {
                best_cell.bsic = cell.bsic;
                best_cell.fn_offset = cell.fn_offset;
                cell = best_cell;
            } else {
                best_q = -1;            /* it did not sync again */
            }
        }
    }
    if (best_q >= 0) {
        reg_ui.arfcn_index = cell.index;
        reg_ui.mcc = cell.mcc;
        reg_ui.mnc = cell.mnc;
        reg_ui.mnc_digits = cell.mnc_digits;
        reg_ui.lac = cell.lac;
        reg_ui.result = RES_NONE;
        found_index = cell.index;
        found_gain = cell.gain;
        klog("register: using idx %u (CCCH %d%%)", cell.index, best_q);
    }
    }
    if (reg_ui.result == RES_NO_CELL)
        goto done;

    /* camp first, as the stock does before any access: command 3 tracks the cell,
     * so its bursts steer the AFC and the uplink lands on the cell's frequency
     * (untracked, the transmitter sits at the crystal's, which a BTS's RACH
     * detector does not forgive). The tracked CCCH read lets the AFC settle. */
    {
        int trc = l1_cell_track(cell.slot, (uint8_t)(2 * cell.gain));

        klog("register: camped (command 3: %d), CCCH %d%% decoded tracked", trc,
             cch_quality(&cell));
    }
    /* a synced home cell (P-GSM or E-GSM): build the LU request and run it */
    reg_init(&reg, imsi_digits, imsi_n, cell.mcc, cell.mnc, cell.mnc_digits == 3,
             cell.lac, CLASSMARK1);
    reg_ui.phase = P_TX;
    /* the RACH's power level, the stock's way (share +0x3ec, flagged in word 5):
     * MS_TXPWR_MAX_CCH from SI 3, not above this phone's class (PCL 5 = 33 dBm on
     * 900). Without it the DSP had no level and sent nothing usable. */
    {
        uint8_t pcl = cell.ms_txpwr_max_cch < 5 ? 5 : cell.ms_txpwr_max_cch;

        if (pcl > 19)
            pcl = 19;
        l1_tx_level_set(pcl);
        klog("register: TX level PCL %u (cell's MS_TXPWR_MAX_CCH %u)", pcl,
             cell.ms_txpwr_max_cch);
    }
    klog("register: RACH to idx %u, BSIC %u (NCC %u BCC %u)", cell.index, cell.bsic,
         cell.bsic >> 3, cell.bsic & 7);
    rc = reg_lu_run(&reg, &cell, 0, SIM_SLOT, (uint8_t)(kernel_ticks() & 0x0f),
                    TX_BUDGET_MS, &reg_prog);
    klog("register: prog rach=%u cch=%u imm=%u rahit=%u refd=%d assign=%u est=%u auth=%u"
         " sim=%u dl=%u/%u ul=%u sabm=%u", reg_prog.rach_sent, reg_prog.cch_blocks,
         reg_prog.imm_seen, reg_prog.ra_hits, reg_prog.ref_d, reg_prog.got_assign,
         reg_prog.established, reg_prog.reached_auth, reg_prog.sim_ok,
         reg_prog.dl_blocks, reg_prog.dl_bad, reg_prog.ul_bursts, reg_prog.sabm_sent);
    klog("register: chan type=%u subch=%u tn=%u tsc=%u, last dl hdr %02x %02x",
         reg_prog.ch_type, reg_prog.ch_subch, reg_prog.ch_tn, reg_prog.ch_tsc,
         reg_prog.dl_addr, reg_prog.dl_ctrl);
    reg_ui.reject_cause = reg.reject_cause;
    reg_ui.result = rc == REG_ACCEPTED ? RES_ACCEPTED :
                    rc == REG_REJECTED ? RES_REJECTED : RES_NO_RESPONSE;

done:
    dsp_stop();
    l1_tx_power_set(L1_TX_OFF);          /* restore the safe default (TX refused) */
    if (found_index != L1_NO_INDEX)
        save_home_cell(found_index, found_gain);
done_nosim:
    sim_deactivate(SIM_SLOT);
    memset(imsi_digits, 0, sizeof imsi_digits);     /* drop the IMSI */
    imsi_n = 0;
    klog("register: result %u (phase %u), cell %u-%0*u lac %u",
         reg_ui.result, reg_ui.phase, reg_ui.mcc, (int)reg_ui.mnc_digits,
         reg_ui.mnc, reg_ui.lac);
    reg_ui.phase = P_DONE;
    reg_ui.runs++;
    reg_ui.busy = 0;
}

static void start_worker(void);
/* Trigger one registration attempt from outside the UI (the net shell "reg"),
 * for driving the attempt over the link while iterating. Same worker, no dialog. */
void reg_trigger(void) { start_worker(); }
int reg_is_busy(void) { return reg_ui.busy; }

static void start_worker(void)
{
    if (reg_ui.busy)
        return;
    if (dsp_info.state == DSP_BOOTING || dsp_info.state == DSP_RUNNING)
        return;
    reg_ui.phase = P_IDLE;
    reg_ui.result = RES_NONE;
    reg_ui.mcc = reg_ui.mnc = reg_ui.lac = 0;
    reg_ui.home_mcc = reg_ui.home_mnc = 0;
    reg_ui.reject_cause = 0;
    reg_ui.arfcn_index = L1_NO_INDEX;
    memset(&reg_prog, 0, sizeof reg_prog);
    reg_ui.busy = 1;
    if (!thread_create("register", PRIO_NORMAL, 4096, worker, NULL))
        reg_ui.busy = 0;
}

/* ---- screen -------------------------------------------------------------- */

static const char *phase_text(void)
{
    switch (reg_ui.phase) {
    case P_SIM:  return "Reading SIM";
    case P_DSP:  return "Starting radio";
    case P_SCAN: return "Scanning 900";
    case P_FIND: return "Finding home cell";
    case P_TX:   return "Attempt";
    default:     return "Working";
    }
}

/* 900 arfcn index -> ARFCN and uplink kHz (integer; no float printf). Corrected
 * map: idx 0..124 is P-GSM (ARFCN 0..124), idx 499..547 is E-GSM (ARFCN
 * 975..1023); uplink = 890 MHz + 0.2 MHz * n, n = ARFCN-1024 in the extended
 * band. Returns the ARFCN, or -1. */
static int arfcn_from_index(uint16_t idx, int *ul_khz)
{
    int arfcn;

    if (idx <= 124)
        arfcn = (int)idx;
    else if (idx >= 499 && idx <= 547)
        arfcn = 975 + (int)idx - 499;
    else
        return *ul_khz = 0, -1;
    *ul_khz = 890000 + 200 * (arfcn >= 975 ? arfcn - 1024 : arfcn);
    return arfcn;
}

/* Turkish operators (MCC 286) by name; others fall back to the numeric code. */
static const char *operator_name(uint16_t mcc, uint16_t mnc)
{
    if (mcc == 286) {
        if (mnc == 1) return "Turkcell";
        if (mnc == 2) return "Vodafone";
        if (mnc == 3) return "Turk Telekom";
    }
    return 0;
}

/* The non-rejected outcomes (REJECTED carries a cause number, drawn separately). */
static void result_line(const char **msg, uint32_t *color)
{
    switch (reg_ui.result) {
    case RES_ACCEPTED:    *msg = "Registered"; *color = C_OK; return;
    case RES_NO_RESPONSE: *msg = "Not registered"; *color = C_WARN; return;
    case RES_NO_CELL:     *msg = "No home cell found"; *color = C_WARN; return;
    case RES_NO_SIM:      *msg = "SIM not ready"; *color = C_BAD; return;
    case RES_RADIO_FAIL:  *msg = "Radio did not start"; *color = C_BAD; return;
    default:              *msg = "Done"; *color = C_TEXT; return;
    }
}

static void draw(screen_t *s)
{
    int y = UI_TOP + 3;
    char line[48];

    gfx_round(UI_PAD, y, LCD_WIDTH - 2 * UI_PAD, 22, UI_RADIUS, C_CARD);
    if (reg_ui.busy) {
        const char *busy = dsp_info.state == DSP_BOOTING ? "Waiting for the DSP..." :
                           phase_text();

        gfx_text_centered(y + 7, busy, C_ACCENT, FONT_SMALL, 1);
    } else if (reg_ui.runs) {
        const char *msg;
        uint32_t color = C_TEXT;
        char rej[40];

        if (reg_ui.result == RES_REJECTED) {
            snprintf(rej, sizeof rej, "Rejected (cause %u)", reg_ui.reject_cause);
            msg = rej; color = C_BAD;
        } else {
            result_line(&msg, &color);
        }
        gfx_text_centered(y + 7, msg, color, FONT_SMALL, 1);
    } else {
        gfx_text_centered(y + 7, "Press Register", C_ACCENT, FONT_SMALL, 1);
    }
    y += 28;
    if ((reg_ui.runs || reg_ui.busy) && reg_ui.home_mcc) {
        const char *hop = operator_name(reg_ui.home_mcc, reg_ui.home_mnc);

        if (hop)
            snprintf(line, sizeof line, "SIM: %s  %u-%02u", hop, reg_ui.home_mcc,
                     reg_ui.home_mnc);
        else
            snprintf(line, sizeof line, "SIM: %u-%02u", reg_ui.home_mcc, reg_ui.home_mnc);
        gfx_text_centered(y, line, C_DIM, FONT_SMALL, 1);
        y += 13;
    }
    if ((reg_ui.runs || reg_ui.busy) && reg_ui.mcc) {
        {
            const char *op = operator_name(reg_ui.mcc, reg_ui.mnc);

            if (op)
                snprintf(line, sizeof line, "%s  %u-%0*u", op, reg_ui.mcc,
                         (int)reg_ui.mnc_digits, reg_ui.mnc);
            else
                snprintf(line, sizeof line, "MCC %u  MNC %0*u", reg_ui.mcc,
                         (int)reg_ui.mnc_digits, reg_ui.mnc);
        }
        gfx_text_centered(y, line, C_TEXT, FONT_SMALL, 1);
        y += 12;
        snprintf(line, sizeof line, "LAC %u", reg_ui.lac);
        gfx_text_centered(y, line, C_DIM, FONT_SMALL, 1);
        if (reg_ui.arfcn_index != L1_NO_INDEX) {
            int ul, arfcn = arfcn_from_index(reg_ui.arfcn_index, &ul);

            if (arfcn >= 0) {
                y += 12;
                snprintf(line, sizeof line, "ARFCN %d  UL %d.%d MHz", arfcn,
                         ul / 1000, (ul % 1000) / 100);
                gfx_text_centered(y, line, C_ACCENT, FONT_SMALL, 1);
            }
        }
        if (reg_ui.phase == P_TX || reg_ui.result >= RES_ACCEPTED) {
            const char *stage = reg_prog.sim_ok ? "auth ok" :
                                reg_prog.reached_auth ? "auth req" :
                                reg_prog.established ? "SDCCH up" :
                                reg_prog.got_assign ? "got grant" : "RACH sent";

            y += 12;
            snprintf(line, sizeof line, "%s  dl%u ul%u", stage,
                     reg_prog.dl_blocks, reg_prog.ul_bursts);
            gfx_text_centered(y, line, C_TEXT, FONT_SMALL, 1);
        }
    }
    gfx_text_centered(UI_BOTTOM - 12, "experimental, transmits", C_DIM, FONT_SMALL, 1);
}

static void confirm_yes(void)
{
    start_worker();
}

static void key(screen_t *s, const key_event_t *ev)
{
    if (reg_ui.busy)
        return;
    if (ev->code == KEY_CENTER || ev->code == KEY_LSOFT)
        ui_confirm("Register",
                   "Transmit a registration to this SIM's home "
                   "network on the live band?", confirm_yes);
    else if (ev->code == KEY_RSOFT)
        ui_pop();
}

static screen_t screen = {
    .title = "Register", .center = "Register", .right = "Back",
    .draw = draw, .key = key, .tick_ms = 200,
};

screen_t *app_register(void)
{
    reg_ui.runs = 0;
    reg_ui.busy = 0;
    reg_ui.phase = P_IDLE;
    reg_ui.result = RES_NONE;
    reg_ui.arfcn_index = L1_NO_INDEX;
    return &screen;
}
