/* B310E OS - the location-update L1 glue: drives the registration FSM (gsm_reg)
 * against the real layer 1 and the SIM. This is the ON-AIR integration: it keys
 * the transmitter on the operator's uplink, so it is exercised only through the
 * owner's cleared Vodafone registration, and the exact command-3 / scheduler
 * cadence is confirmed there. Everything it calls is unit-tested or SDR/SIM
 * verified in isolation; this file wires them together.
 *
 * Flow: RACH (CHANNEL REQUEST) -> read IMMEDIATE ASSIGNMENT off CCCH -> tune the
 * assigned SDCCH -> reg_begin (SABM+LU request) -> pump LAPDm blocks through the
 * SDCCH scheduler, feeding reg_rx and answering AUTHENTICATION via the SIM, until
 * ACCEPT or REJECT.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/gsm_lu.h"
#include "os/gsm_reg.h"
#include "os/dsp.h"                     /* l1_api, l1_cell_t, l1_rx_t */
#include "os/sim.h"
#include "os/kernel.h"
#include "os/fs.h"                     /* klog (SDCCH diagnostics) */

#define LU_RACH_ATTEMPTS 4              /* RACH retransmissions (a real MS retries) */
#define LU_RACH_WAIT_MS 2000u           /* wait for the grant after each RACH - long
                                         * enough for the RX to settle after the TX
                                         * burst and read the CCCH (a short wait gets 0) */
#define LU_BLOCK_MS     350u            /* ~ one SDCCH block period to pump */
#define LU_RETRACK_MS   200u            /* re-assert command 3 at least this often in
                                         * the dedicated phase: a single track locks
                                         * the AFC once, then the synth free-runs and
                                         * the SDCCH CRC drifts (dm#0 after a track
                                         * decoded 3/3, then degraded to 0/1). */
#define LU_T200_MS      300u            /* resend the SABM if no UA by this */
#define LU_SABM_MAX     60              /* resend the SABM at every chance */
#define LU_FN_MOD       2715648u        /* 26 x 51 x 2048 */
#define LU_REF_SLACK    4               /* frames either side of our RACH's: the grant's
                                         * request reference is the frame the BTS heard
                                         * it in, and our local->cell FN map is +-1 at best */

/* TS 44.018 10.5.2.30: T1' = FN div 1326 mod 32, T3 = FN mod 51, T2 = FN mod 26. */
static uint16_t ref_of_fn(uint32_t fn)
{
    return (uint16_t)((fn / 1326u % 32u) << 11 | (fn % 51u) << 5 | (fn % 26u));
}

/* How many frames the grant's request reference is from our RACH's frame, or 127
 * if not within LU_REF_SLACK. The RA byte alone is 8 bits: a busy cell grants ~10
 * a second, so another phone's grant with our RA comes every ~25 s. */
static int ref_delta(uint16_t ref, uint32_t rach_fn)
{
    for (int d = -LU_REF_SLACK; d <= LU_REF_SLACK; d++)
        if (ref_of_fn((rach_fn + LU_FN_MOD + (uint32_t)d) % LU_FN_MOD) == ref)
            return d;
    return 127;
}

/* CCCH FN mod 51 of the non-combined config (as l1cell.c cch_start). */
static int ccch_block(uint32_t m)
{
    return m == 6 || m == 12 || m == 16 || m == 22 || m == 26 ||
           m == 32 || m == 36 || m == 42 || m == 46;
}

/* Keep the frame ring LU_AHEAD deep, as every other L1 loop does ("keep about 8
 * instructions queued", docs/gsm-plan.md). The first version filled all of it,
 * so when a grant wait ran out the ring was full and the next RACH got DSP_EFULL:
 * every retransmission build sent one RACH, never a second. */
#define LU_FI_EMPTY     25              /* l1_fi_free() with the ring empty */
#define LU_AHEAD        8

/* Top the ring up with this cell's CCCH bursts (4 per block, each block from its
 * first burst) and idle frames. finishing: start no new block and stop once the
 * current one is all posted, so a RACH never lands inside a half-posted block.
 * DSP_OK or the post's error. */
static int cch_pump(l1_cell_t *c, int *burst, int finishing)
{
    while (l1_api.fi_free() > LU_FI_EMPTY - LU_AHEAD) {
        uint32_t m = (l1_api.next_fn() + c->fn_offset) % LU_FN_MOD % 51u;
        int rc;

        if (*burst < 0 && !finishing && ccch_block(m))
            *burst = 0;
        if (*burst >= 0) {
            rc = l1_api.burst_post(c->index, 7, *burst, 0, c->bsic & 7, c->slot);
            if (rc == DSP_OK && ++*burst == 4)
                *burst = -1;
        } else if (finishing) {
            return DSP_OK;
        } else {
            rc = l1_api.idle_post(1);
        }
        if (rc != DSP_OK)
            return rc;
    }
    return DSP_OK;
}

