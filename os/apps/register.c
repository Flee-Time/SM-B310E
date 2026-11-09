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

/* The 900 band in the corrected index map (os/dsp.h, stock 0x5287): P-GSM at
 * indexes 0-124 and E-GSM at 499-547. 1800/1900/850 are left out - their RF path
 * is not selected yet (see docs; the register works on 900, same PA as P-GSM). */
#define IDX_MAX         548             /* best[] spans indexes 0..547 */
static const struct { uint16_t lo, hi; } band900[] = { { 0, 124 }, { 499, 547 } };
#define GAIN_START      25u             /* the stock's middle AGC (gsmstatus) */
#define SWEEPS          2u
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
static reg_lu_progress_t reg_prog;      /* how far the last/current attempt got */

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

static void take_pm(uint16_t *best, l1_pm_t *res, int got)
{
    for (int j = 0; j < got; j++)
        if (res[j].status == 1 && res[j].index < IDX_MAX &&
            (res[j].level & 0xfffu) > best[res[j].index])
            best[res[j].index] = res[j].level & 0xfffu;
}

static int scan_candidates(uint16_t *cand, int max)
{
    uint16_t *best = kmalloc(IDX_MAX * sizeof(uint16_t));
    l1_pm_t res[8];
    int n = 0;

    if (!best)
        return 0;
    for (uint32_t i = 0; i < IDX_MAX; i++)
        best[i] = 0;
    for (uint32_t sweep = 0; sweep < SWEEPS; sweep++)
        for (uint32_t b = 0; b < sizeof band900 / sizeof band900[0]; b++)
            for (uint32_t base = band900[b].lo; base <= band900[b].hi; base += L1_PM_PER_FRAME) {
                uint16_t idx[L1_PM_PER_FRAME];
                uint8_t gain[L1_PM_PER_FRAME];
                int k = 0, got;

                while (k < L1_PM_PER_FRAME && base + (uint32_t)k <= band900[b].hi) {
                    idx[k] = (uint16_t)(base + (uint32_t)k);
                    gain[k] = GAIN_START;
                    k++;
                }
                for (int w = 0; queued() >= QUEUE_AHEAD && w < 200; w++) {
                    l1_poll();
                    thread_sleep(1);    /* bounded: never hang if the DSP stalls */
                }
                if (l1_pm_post(idx, gain, k) == DSP_OK) {
                    posted++;
                    l1_ask_more(3);
                }
                l1_poll();
                while ((got = l1_pm_read(res, 8)) > 0)
                    take_pm(best, res, got);
                feed_idle();
                thread_sleep(1);
            }
    for (int i = 0; i < 12; i++) {       /* drain the last results */
        int got;

        l1_poll();
        while ((got = l1_pm_read(res, 8)) > 0)
            take_pm(best, res, got);
        feed_idle();
        thread_sleep(2);
    }
    for (int k = 0; k < max; k++) {      /* the top `max` by level across both sub-bands */
        uint16_t bi = L1_NO_INDEX, bv = 0;

        for (uint32_t i = 0; i < IDX_MAX; i++) {
            bool taken = false;

            for (int j = 0; j < k; j++)
                taken |= cand[j] == i;
            if (!taken && best[i] > bv)
                bv = best[i], bi = (uint16_t)i;
        }
        if (bi == L1_NO_INDEX)
            break;
        cand[n++] = bi;
    }
    kfree(best);
    return n;
}

/* ---- 3. sync a candidate and read its SI; is it the SIM's home network? --- */

static bool try_cell(uint16_t index)
{
    int s = l1_cell_sync(&cell, index, GAIN_START, 0, SYNC_BUDGET_MS);

    if (s != 1)
        return false;                   /* not a BCCH (fast fail); not logged - noisy */
    l1_cell_read(&cell, 24, L1_SI3 | L1_SI4, READ_BUDGET_MS);
    klog("register: idx %u -> %u-%02u si %08lx", index, cell.mcc, cell.mnc,
         (unsigned long)cell.si_seen);
    return cell.mcc == home_mcc && cell.mnc == home_mnc && (cell.si_seen & L1_SI3);
}

/* ---- the run ------------------------------------------------------------- */

static void worker(void *arg)
{
    uint16_t cand[CANDIDATES];
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
    klog("register: home PLMN %u-%02u", home_mcc, home_mnc);

    reg_ui.phase = P_DSP;
    if (dsp_boot() != DSP_OK) {
        reg_ui.result = RES_RADIO_FAIL;
        goto done;
    }
    l1_tx_power_set(L1_TX_NORMAL);       /* normal (calibrated) power before l1_start */
    if (l1_start() != DSP_OK || l1_run() != DSP_OK) {
        reg_ui.result = RES_RADIO_FAIL;
        goto done;
    }
    posted = 4;                          /* l1_run()'s idle frames */

    reg_ui.phase = P_SCAN;
    ncand = scan_candidates(cand, CANDIDATES);

    reg_ui.phase = P_FIND;
    reg_ui.result = RES_NO_CELL;
    {
        uint32_t find_t0 = kernel_ticks();

    for (int i = 0; i < ncand && kernel_ticks() - find_t0 < FIND_BUDGET_MS; i++) {
        reg_ui.arfcn_index = cand[i];
        if (try_cell(cand[i])) {
            reg_ui.mcc = cell.mcc;
            reg_ui.mnc = cell.mnc;
            reg_ui.mnc_digits = cell.mnc_digits;
            reg_ui.lac = cell.lac;
            reg_ui.result = RES_NONE;
            break;
        }
    }
    }
    if (reg_ui.result == RES_NO_CELL)
        goto done;

    /* a synced home cell (P-GSM or E-GSM): build the LU request and run it */
    reg_init(&reg, imsi_digits, imsi_n, cell.mcc, cell.mnc, cell.mnc_digits == 3,
             cell.lac, CLASSMARK1);
    reg_ui.phase = P_TX;
    rc = reg_lu_run(&reg, &cell, 0, 0, SIM_SLOT, (uint8_t)(kernel_ticks() & 0x0f),
                    TX_BUDGET_MS, &reg_prog);
    klog("register: prog cch=%u imm=%u assign=%u est=%u auth=%u sim=%u dl=%u ul=%u",
         reg_prog.cch_blocks, reg_prog.imm_seen, reg_prog.got_assign,
         reg_prog.established, reg_prog.reached_auth, reg_prog.sim_ok,
         reg_prog.dl_blocks, reg_prog.ul_bursts);
    reg_ui.reject_cause = reg.reject_cause;
    reg_ui.result = rc == REG_ACCEPTED ? RES_ACCEPTED :
                    rc == REG_REJECTED ? RES_REJECTED : RES_NO_RESPONSE;

done:
    dsp_stop();
    l1_tx_power_set(L1_TX_OFF);          /* restore the safe default (TX refused) */
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
    case P_TX:   return "Attempt (normal power)";
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
    gfx_text_centered(UI_BOTTOM - 12, "experimental, normal power", C_DIM, FONT_SMALL, 1);
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
                   "Transmit a NORMAL-power registration to this SIM's home "
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
