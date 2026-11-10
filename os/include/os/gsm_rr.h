/* B310E OS - GSM Radio Resource (04.08) message parsing, MS side. The piece we
 * need first is IMMEDIATE ASSIGNMENT: the CCCH block (we already receive these)
 * that grants a dedicated channel (SDCCH) after our RACH - the bridge from the
 * receive path into the dedicated channel + LAPDm.
 *
 * Pure stdint parsing, no OS deps, so it unit-tests on the host. Bit layouts are
 * GSM 04.08; confirmed end-to-end against the live network at registration.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef OS_GSM_RR_H
#define OS_GSM_RR_H

#include <stdint.h>

#define RR_PD               0x06        /* protocol discriminator: Radio Resource */

/* RR message types (octet after PD) */
#define RR_IMM_ASSIGN       0x3f
#define RR_IMM_ASSIGN_EXT   0x39
#define RR_IMM_ASSIGN_REJ   0x3a
#define RR_CHAN_RELEASE     0x0d
#define RR_CIPH_MODE_CMD    0x35
#define RR_CIPH_MODE_COMPLETE 0x32
#define RR_PAGING_1         0x21
#define RR_PAGING_2         0x22
#define RR_PAGING_3         0x24

enum { RR_CH_TCHF, RR_CH_TCHH, RR_CH_SDCCH4, RR_CH_SDCCH8, RR_CH_UNKNOWN };

typedef struct {
    uint8_t chan_type;                  /* RR_CH_* */
    uint8_t subch;                      /* subchannel (SDCCH) */
    uint8_t tn;                         /* timeslot */
    uint8_t tsc;                        /* training sequence code */
    uint16_t arfcn;                     /* absolute radio frequency channel */
    uint8_t hopping;                    /* frequency hopping (then maio/hsn) */
    uint8_t maio, hsn;
    uint8_t ta;                         /* timing advance (6 bits) */
    uint8_t ra;                         /* request reference: the RACH request byte */
    uint16_t ref_fn;                    /* request reference octets 2-3 (TS 44.018
                                         * 10.5.2.30): T1' << 11 | T3 << 5 | T2 of the
                                         * frame the RACH was heard in */
} rr_chan_desc_t;

/* The L3 message type of a CCCH/DCCH block (its PD must be RR), or -1. */
int rr_msg_type(const uint8_t block[23]);
/* Decode a 3-octet Channel Description IE (10.5.2.5) into d. 0, or -1. */
int rr_parse_chan_desc(const uint8_t cd[3], rr_chan_desc_t *d);
/* Parse an IMMEDIATE ASSIGNMENT CCCH block into the assigned channel + TA + RA. */
int rr_parse_imm_assign(const uint8_t block[23], rr_chan_desc_t *d);

#define RR_SELFTEST_TOTAL 9
int rr_selftest(void);

/* CIPHERING MODE COMPLETE (no IMEISV); 2 bytes. */
int rr_encode_ciph_mode_complete(uint8_t out[2]);

#endif
