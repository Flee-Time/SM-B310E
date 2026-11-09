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

#define LU_RACH_MS      2000u           /* wait for IMMEDIATE ASSIGNMENT */
#define LU_BLOCK_MS     350u            /* ~ one SDCCH block period to pump */

/* CCCH FN mod 51 of the non-combined config (as l1cell.c cch_start). */
static int ccch_block(uint32_t m)
{
    return m == 6 || m == 12 || m == 16 || m == 22 || m == 26 ||
           m == 32 || m == 36 || m == 42 || m == 46;
}

/* Read CCCH until an IMMEDIATE ASSIGNMENT whose request reference (RA) matches
 * ours arrives, or the budget runs out. 0 + *ch filled, or -1. */
static int wait_imm_assign(l1_cell_t *c, uint8_t ra, rr_chan_desc_t *ch, uint32_t ms)
{
    uint32_t t0 = kernel_ticks();
    int burst = -1;

    while (kernel_ticks() - t0 < ms) {
        while (l1_api.fi_free() > 0) {
            uint32_t m = (l1_api.next_fn() + c->fn_offset) % 51u;
            int rc;

            if (burst < 0 && ccch_block(m))
                burst = 0;
            if (burst >= 0) {
                rc = l1_api.burst_post(c->index, 7, burst, 0, c->bsic & 7, c->slot);
                if (rc == DSP_OK && ++burst == 4)
                    burst = -1;
            } else {
                rc = l1_api.idle_post(1);
            }
            if (rc != DSP_OK)
                return -1;
        }
        l1_api.ask_more(3);
        l1_api.poll();
        {
            l1_rx_t rx;

            while (l1_api.rx_read(&rx) == 1) {
                if ((rx.hdr[0] != 7 && rx.hdr[0] != 8) || rx.hdr[1] != 0)
                    continue;
                if (rr_parse_imm_assign(rx.data, ch) == 0 && ch->ra == ra)
                    return 0;
            }
        }
        thread_sleep(2);
    }
    return -1;
}

/* Run one location-update attempt on a synced cell. Returns REG_ACCEPTED,
 * REG_REJECTED or REG_ERROR. r must be reg_init'd; cal is the channel's RF-cal
 * byte; ra is the CHANNEL REQUEST byte (establishment cause = location updating).
 * NOTE: the min-power clamp interlock in l1_access_post/uplink must be released
 * for a real registration (normal power, owner-cleared) before calling this. */
int reg_lu_run(gsm_reg_t *r, l1_cell_t *c, int cell_slot, uint8_t cal, int sim_slot,
               uint8_t ra, uint32_t budget_ms)
{
    rr_chan_desc_t ch;
    uint8_t ul[LAPDM_BLOCK];
    int have_ul;
    uint32_t t0;

    if (!c->synced)
        return REG_ERROR;
    /* 1: CHANNEL REQUEST (RACH) */
    if (l1_api.access_post(c->index, cell_slot, cal, ra) != DSP_OK)
        return REG_ERROR;
    /* 2: IMMEDIATE ASSIGNMENT -> the dedicated channel */
    if (wait_imm_assign(c, ra, &ch, LU_RACH_MS) != 0)
        return REG_ERROR;
    /* 3: establish the dedicated channel (command 3). sel/gain/aux are the
     * channel/mode selectors; their exact encoding is confirmed on-air. */
    l1_api.dm_est((uint16_t)(ch.chan_type << 3 | ch.subch), 0, 0);
    /* 4: SABM carrying LOCATION UPDATING REQUEST */
    if (reg_begin(r, ul) < 0)
        return REG_ERROR;
    have_ul = 1;
    /* 5: pump the SDCCH, driving the FSM, answering AUTHENTICATION via the SIM */
    t0 = kernel_ticks();
    while (kernel_ticks() - t0 < budget_ms &&
           r->state != REG_ACCEPTED && r->state != REG_REJECTED) {
        l1_dm_t st;

        l1_api.dm_sched(c, ch.subch, ch.tn, ch.tsc, cal, LU_BLOCK_MS,
                        have_ul ? ul : (const uint8_t *)0, &st);
        have_ul = 0;
        if (st.has_dl) {
            uint8_t out[LAPDM_BLOCK];
            int ov = 0, state = reg_rx(r, st.last_dl, out, &ov);

            if (ov) {
                for (int i = 0; i < LAPDM_BLOCK; i++)
                    ul[i] = out[i];
                have_ul = 1;
            }
            if (state == REG_NEED_SRES) {
                uint8_t sres[4], kc[8];

                if (sim_run_gsm_algo(sim_slot, r->rand, sres, kc) == SIM_OK &&
                    reg_sres(r, sres, ul) > 0)
                    have_ul = 1;
            }
        }
    }
    return r->state == REG_ACCEPTED ? REG_ACCEPTED :
           r->state == REG_REJECTED ? REG_REJECTED : REG_ERROR;
}
