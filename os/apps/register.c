/* B310E OS — Tools / Register (experimental, developer-mode only): a GSM location
 * update (registration) attempt on the owner's SIM, the end-to-end exercise of the
 * registration stack (L1 RACH + CCCH + SDCCH, LAPDm, RR, MM, the FSM, SIM auth).
 *
 * SAFETY. This TRANSMITS, so nothing fires without the user pressing Register and
 * confirming. The min-power clamp is armed before l1_start(), so the RACH goes at
 * the minimum APC setpoint — a live tower will not decode it (a safe dry run), and
 * reg_lu_run() times out waiting for an IMMEDIATE ASSIGNMENT (result "no response").
 * A real, normal-power registration — which the owner has cleared for Vodafone WHEN
 * the full tool set exists — needs the clamp interlock released (a deliberate code
 * change) and the SIM-auth timing fix noted in os/gsm_lu.h; it is NOT this screen.
 *
 * Receive path (SIM read, DSP boot, scan, sync, read SI) is all safe; only the
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

#define G900            174u            /* GSM900 arfcn indexes 0..173 */
#define GAIN_START      25u             /* the stock's middle AGC (gsmstatus) */
#define SWEEPS          2u
#define CANDIDATES      4               /* strongest 900 channels to try */
#define FI_RING_FREE    25              /* l1_fi_free() with the ring empty */
#define QUEUE_AHEAD     8
#define SYNC_BUDGET_MS  1500u
#define READ_BUDGET_MS  2500u
#define TX_BUDGET_MS    10000u
#define VODAFONE_MCC    286
#define VODAFONE_MNC    2
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
} reg_ui;

static uint8_t imsi_digits[16];
static int imsi_n;
static gsm_reg_t reg;
static l1_cell_t cell;

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

