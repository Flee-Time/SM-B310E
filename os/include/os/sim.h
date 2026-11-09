/* B310E OS — SIM card reader (drivers/sim.c).
 *
 * One controller (0x85000000) serves both slots, so one slot is active at
 * a time: sim_activate() powers a slot and reads its ATR, sim_apdu() runs
 * T=0 commands on it, sim_deactivate() powers it down again. Every call
 * blocks (up to about a second) and runs in thread context only; calls
 * from several threads are serialised.
 *
 * The card stays at F=372 / D=1 (3.25 MHz clock, no PPS). The supply
 * starts at class C (1.8 V); class B (3 V) follows when there is no ATR,
 * or when the ATR's class indicator (T=15 TA) leaves out class C. A GSM
 * layer that finds the SIM is not 1.8 V capable (TS 51.011 file
 * characteristics after SELECT MF) re-activates with
 * sim_activate_class(slot, SIM_CLASS_B, ...).
 *
 * Privacy: the ATR's historical bytes and anything read from the card
 * (ICCID, IMSI ...) must never be logged or stored by the driver's users
 * unless the user asked for it; the driver itself logs nothing.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef OS_SIM_H
#define OS_SIM_H

#include <stdbool.h>
#include <stdint.h>

#define SIM_SLOTS     2
#define SIM_ATR_MAX   33

/* supply classes (ISO/IEC 7816-3) */
#define SIM_CLASS_C   1             /* 1.8 V */
#define SIM_CLASS_B   2             /* 3 V */

/* errors (negative); sim_apdu() returns SW1SW2 (> 0) on success */
#define SIM_OK          0
#define SIM_ENOCARD   (-1)          /* no ATR at any class */
#define SIM_EBADATR   (-2)          /* bytes came, but no valid TS */
#define SIM_ETIMEOUT  (-3)          /* the card stopped answering */
#define SIM_EPROTO    (-4)          /* not a T=0 procedure byte */
#define SIM_EBUSY     (-5)          /* the other slot is active */
#define SIM_EINVAL    (-6)
#define SIM_ENOTACTIVE (-7)         /* sim_apdu() on a slot not active */

typedef struct {
    uint8_t raw[SIM_ATR_MAX];       /* as received (inverse: converted) */
    uint8_t len;
    bool inverse;                   /* TS 0x3F (inverse convention) */
    bool complete;                  /* every byte T0/TDi announce came */
    bool tck_ok;                    /* TCK right, or none needed (T=0 only) */
    uint8_t proto;                  /* first offered protocol (TD1), 0 without */
    uint16_t protocols;             /* bit n: T=n offered (T=15 left out) */
    uint8_t classes;                /* T=15 TA bits 6:1: 1 A, 2 B, 4 C; 0 none */
    int16_t ta1, tc1;               /* Fi/Di and extra guard time, -1 absent */
    uint8_t hist_off, hist_len;     /* historical bytes: raw[hist_off..] */
    uint8_t cls;                    /* SIM_CLASS_C or SIM_CLASS_B used */
    uint16_t mv;                    /* the supply: 1800 or 3000 */
} sim_atr_t;

/* Power slot 0 or 1 and read its ATR, at class C and then class B.
 * SIM_OK with *atr filled (the slot stays active), or an error with the
 * slot powered down again (atr->len tells whether bytes came). */
int sim_activate(int slot, sim_atr_t *atr);

/* The same at one class only (SIM_CLASS_C or SIM_CLASS_B). */
int sim_activate_class(int slot, int cls, sim_atr_t *atr);

/* A T=0 command on the active slot. hdr = CLA INS P1 P2 P3.
 *  - data != NULL: case 3, the P3 bytes of data go to the card;
 *  - resp != NULL: case 2, up to P3 bytes (0 means 256) come back into
 *    resp, which must hold that many; *resp_len (may be NULL) gets the count;
 *  - both NULL: case 1 (P3 should be 0).
 * Returns SW1SW2 (0x6xxx or 0x9xxx), or a negative error. 61xx / 6Cxx /
 * 9Fxx are returned as they are: GET RESPONSE is the caller's job. */
int sim_apdu(int slot, const uint8_t hdr[5], const uint8_t *data,
             uint8_t *resp, int *resp_len);

/* Power the slot down (RST low, clock off, I/O low, supply off). */
void sim_deactivate(int slot);

int sim_active_slot(void);          /* -1: none */
const char *sim_strerror(int err);

/* GSM authentication on the active slot: RUN GSM ALGORITHM over the 16-octet
 * RAND -> SRES (4) + Kc (8). SIM_OK, or the SW / a negative error. Nothing of
 * the RAND/SRES/Kc is logged by the driver. */
int sim_run_gsm_algo(int slot, const uint8_t rand[16], uint8_t sres[4], uint8_t kc[8]);

/* Parse an ATR in raw[0..len) into the other fields (sim_activate does
 * this; exported for tests and the GSM layer). */
void sim_atr_parse(sim_atr_t *atr);

#endif
