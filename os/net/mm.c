/* B310E OS - GSM Mobility Management (04.08), MS side. See os/gsm_mm.h.
 * Self-contained stdint, host-unit-testable.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/gsm_mm.h"

int mm_encode_mi_digits(uint8_t *out, int type, const uint8_t *d, int n)
{
    int odd = n & 1, o = 0;

    if (n < 1)
        return 0;
    out[o++] = (uint8_t)((d[0] & 0xf) << 4 | odd << 3 | (type & 7));
    for (int i = 1; i < n; i += 2) {
        uint8_t lo = (uint8_t)(d[i] & 0xf);
        uint8_t hi = (uint8_t)(i + 1 < n ? d[i + 1] & 0xf : 0xf);

        out[o++] = (uint8_t)(hi << 4 | lo);
    }
    return o;
}

int mm_encode_mi_tmsi(uint8_t *out, uint32_t tmsi)
{
    out[0] = (uint8_t)(0xf0 | MI_TYPE_TMSI);    /* filler | even | type TMSI */
    out[1] = (uint8_t)(tmsi >> 24);
    out[2] = (uint8_t)(tmsi >> 16);
    out[3] = (uint8_t)(tmsi >> 8);
    out[4] = (uint8_t)tmsi;
    return 5;
}

void mm_encode_lai(uint8_t out[5], uint16_t mcc, uint16_t mnc, int mnc3, uint16_t lac)
{
    int m0 = mcc / 100 % 10, m1 = mcc / 10 % 10, m2 = mcc % 10;
    int n0, n1, n2;

    if (mnc3) {
        n0 = mnc / 100 % 10;
        n1 = mnc / 10 % 10;
        n2 = mnc % 10;
    } else {
        n0 = mnc / 10 % 10;
        n1 = mnc % 10;
        n2 = 0xf;                               /* 2-digit MNC: third digit filler */
    }
    out[0] = (uint8_t)(m1 << 4 | m0);
    out[1] = (uint8_t)(n2 << 4 | m2);
    out[2] = (uint8_t)(n1 << 4 | n0);
    out[3] = (uint8_t)(lac >> 8);
    out[4] = (uint8_t)lac;
}

int mm_encode_lu_request(uint8_t *out, int lu_type, int cksn,
                         uint16_t mcc, uint16_t mnc, int mnc3, uint16_t lac,
                         uint8_t classmark1, const uint8_t *imsi, int n)
{
    int o = 0, milen;

    out[o++] = MM_PD;
    out[o++] = MM_LU_REQUEST;
    out[o++] = (uint8_t)((cksn & 7) << 4 | (lu_type & 0x0f));
    mm_encode_lai(out + o, mcc, mnc, mnc3, lac);
    o += 5;
    out[o++] = classmark1;
    milen = mm_encode_mi_digits(out + o + 1, MI_TYPE_IMSI, imsi, n);
    out[o] = (uint8_t)milen;                    /* mobile identity: length then value */
    o += 1 + milen;
    return o;
}

int mm_msg_type(const uint8_t *l3, int len)
{
    if (len < 2 || (l3[0] & 0x0f) != MM_PD)
        return -1;
    return l3[1] & 0x3f;                        /* bits 8-7 may hold N(SD) */
}

int mm_parse_lu_reject(const uint8_t *l3, int len, uint8_t *cause)
{
    if (len < 3 || mm_msg_type(l3, len) != MM_LU_REJECT)
        return -1;
    if (cause)
        *cause = l3[2];
    return 0;
}

int mm_selftest(void)
{
    static const uint8_t imsi[15] = { 0, 0, 1, 0, 1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 };
    static const uint8_t accept[] = { 0x05, 0x02, 0x00 };
    static const uint8_t reject[] = { 0x05, 0x04, 0x0b };
    uint8_t lai[5], mi[9], tmsi[5], lu[32];
    uint8_t cause = 0;
    int ok = 0, n;

    mm_encode_lai(lai, 1, 1, 0, 0x1234);
    if (lai[0] == 0x00 && lai[1] == 0xf1 && lai[2] == 0x10 && lai[3] == 0x12 &&
        lai[4] == 0x34) ok++;
    n = mm_encode_mi_digits(mi, MI_TYPE_IMSI, imsi, 15);
    if (n == 8) ok++;
    if (mi[0] == 0x09) ok++;                    /* digit 0 | odd | IMSI */
    if (mi[7] == 0x98) ok++;                    /* last two digits 8,9 */
    if (mm_encode_mi_tmsi(tmsi, 0x12345678u) == 5 && tmsi[0] == 0xf4 &&
        tmsi[4] == 0x78) ok++;
    n = mm_encode_lu_request(lu, MM_LU_TYPE_NORMAL, 7, 1, 1, 0, 0x1234, 0x33, imsi, 15);
    if (lu[0] == MM_PD) ok++;
    if (lu[1] == MM_LU_REQUEST) ok++;
    if (lu[2] == 0x70) ok++;                    /* CKSN 7 | normal LU */
    if (n == 18 && lu[8] == 0x33 && lu[9] == 8 && lu[10] == 0x09 && lu[17] == 0x98) ok++;
    if (mm_msg_type(accept, (int)sizeof accept) == MM_LU_ACCEPT) ok++;
    if (mm_parse_lu_reject(reject, (int)sizeof reject, &cause) == 0 && cause == 0x0b) ok++;
    return ok;
}