static int scan_candidates(uint16_t *cand, int max)
{
    uint16_t *best = kmalloc(G900 * sizeof(uint16_t));
    l1_pm_t res[8];
    int n = 0;

    if (!best)
        return 0;
    for (uint32_t i = 0; i < G900; i++)
        best[i] = 0;
    for (uint32_t sweep = 0; sweep < SWEEPS; sweep++) {
        for (uint32_t base = 0; base < G900; base += L1_PM_PER_FRAME) {
            uint16_t idx[L1_PM_PER_FRAME];
            uint8_t gain[L1_PM_PER_FRAME];
            int k = 0, got;

            while (k < L1_PM_PER_FRAME && base + (uint32_t)k < G900) {
                idx[k] = (uint16_t)(base + (uint32_t)k);
                gain[k] = GAIN_START;
                k++;
            }
            for (int w = 0; queued() >= QUEUE_AHEAD && w < 200; w++) {
                l1_poll();
                thread_sleep(1);        /* bounded: never hang if the DSP stalls */
            }
            if (l1_pm_post(idx, gain, k) == DSP_OK) {
                posted++;
                l1_ask_more(3);
            }
            l1_poll();
            while ((got = l1_pm_read(res, 8)) > 0)
                for (int j = 0; j < got; j++)
                    if (res[j].status == 1 && res[j].index < G900 &&
                        (res[j].level & 0xfffu) > best[res[j].index])
                        best[res[j].index] = res[j].level & 0xfffu;
            feed_idle();
            thread_sleep(1);
        }
    }
    for (int i = 0; i < 12; i++) {       /* drain the last results */
        int got;

        l1_poll();
        while ((got = l1_pm_read(res, 8)) > 0)
            for (int j = 0; j < got; j++)
                if (res[j].status == 1 && res[j].index < G900 &&
                    (res[j].level & 0xfffu) > best[res[j].index])
                    best[res[j].index] = res[j].level & 0xfffu;
        feed_idle();
        thread_sleep(2);
    }
    for (int k = 0; k < max; k++) {      /* the top `max` by level */
        uint16_t bi = L1_NO_INDEX, bv = 0;

        for (uint32_t i = 0; i < G900; i++) {
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

/* ---- 3. sync a candidate and read its SI; is it Vodafone? ---------------- */

static bool try_cell(uint16_t index)
{
    if (l1_cell_sync(&cell, index, GAIN_START, 0, SYNC_BUDGET_MS) != 1)
        return false;
    l1_cell_read(&cell, 24, L1_SI3 | L1_SI4, READ_BUDGET_MS);
    return cell.mcc == VODAFONE_MCC && cell.mnc == VODAFONE_MNC &&
           (cell.si_seen & L1_SI3);
}

/* ---- the run ------------------------------------------------------------- */

static void worker(void *arg)
{
    uint16_t cand[CANDIDATES];
    int ncand, rc;

    reg_ui.phase = P_SIM;
    if (read_imsi() != 0) {
        reg_ui.result = RES_NO_SIM;
        goto done_nosim;
    }

    reg_ui.phase = P_DSP;
    if (dsp_boot() != DSP_OK) {
        reg_ui.result = RES_RADIO_FAIL;
        goto done;
    }
    l1_tx_min_power(1);                  /* clamp TX to the minimum before l1_start */
    if (l1_start() != DSP_OK || l1_run() != DSP_OK) {
        reg_ui.result = RES_RADIO_FAIL;
        goto done;
    }
    posted = 4;                          /* l1_run()'s idle frames */

    reg_ui.phase = P_SCAN;
    ncand = scan_candidates(cand, CANDIDATES);

    reg_ui.phase = P_FIND;
    reg_ui.result = RES_NO_CELL;
    for (int i = 0; i < ncand; i++) {
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
    if (reg_ui.result == RES_NO_CELL)
        goto done;

    /* a synced Vodafone cell: build the LU request and run the attempt (min power) */
    reg_init(&reg, imsi_digits, imsi_n, cell.mcc, cell.mnc, cell.mnc_digits == 3,
             cell.lac, CLASSMARK1);
    reg_ui.phase = P_TX;
    rc = reg_lu_run(&reg, &cell, 0, 0, SIM_SLOT, (uint8_t)(kernel_ticks() & 0x0f),
                    TX_BUDGET_MS);
    reg_ui.reject_cause = reg.reject_cause;
    reg_ui.result = rc == REG_ACCEPTED ? RES_ACCEPTED :
                    rc == REG_REJECTED ? RES_REJECTED : RES_NO_RESPONSE;

done:
    dsp_stop();
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
    reg_ui.reject_cause = 0;
    reg_ui.arfcn_index = L1_NO_INDEX;
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
    case P_FIND: return "Finding Vodafone";
    case P_TX:   return "Attempt (min power)";
    default:     return "Working";
    }
}

/* The non-rejected outcomes (REJECTED carries a cause number, drawn separately). */
static void result_line(const char **msg, uint32_t *color)
{
    switch (reg_ui.result) {
    case RES_ACCEPTED:    *msg = "Registered"; *color = C_OK; return;
    case RES_NO_RESPONSE: *msg = "No response (min power)"; *color = C_WARN; return;
    case RES_NO_CELL:     *msg = "No Vodafone cell"; *color = C_WARN; return;
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
    if ((reg_ui.runs || reg_ui.busy) && reg_ui.mcc) {
        snprintf(line, sizeof line, "Vodafone  MCC %u MNC %0*u", reg_ui.mcc,
                 (int)reg_ui.mnc_digits, reg_ui.mnc);
        gfx_text_centered(y, line, C_TEXT, FONT_SMALL, 1);
        y += 12;
        snprintf(line, sizeof line, "LAC %u", reg_ui.lac);
        gfx_text_centered(y, line, C_DIM, FONT_SMALL, 1);
    }
    gfx_text_centered(UI_BOTTOM - 12, "experimental, minimum power", C_DIM, FONT_SMALL, 1);
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
                   "Send a minimum-power registration attempt to Vodafone? "
                   "(experimental, transmits)", confirm_yes);
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
