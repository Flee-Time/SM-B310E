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
#define LU_BLOCK_MS     115u            /* ~ one SDCCH block period (235ms/2) so each
                                         * dm_sched call re-tracks right before the
                                         * block it reads - the AFC free-runs on the
                                         * dedicated channel, so a short call keeps the
                                         * synth from drifting off between blocks. */
#define LU_RETRACK_MS   0u              /* re-track every call to hold the AFC (decode /
                                         * est needs it; backing off lost est=1). TX
                                         * froze independently of this - see the SACCH
                                         * arming below, disabled to test if its TXBUFQ
                                         * posts were jamming the queue head (txout=1). */
#define LU_T200_MS      300u            /* resend the SABM / an unacked I-frame by this */
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

/* Assemble the 23-byte uplink SACCH block: the 2-byte L1 header (ordered MS power
 * level, timing advance) then a LAPDm UI frame carrying the RR MEASUREMENT REPORT.
 * l1_sacch_set arms it and l1_dm_sched posts it every SACCH period, so the radio
 * link survives the dedicated exchange - without it the network's radio-link
 * timeout releases the SDCCH part-way through auth/cipher. */
static void build_sacch(uint8_t blk[LAPDM_BLOCK], uint8_t pwr, uint8_t ta, int rxlev)
{
    uint8_t rep[18];
    int n = rr_encode_meas_report(rep, rxlev);

    blk[0] = (uint8_t)(pwr & 0x1f);             /* L1 header: MS power level */
    blk[1] = (uint8_t)(ta & 0x3f);              /* L1 header: timing advance */
    blk[2] = LAPDM_ADDR(0, 0);                  /* SAPI 0, command */
    blk[3] = LAPDM_U_UI;                        /* unnumbered information (unacked) */
    blk[4] = (uint8_t)((n & 0x3f) << 2 | 1u);   /* length | M=0 | EL=1 */
    for (int i = 0; i < n; i++)
        blk[5 + i] = rep[i];
    for (int i = 5 + n; i < LAPDM_BLOCK; i++)
        blk[i] = LAPDM_FILL;
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
    int have_ul, sim_pending = 0, sabm_tries = 0, cm_sent = 0;
    uint8_t sabm[LAPDM_BLOCK];           /* the SABM, kept for retransmission */
    uint8_t sacch[LAPDM_BLOCK];          /* the uplink SACCH keep-alive, armed for the run */
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
    /* ROOT CAUSE of the decode drift (RE 2026-11-10): decode is good for ~1s after
     * the camp then falls to ~0, in BOTH mode 2 and mode 0 (tested, identical). It is
     * not the mode - it is that the dedicated scheduler reads ONLY the SDCCH timeslot
     * (tn1) and never the BCCH FCCH (tn0) again, so the DSP's AFC has no reference and
     * free-runs. The grant is on the SAME carrier as the BCCH (arfcn 998, tn1, no hop),
     * so the fix is to interleave FCCH reads on tn0 at the FCCH frames (fn%51 in
     * {0,10,20,30,40}) during the dedicated phase to re-lock the AFC - what stock's
     * state machine does internally. l1_fcch_post touches frame_number directly, so
     * the scheduler interleave must keep the per-frame accounting exact. TODO. */
    l1_cell_track(c->slot, (uint8_t)(2 * c->gain));
    last_track = kernel_ticks();
    /* apply the grant's timing advance so the uplink lands in the BTS window */
    l1_tx_ta_set(ch.ta);
    klog("lu: retrack + TA %u", ch.ta);
    if (reg_begin(r, ul) < 0)
        return REG_ERROR;
    /* Log the LU REQUEST's old-LAI + classmark we send (non-SIM fields only - no IMSI).
     * A wrong PLMN here (esp. mnc3 digit-count: TT 286-03 is a 2-digit MNC) makes the
     * network read us as a disallowed roamer -> LU REJECT cause 13. */
    klog("lu: LU-REQ oldLAI mcc=%u mnc=%u mnc3=%u lac=%u cm1=%02x imsilen=%u",
         r->mcc, r->mnc, r->mnc3, r->lac, r->classmark1, r->imsi_len);
    /* IMSI PLMN prefix only (MCC+MNC = first 5 digits) - loggable (it is the PLMN id,
     * not the subscriber MSIN) and tells us the SIM read a real TT IMSI (28603...). A
     * wrong prefix = bad SIM read -> network sees a foreign/invalid subscriber. */
    if (r->imsi_len >= 5)
        klog("lu: LU-REQ imsi-plmn %u%u%u%u%u", r->imsi[0], r->imsi[1], r->imsi[2],
             r->imsi[3], r->imsi[4]);
    for (int i = 0; i < LAPDM_BLOCK; i++)
        sabm[i] = ul[i];              /* last uplink frame, kept for retransmission */
    have_ul = 1;
    /* Arm the uplink SACCH for the whole dedicated phase: the scheduler now posts
     * a MEASUREMENT REPORT at this subchannel's SACCH window every 102-multiframe,
     * so the network does not time the radio link out mid-exchange. ch.ta is the
     * grant's timing advance; the stock reports its used power + TA in the L1
     * header. Disarmed after the loop (sacch is on this stack frame). */
    build_sacch(sacch, c->ms_txpwr_max_cch, ch.ta, c->rxlev);
    (void)sacch;
    /* DIAGNOSTIC: SACCH arming disabled. Its block posts ahead of the SABM and the
     * DSP would not drain it (txout froze at 1), jamming the queue head and killing
     * all later uplink. Test whether, without it, txout advances and TX sustains to
     * auth/cipher (as the pre-SACCH build reached this morning). */
    /* l1_sacch_set(sacch); */
    /* 5: pump the SDCCH, one block a call. A real MS retransmits the SABM on
     * T200 until the UA; without it one lost block ended the attempt. Once the
     * link is up, AUTHENTICATION runs off-thread so the ring stays fed. */
    t0 = kernel_ticks();
    last_ul = kernel_ticks();
    uint16_t prev_l3 = 0;              /* log each decoded L3 message as it arrives */
    while (kernel_ticks() - t0 < budget_ms &&
           r->state != REG_ACCEPTED && r->state != REG_REJECTED) {
        l1_dm_t st;

        /* Keep the AFC locked to the cell through the dedicated phase. A single
         * command-3 track corrects the synth once; it then free-runs and the
         * SDCCH CRC drifts within a few blocks (seen as good-then-bad downlink
         * even with no uplink in between), so re-assert it on a timer - and
         * always right before a call that will transmit, so the uplink burst
         * leaves on the cell's exact frequency (an off-frequency burst is what
         * the BTS silently drops, leaving a clean LU REQUEST / AUTHENTICATION
         * RESPONSE that the network never acts on). */
        if (have_ul || kernel_ticks() - last_track >= LU_RETRACK_MS) {
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
        if (dm_dbg < 28) {
            klog("lu: dm#%d ul=%lu sa=%lu fc=%lu done=%d dlw=%lu dl=%lu/%lu fn%%51=%lu hdr=%02x%02x fimov=%lu",
                 dm_dbg, (unsigned long)st.ul_bursts, (unsigned long)st.sacch_bursts,
                 (unsigned long)st.fcch_reads,
                 st.ul_done, (unsigned long)st.dl_bursts,
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
            if (r->l3_msgs != prev_l3) {   /* a fresh L3 message - log PD+type so we see
                                            * the whole downlink sequence (a CLASSMARK
                                            * ENQUIRY before the CIPHER cmd would explain
                                            * the A5/3 request) */
                klog("lu: L3 rx PD=%u type=0x%02x", r->last_pd, r->last_mtype);
                prev_l3 = r->l3_msgs;
            }

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
                /* This DSP implements A5/1 only (alg 0); A5/2 and A5/3 (KASUMI) are
                 * absent. If the network commands anything else, do NOT fake a
                 * CIPHERING MODE COMPLETE: a garbage ciphered COMPLETE the network
                 * cannot validate both fails the attempt AND reinforces the VLR's
                 * belief that this IMSI does A5/3 (bidding-down lock), so every retry
                 * is commanded A5/3 again. Abort cleanly instead - send nothing, let
                 * the channel release - so the VLR is not fed a bogus A5/3 completion. */
                if (r->ciph_sc && r->ciph_alg != 0) {
                    klog("lu: A5/%u unsupported (DSP is A5/1 only) - abort, no COMPLETE",
                         r->ciph_alg + 1);
                    break;      /* stop responding; let the channel release */
                }
                if (r->ciph_sc)
                    l1_cipher_set(r->kc, r->ciph_alg, c->fn_offset);  /* A5 before COMPLETE */
                {
                    int cc = reg_cipher_complete(r, ul);

                    klog("lu: CIPHER COMPLETE build=%d (tx_pend=%u vs=%u va=%u)", cc,
                         r->dl.tx_pending, r->dl.vs, r->dl.va);
                    if (cc > 0) {
                        for (int i = 0; i < LAPDM_BLOCK; i++)
                            sabm[i] = ul[i];
                        have_ul = 1;
                        sabm_tries = 0;
                    }
                }
            }
        }
        /* Once the link is up, the first thing we send is the early CLASSMARK
         * CHANGE (A5/1 only) so the network ciphers with A5/1 at CIPHERING MODE
         * COMMAND - the stock sends it from NV (support_early_classmark); without
         * it TT asks for A5/3, which our DSP path does not do. Preempts the SABM
         * retransmit below, which would otherwise keep have_ul set and starve it. */
        if (!have_ul && p.established && !cm_sent && reg_classmark_change(r, ul) > 0) {
            for (int i = 0; i < LAPDM_BLOCK; i++)
                sabm[i] = ul[i];
            have_ul = 1;
            cm_sent = 1;
            sabm_tries = 0;
            klog("lu: early CLASSMARK CHANGE (A5/1 only)");
        }
        /* Hold the SDCCH link up the way the stock MS does - a capture of its
         * location update shows ~23 back-to-back uplink blocks, while going
         * receive-only between steps lets the link stall before auth/cipher/accept
         * finish (and a dropped CLASSMARK CHANGE never takes effect). In priority:
         *   (a) before the UA, retransmit the SABM on T200 until establishment;
         *   (b) once up, retransmit a genuinely unacked I-frame on T200 (a lost
         *       AUTHENTICATION RESPONSE / CIPHERING MODE COMPLETE must be resent);
         *   (c) otherwise send an RR keep-alive on every free uplink opportunity,
         *       which also keeps our N(R) ack flowing so the network advances.
         * (b) resends sabm[] - the last real frame built - only while it is still
         * unacked, so an already-acked I-frame is not blindly repeated (which
         * desynced N(S) before); (c) touches no sequence state. */
        if (!have_ul && r->dl.state != LAPDM_ESTABLISHED &&
            sabm_tries < LU_SABM_MAX && kernel_ticks() - last_ul >= LU_T200_MS) {
            for (int i = 0; i < LAPDM_BLOCK; i++)
                ul[i] = sabm[i];
            have_ul = 1;
            sabm_tries++;
        } else if (!have_ul && r->dl.state == LAPDM_ESTABLISHED && r->dl.tx_pending &&
                   kernel_ticks() - last_ul >= LU_T200_MS) {
            for (int i = 0; i < LAPDM_BLOCK; i++)
                ul[i] = sabm[i];
            have_ul = 1;
        } else if (!have_ul && r->dl.state == LAPDM_ESTABLISHED &&
                   reg_keepalive(r, ul) > 0) {
            have_ul = 1;        /* every spare multiframe, not every other one:
                                 * gating this on LU_FILL_MS after last_ul (which
                                 * updates on each send) made us skip the very next
                                 * window, so blocks went out at 470 ms - half the
                                 * SDCCH rate - and the exchange faded. One block per
                                 * ~235 ms window is the SDCCH maximum; stock's extra
                                 * density is the SACCH on top. */
        }
        if (sim_pending && sim_job.done) {
            if (sim_job.done == 1) {
                p.sim_ok = 1;
                for (int i = 0; i < 8; i++)
                    r->kc[i] = sim_job.kc[i];       /* for A5 if ciphering starts */
                {
                    int ar = reg_sres(r, sim_job.sres, ul);

                    klog("lu: AUTH RESPONSE build=%d (est=%u tx_pend=%u vs=%u va=%u)", ar,
                         r->dl.state == LAPDM_ESTABLISHED, r->dl.tx_pending, r->dl.vs,
                         r->dl.va);
                    if (ar > 0) {
                        for (int i = 0; i < LAPDM_BLOCK; i++)
                            sabm[i] = ul[i];
                        have_ul = 1;
                        sabm_tries = 0;
                    }
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
    l1_sacch_set(0);                     /* disarm: sacch[] leaves scope on return */
    /* let the SIM thread finish before its job is reused or the SIM is freed;
     * bounded, since sim_run_gsm_algo self-times-out (SIM_ETIMEOUT) anyway */
    for (int w = 0; sim_pending && !sim_job.done && w < 600; w++)
        thread_sleep(5);
    if (prog)
        *prog = p;
    return r->state == REG_ACCEPTED ? REG_ACCEPTED :
           r->state == REG_REJECTED ? REG_REJECTED : REG_ERROR;
}