/* Received blocks: count the good CCCH ones and look for our grant. 1 when
 * IMMEDIATE ASSIGNMENT for our RA and frame came (*ch filled), else 0. */
static int take_cch(uint8_t ra, uint32_t rach_fn, rr_chan_desc_t *ch,
                    reg_lu_progress_t *prog)
{
    l1_rx_t rx;

    while (l1_api.rx_read(&rx) == 1) {
        if ((rx.hdr[0] != 7 && rx.hdr[0] != 8) || rx.hdr[1] != 0)
            continue;
        prog->cch_blocks++;
        if (rr_parse_imm_assign(rx.data, ch) == 0) {
            prog->imm_seen++;
            if (ch->ra == ra) {
                int d = ref_delta(ch->ref_fn, rach_fn);

                prog->ra_hits++;
                prog->ref_d = (int8_t)d;
                if (d != 127)
                    return 1;
            }
        }
    }
    return 0;
}

/* Read CCCH until an IMMEDIATE ASSIGNMENT whose request reference (RA and frame)
 * matches our RACH (sent in cell frame rach_fn) arrives, or the budget runs out.
 * Either way the block being posted is completed and the ring left LU_AHEAD
 * deep, so the next post has room. 1 + *ch filled, 0 none, or a DSP_E* error. */
static int wait_imm_assign(l1_cell_t *c, uint8_t ra, uint32_t rach_fn, rr_chan_desc_t *ch,
                           uint32_t ms, reg_lu_progress_t *prog)
{
    uint32_t t0 = kernel_ticks();
    int burst = -1, got = 0, rc;

    while (!got && kernel_ticks() - t0 < ms) {
        if ((rc = cch_pump(c, &burst, 0)) != DSP_OK)
            return rc;
        l1_api.ask_more(3);
        l1_api.poll();
        got = take_cch(ra, rach_fn, ch, prog);
        thread_sleep(2);
    }
    for (int w = 0; burst >= 0 && w < 200; w++) {       /* bounded: ~0.4 s */
        if ((rc = cch_pump(c, &burst, 1)) != DSP_OK)
            return rc;
        l1_api.ask_more(3);
        l1_api.poll();
        if (!got)
            got = take_cch(ra, rach_fn, ch, prog);
        thread_sleep(2);
    }
    return got;
}

/* SIM auth runs on its own thread: RUN GSM ALGORITHM blocks ~1s, and a
 * synchronous call inside the SDCCH pump would starve the frame-instruction ring
 * and break L1. The pump keeps feeding frames while this computes. One attempt
 * runs at a time, so a file-scope job is safe; reg_lu_run waits for the thread to
 * finish before returning (so the job is not reused and the SIM not freed under
 * it). */
static struct {
    volatile int done;                  /* 0 running, 1 ok, -1 failed */
    int slot;
    uint8_t rand[16];
    uint8_t sres[4], kc[8];
} sim_job;

static void sim_auth_thread(void *arg)
{
    (void)arg;
    sim_job.done = sim_run_gsm_algo(sim_job.slot, sim_job.rand, sim_job.sres,
                                    sim_job.kc) == SIM_OK ? 1 : -1;
}

/* Run one location-update attempt on a synced cell. Returns REG_ACCEPTED,
 * REG_REJECTED or REG_ERROR, and fills prog (may be NULL) as it goes. r must be
 * reg_init'd; ra is the CHANNEL REQUEST byte (establishment cause = location
 * updating). The bursts carry the cell's BSIC (c->bsic, from its SCH), which the
 * BTS checks an access burst's parity against. The TX power mode (os/dsp.h) must
 * be set (not L1_TX_OFF) before l1_start(). */
