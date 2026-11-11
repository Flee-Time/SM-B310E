/* B310E OS - the location-updating (registration) orchestration, MS side. The
 * FSM that drives L1+L2+L3: it builds the LOCATION UPDATING REQUEST in a LAPDm
 * SABM, then processes the SDCCH exchange (UA, AUTHENTICATION, IDENTITY,
 * CIPHERING, ACCEPT/REJECT) and produces the replies. The L1 glue (RACH, reading
 * IMMEDIATE ASSIGNMENT, driving the SDCCH scheduler) wraps this; the FSM's message
 * logic is pure and host-unit-testable with a scripted network exchange.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef OS_GSM_REG_H
#define OS_GSM_REG_H

#include <stdint.h>
#include "os/lapdm.h"
#include "os/gsm_rr.h"
#include "os/gsm_mm.h"

enum {
    REG_IDLE,
    REG_WAIT_UA,            /* SABM(+LU request) sent, awaiting UA */
    REG_RUNNING,            /* link up, exchanging L3 */
    REG_NEED_SRES,          /* AUTHENTICATION REQUEST seen; caller must run the SIM */
    REG_ACCEPTED,           /* LOCATION UPDATING ACCEPT */
    REG_REJECTED,           /* LOCATION UPDATING REJECT (see reject_cause) */
    REG_ERROR,
    REG_NEED_CIPHER,       /* CIPHERING MODE COMMAND seen; enable A5 + send COMPLETE */
};

typedef struct {
    uint8_t state;
    uint8_t imsi[16];
    uint8_t imsi_len;
    uint8_t imei[16];       /* IMEI digits (0..15) for IDENTITY REQUEST; 0 len: none */
    uint8_t imei_len;
    uint16_t mcc, mnc, lac;
    uint8_t mnc3;
    uint8_t classmark1;
    uint32_t tmsi;          /* stored TMSI (EF_LOCI); presented when use_tmsi */
    uint8_t use_tmsi;       /* 1 = identify the LU REQUEST by TMSI, not IMSI */
    lapdm_dl_t dl;
    uint8_t rand[16];       /* from AUTHENTICATION REQUEST, for the SIM */
    uint8_t kc[8];          /* ciphering key from the SIM, for A5 */
    uint8_t ciph_sc, ciph_alg;  /* CIPHERING MODE COMMAND: start flag, algorithm */
    uint8_t reject_cause;
    uint8_t last_pd, last_mtype;    /* last delivered L3 message (diag) */
    uint16_t l3_msgs;               /* delivered L3 messages count (diag) */
} gsm_reg_t;

void reg_init(gsm_reg_t *r, const uint8_t *imsi, int imsi_len,
              uint16_t mcc, uint16_t mnc, int mnc3, uint16_t lac, uint8_t classmark1);
/* Set the IMEI (digit bytes, 0-9) that IDENTITY REQUEST answers with when the
 * network asks for identity type IMEI or IMEISV. n = 0 clears it (then we fall
 * back to the IMSI, the old behaviour). Call after reg_init. */
void reg_set_imei(gsm_reg_t *r, const uint8_t *digits, int n);
/* Identify the LU REQUEST by a stored TMSI (from EF_LOCI) instead of the IMSI, so
 * the network sees a known subscriber (stock's path). Call after reg_init, only
 * when the stored TMSI is valid and its LAI matches the serving cell. */
void reg_set_tmsi(gsm_reg_t *r, uint32_t tmsi);
/* Build the SABM carrying LOCATION UPDATING REQUEST. Returns a frame length. */
int reg_begin(gsm_reg_t *r, uint8_t out[LAPDM_BLOCK]);
/* Feed one received SDCCH frame; may write a reply (out, out_valid). Returns the
 * new state. When it returns REG_NEED_SRES, the caller runs the SIM and calls
 * reg_sres with the 4-octet SRES. */
int reg_rx(gsm_reg_t *r, const uint8_t in[LAPDM_BLOCK], uint8_t out[LAPDM_BLOCK], int *out_valid);
/* Provide SRES after a REG_NEED_SRES; builds the AUTHENTICATION RESPONSE frame. */
int reg_sres(gsm_reg_t *r, const uint8_t sres[4], uint8_t out[LAPDM_BLOCK]);
/* Build CIPHERING MODE COMPLETE after enabling A5; 0 when not awaiting it. */
int reg_cipher_complete(gsm_reg_t *r, uint8_t out[LAPDM_BLOCK]);
/* Early CLASSMARK CHANGE advertising A5/1 only (forces the network's cipher to A5/1). */
int reg_classmark_change(gsm_reg_t *r, uint8_t out[LAPDM_BLOCK]);
/* Idle RR keep-alive to hold the SDCCH uplink active between L3 steps; 0 if the
 * link is not established. */
int reg_keepalive(gsm_reg_t *r, uint8_t out[LAPDM_BLOCK]);

#define REG_SELFTEST_TOTAL 10
int reg_selftest(void);

#endif
