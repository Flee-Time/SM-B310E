/* B310E OS - GSM Radio Resource (04.08) parsing, MS side. See os/gsm_rr.h.
 * Self-contained stdint, host-unit-testable.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/gsm_rr.h"

int rr_msg_type(const uint8_t block[23])
{
    /* CCCH/DCCH L3 after the L2 pseudo-length (octet 0): octet 1 = skip|PD,
     * octet 2 = message type. */
    if ((block[1] & 0x0f) != RR_PD)
        return -1;
    return block[2];
}

int rr_parse_chan_desc(const uint8_t cd[3], rr_chan_desc_t *d)
{
    uint8_t t = (uint8_t)(cd[0] >> 3);          /* channel type + subch, bits 8-4 */

    d->tn = (uint8_t)(cd[0] & 7);
    d->hopping = (uint8_t)((cd[1] >> 4) & 1);   /* H bit (b5) */
    if (t == 0x01) {                            /* 00001 */
        d->chan_type = RR_CH_TCHF;
        d->subch = 0;
    } else if ((t & 0x1e) == 0x02) {            /* 0001T */
        d->chan_type = RR_CH_TCHH;
        d->subch = (uint8_t)(t & 1);
    } else if ((t & 0x1c) == 0x04) {            /* 001TT */
        d->chan_type = RR_CH_SDCCH4;
        d->subch = (uint8_t)(t & 3);
    } else if ((t & 0x18) == 0x08) {            /* 01TTT */
        d->chan_type = RR_CH_SDCCH8;
        d->subch = (uint8_t)(t & 7);
    } else {
        d->chan_type = RR_CH_UNKNOWN;
        d->subch = 0;
    }
    if (d->hopping) {                           /* MAIO/HSN instead of a fixed ARFCN */
        d->maio = (uint8_t)((cd[1] & 0x0f) << 2 | cd[2] >> 6);
        d->hsn = (uint8_t)(cd[2] & 0x3f);
        d->tsc = (uint8_t)(cd[1] >> 5);
        d->arfcn = 0;
    } else {
        d->tsc = (uint8_t)(cd[1] >> 5);         /* bits 8-6 */
        d->arfcn = (uint16_t)((cd[1] & 3) << 8 | cd[2]);  /* 10-bit ARFCN */
        d->maio = d->hsn = 0;
    }
    return 0;
}

int rr_parse_imm_assign(const uint8_t block[23], rr_chan_desc_t *d)
{
    if (rr_msg_type(block) != RR_IMM_ASSIGN)
        return -1;
    /* octet 3: page mode/dedicated; 4-6: Channel Description; 7-9: Request
     * Reference (7 = RA); 10: Timing Advance; 11: Mobile Allocation length. */
    if (rr_parse_chan_desc(block + 4, d) < 0)
        return -1;
    d->ra = block[7];
    d->ref_fn = (uint16_t)(block[8] << 8 | block[9]);   /* T1'(5) T3(6) T2(5) */
    d->ta = (uint8_t)(block[10] & 0x3f);        /* timing advance, 6 bits */
    return 0;
}

int rr_selftest(void)
{
    /* IMMEDIATE ASSIGNMENT: SDCCH/8 subch 2, TN 1, TSC 5, ARFCN 54, TA 0, RA 0x3f */
    static const uint8_t ia[23] = {
        0x2d,                           /* L2 pseudo-length */
        0x06,                           /* PD = RR */
        0x3f,                           /* IMMEDIATE ASSIGNMENT */
        0x00,                           /* page mode / dedicated */
        0x51, 0xa0, 0x36,               /* Channel Description */
        0x3f, 0x00, 0x00,               /* Request Reference (RA, frame) */
        0x00,                           /* Timing Advance */
        0x00,                           /* Mobile Allocation length */
        0x2b, 0x2b, 0x2b, 0x2b, 0x2b, 0x2b, 0x2b, 0x2b, 0x2b, 0x2b,
    };
    static const uint8_t tchf[3] = { 0x08, 0x00, 0x00 };  /* TCH/F, TN 0 */
    rr_chan_desc_t d;
    int ok = 0;

    if (rr_msg_type(ia) == RR_IMM_ASSIGN) ok++;
    if (rr_parse_imm_assign(ia, &d) == 0) ok++;
    if (d.chan_type == RR_CH_SDCCH8) ok++;
    if (d.subch == 2) ok++;
    if (d.tn == 1) ok++;
    if (d.tsc == 5) ok++;
    if (d.arfcn == 54) ok++;
    if (d.ra == 0x3f && d.ta == 0 && !d.hopping) ok++;
    if (rr_parse_chan_desc(tchf, &d) == 0 && d.chan_type == RR_CH_TCHF && d.tn == 0) ok++;
    return ok;
}
