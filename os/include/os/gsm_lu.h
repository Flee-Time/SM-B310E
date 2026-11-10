/* B310E OS - the location-update L1 glue (net/gsm_lu.c): the on-air registration
 * driver that ties the registration FSM (gsm_reg) to the real layer 1 and the SIM.
 * Kept out of gsm_reg.h so that header stays pure/host-testable; this one pulls in
 * the OS layer-1 types.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef OS_GSM_LU_H
#define OS_GSM_LU_H

#include <stdint.h>
#include "os/gsm_reg.h"
#include "os/l1cell.h"

/* How far one attempt got, for the UI and the log (updated live). */
typedef struct {
    uint8_t rach_sent;       /* CHANNEL REQUESTs actually posted to the DSP */
    uint16_t cch_blocks;     /* good CCCH blocks read while waiting for the grant */
    uint16_t imm_seen;       /* IMMEDIATE ASSIGNMENTs seen (any request reference) */
    uint8_t ra_hits;         /* of those, with our RA byte (ours or another phone's) */
    int8_t ref_d;            /* the last RA hit's frame vs our RACH's, frames; 127: not
                              * within LU_REF_SLACK (another phone's, or our FN off) */
    uint8_t got_assign;      /* one matched our RA + frame -> dedicated channel */
    uint8_t established;      /* LAPDm link up on the SDCCH (UA) */
    uint8_t reached_auth;    /* AUTHENTICATION REQUEST seen */
    uint8_t sim_ok;          /* the SIM returned SRES for it */
    uint8_t ciphered;        /* CIPHERING MODE COMMAND seen and answered */
    uint16_t dl_blocks;      /* downlink SDCCH blocks received (good CRC) */
    uint16_t dl_bad;         /* downlink SDCCH blocks with a bad CRC */
    uint16_t ul_bursts;      /* uplink bursts sent */
    uint8_t sabm_sent;       /* complete SABM blocks (4 bursts) sent */
    uint8_t dl_addr, dl_ctrl; /* the last downlink block's LAPDm address+control */
    uint8_t ch_type, ch_subch, ch_tn, ch_tsc; /* the assigned dedicated channel */
} reg_lu_progress_t;

/* Run one location-update (registration) attempt on a synced cell c. r must be
 * reg_init'd; sim_slot's SIM must be active (for RUN GSM ALGORITHM). The bursts
 * are coded with c->bsic (from its SCH); ra is the CHANNEL REQUEST (establishment
 * cause) byte, and budget_ms bounds the SDCCH exchange. prog (may be NULL) is
 * filled as it goes. Returns REG_ACCEPTED, REG_REJECTED or REG_ERROR.
 *
 * This TRANSMITS (RACH + uplink), gated by the TX power mode (os/dsp.h): in
 * L1_TX_MIN the burst goes at the minimum setpoint (a tower will not decode it -
 * a safe dry run that times out at the IMMEDIATE ASSIGNMENT); L1_TX_NORMAL is a
 * real, owner-cleared registration. Set the mode before l1_start().
 *
 * AUTHENTICATION runs the SIM (RUN GSM ALGORITHM, ~1s) on its own thread so the
 * SDCCH frame pump keeps feeding L1 while it computes - a synchronous call would
 * starve the frame-instruction ring. */
int reg_lu_run(gsm_reg_t *r, l1_cell_t *c, int cell_slot, int sim_slot,
               uint8_t ra, uint32_t budget_ms, reg_lu_progress_t *prog);

#endif
