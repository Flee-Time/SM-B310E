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

/* Run one location-update (registration) attempt on a synced cell c. r must be
 * reg_init'd; sim_slot's SIM must be active (for RUN GSM ALGORITHM). cal is the
 * channel RF-cal byte, ra the CHANNEL REQUEST (establishment cause) byte, and
 * budget_ms bounds the SDCCH exchange. Returns REG_ACCEPTED, REG_REJECTED or
 * REG_ERROR.
 *
 * This TRANSMITS (RACH + uplink). The min-power clamp interlock in access_post
 * gates it: with the clamp armed the burst goes at the minimum setpoint (a real
 * tower will not decode it - a safe dry run); a normal-power run needs the clamp
 * released, which is the owner's deliberate, cleared step.
 *
 * KNOWN LIMITATION (normal-power run): on AUTHENTICATION it calls
 * sim_run_gsm_algo() synchronously, a ~1s blocking SIM exchange, inside the
 * SDCCH pump - that would starve the frame-instruction ring. The auth step is
 * never reached at min power (no IMMEDIATE ASSIGNMENT comes back), so the dry run
 * is unaffected; a real run needs the SIM computation moved off the pump thread. */
int reg_lu_run(gsm_reg_t *r, l1_cell_t *c, int cell_slot, uint8_t cal, int sim_slot,
               uint8_t ra, uint32_t budget_ms);

#endif