int reg_lu_run(gsm_reg_t *r, l1_cell_t *c, int cell_slot, int sim_slot,
               uint8_t ra, uint32_t budget_ms, reg_lu_progress_t *prog)
{
    rr_chan_desc_t ch;
    uint8_t ul[LAPDM_BLOCK];
    uint8_t bsic = c->bsic;
    reg_lu_progress_t p = {0};
    int have_ul, sim_pending = 0, sabm_tries = 0;
    uint8_t sabm[LAPDM_BLOCK];           /* the SABM, kept for retransmission */
    uint32_t last_ul = 0, last_track = 0;
    int dm_dbg = 0;
    uint32_t t0;

    if (prog)
        *prog = p;
    if (!c->synced)
        return REG_ERROR;
    /* 1+2: CHANNEL REQUEST (RACH), retransmitted until a matching IMMEDIATE
     * ASSIGNMENT comes back or the attempts run out - a real MS retransmits per
     * the cell's RACH control (one burst is easily lost to collision/weak decode).
     * The low nibble (random reference) is varied per attempt. */
    {
        /* the cell's RACH control: at most M + 1 attempts (TS 44.018 10.5.2.29) */
        static const uint8_t retrans[4] = { 1, 2, 4, 7 };
        int granted = 0, attempts = 1 + retrans[c->max_retrans & 3];

        if (attempts > LU_RACH_ATTEMPTS)
            attempts = LU_RACH_ATTEMPTS;
        for (int att = 0; att < attempts && !granted; att++) {
            uint8_t ra_a = (uint8_t)((ra & 0xf0) | ((ra + att) & 0x0f));
            /* the access instruction carries the next frame number (our count) */
            uint32_t rach_fn = (l1_api.next_fn() + c->fn_offset) % LU_FN_MOD;
            int rc;

            if (l1_api.access_post(c->index, cell_slot, bsic, ra_a) != DSP_OK)
                break;                  /* reported below: rach_sent tells how far */
            p.rach_sent++;
            if (prog)
                *prog = p;
            rc = wait_imm_assign(c, ra_a, rach_fn, &ch, LU_RACH_WAIT_MS, &p);
            if (rc < 0)
                break;
            granted = rc == 1;
        }
        if (prog)
            *prog = p;
        if (!granted)
            return REG_ERROR;
    }
    p.got_assign = 1;
    klog("lu: grant chan t=%u sub=%u tn=%u tsc=%u TA=%u arfcn=%u hop=%u", ch.chan_type,
         ch.subch, ch.tn, ch.tsc, ch.ta, ch.arfcn, ch.hopping);
    p.ch_type = ch.chan_type;
    p.ch_subch = ch.subch;
    p.ch_tn = ch.tn;
    p.ch_tsc = ch.tsc;
    if (prog)
        *prog = p;
    /* Re-assert cell tracking (command 3) for the dedicated phase: the RACH keyed
     * the PA, which disturbs the AFC, and a drifting AFC fails the SDCCH CRC. The
     * camp before the RACH is not enough once we have transmitted. Dedicated
     * mode (2) makes the DSP decode the SDCCH timeslot instead of half-camping
     * on the BCCH - it is what gets the exchange past the UA to authentication. */
    l1_track_mode = 2;
    l1_cell_track(c->slot, (uint8_t)(2 * c->gain));
    last_track = kernel_ticks();
    /* apply the grant's timing advance so the uplink lands in the BTS window */
    l1_tx_ta_set(ch.ta);
    klog("lu: retrack + TA %u", ch.ta);
    if (reg_begin(r, ul) < 0)
        return REG_ERROR;
    for (int i = 0; i < LAPDM_BLOCK; i++)
        sabm[i] = ul[i];              /* last uplink frame, kept for retransmission */
    have_ul = 1;
    /* 5: pump the SDCCH, one block a call. A real MS retransmits the SABM on
     * T200 until the UA; without it one lost block ended the attempt. Once the
     * link is up, AUTHENTICATION runs off-thread so the ring stays fed. */
    t0 = kernel_ticks();
    last_ul = kernel_ticks();
    while (kernel_ticks() - t0 < budget_ms &&
           r->state != REG_ACCEPTED && r->state != REG_REJECTED) {
        l1_dm_t st;

        /* Keep the AFC locked to the cell through the dedicated phase. A single
         * command-3 track corrects the synth once; it then free-runs and the
         * SDCCH CRC drifts within a few blocks (seen as good-then-bad downlink
         * even with no uplink in between), so re-assert it on a timer. */
        if (kernel_ticks() - last_track >= LU_RETRACK_MS) {
            l1_cell_track(c->slot, (uint8_t)(2 * c->gain));
            last_track = kernel_ticks();
        }
        l1_api.dm_sched(c, ch.subch, ch.tn, ch.tsc, bsic, LU_BLOCK_MS,
                        have_ul ? ul : (const uint8_t *)0, &st);
        if (have_ul && st.ul_done) {
            last_ul = kernel_ticks();
            if (!p.established)
                p.sabm_sent++;
            /* An uplink block keys the PA, which pulls the synth off the cell
             * hardest, and the network's reply (UA, then AUTHENTICATION REQUEST,
             * then ACCEPT) lands right after - re-lock the AFC before reading it. */
            l1_cell_track(c->slot, (uint8_t)(2 * c->gain));
            last_track = kernel_ticks();
        }
        have_ul = 0;
        p.dl_blocks = (uint16_t)(p.dl_blocks + st.dl_blocks);
        p.dl_bad = (uint16_t)(p.dl_bad + st.dl_bad);
        p.ul_bursts = (uint16_t)(p.ul_bursts + st.ul_bursts);
        if (dm_dbg < 8) {
            klog("lu: dm#%d ul=%lu done=%d dlw=%lu dl=%lu/%lu fn%%51=%lu hdr=%02x%02x fimov=%lu",
                 dm_dbg, (unsigned long)st.ul_bursts, st.ul_done,
                 (unsigned long)st.dl_bursts,
                 (unsigned long)st.dl_blocks, (unsigned long)st.dl_bad,
                 (unsigned long)st.dl_fn, st.last_dl[0], st.last_dl[1],
                 (unsigned long)dsp_info.fi_moves);
            klog("lu:   dm#%d seen=%lu hdr0=%u badfn=%lu", dm_dbg,
                 (unsigned long)st.dl_seen, st.dl_hdr0, (unsigned long)st.dl_bad_fn);
            dm_dbg++;
        }
        if (st.has_dl) {
            uint8_t out[LAPDM_BLOCK];
            int ov = 0, state;

            p.dl_addr = st.last_dl[0];
            p.dl_ctrl = st.last_dl[1];
            state = reg_rx(r, st.last_dl, out, &ov);

            if (r->dl.state == LAPDM_ESTABLISHED)
                p.established = 1;
            if (ov) {
                for (int i = 0; i < LAPDM_BLOCK; i++)
                    ul[i] = sabm[i] = out[i];
                have_ul = 1;
                sabm_tries = 0;
            }
            if (state == REG_NEED_SRES && !sim_pending) {
                p.reached_auth = 1;
                sim_job.done = 0;
                sim_job.slot = sim_slot;
                for (int i = 0; i < 16; i++)
                    sim_job.rand[i] = r->rand[i];
                if (thread_create("simauth", PRIO_NORMAL, 2048, sim_auth_thread, 0))
                    sim_pending = 1;
                else
                    break;              /* could not start the SIM thread */
            }
            if (state == REG_NEED_CIPHER) {
                p.ciphered = 1;
                klog("lu: CIPHER cmd sc=%u A5/%u", r->ciph_sc, r->ciph_alg + 1);
                if (r->ciph_sc)
                    l1_cipher_set(r->kc, r->ciph_alg);  /* enable A5 before COMPLETE */
                if (reg_cipher_complete(r, ul) > 0) {
                    for (int i = 0; i < LAPDM_BLOCK; i++)
                        sabm[i] = ul[i];
                    have_ul = 1;
                    sabm_tries = 0;
                }
            }
        }
        /* Retransmit the last uplink frame on T200 until the exchange moves on -
         * the SABM before the UA, then the AUTHENTICATION RESPONSE before the
         * ACCEPT. Each is marginal on this link, so a lost one must be resent, not
         * sent once. Most calls stay receive-only (ul == NULL) for clean RX. */
        if (!have_ul && sabm_tries < LU_SABM_MAX &&
            kernel_ticks() - last_ul >= LU_T200_MS) {
            for (int i = 0; i < LAPDM_BLOCK; i++)
                ul[i] = sabm[i];
            have_ul = 1;
            sabm_tries++;
        }
        if (sim_pending && sim_job.done) {
            if (sim_job.done == 1) {
                p.sim_ok = 1;
                for (int i = 0; i < 8; i++)
                    r->kc[i] = sim_job.kc[i];       /* for A5 if ciphering starts */
                if (reg_sres(r, sim_job.sres, ul) > 0) {
                    for (int i = 0; i < LAPDM_BLOCK; i++)
                        sabm[i] = ul[i];
                    have_ul = 1;
                    sabm_tries = 0;
                }
                sim_pending = 0;
            } else {
                sim_pending = 0;
                break;                  /* the SIM did not authenticate */
            }
        }
        if (prog)
            *prog = p;
    }
    /* let the SIM thread finish before its job is reused or the SIM is freed;
     * bounded, since sim_run_gsm_algo self-times-out (SIM_ETIMEOUT) anyway */
    for (int w = 0; sim_pending && !sim_job.done && w < 600; w++)
        thread_sleep(5);
    if (prog)
        *prog = p;
    return r->state == REG_ACCEPTED ? REG_ACCEPTED :
           r->state == REG_REJECTED ? REG_REJECTED : REG_ERROR;
}
