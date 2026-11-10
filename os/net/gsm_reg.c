/* B310E OS - location-updating (registration) FSM, MS side. See os/gsm_reg.h.
 * Pure logic over LAPDm/RR/MM, host-unit-testable.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/gsm_reg.h"

void reg_init(gsm_reg_t *r, const uint8_t *imsi, int imsi_len,
              uint16_t mcc, uint16_t mnc, int mnc3, uint16_t lac, uint8_t classmark1)
{
    for (int i = 0; i < (int)sizeof(*r); i++)
        ((uint8_t *)r)[i] = 0;
    if (imsi_len > (int)sizeof r->imsi)
        imsi_len = (int)sizeof r->imsi;
    for (int i = 0; i < imsi_len; i++)
        r->imsi[i] = imsi[i];
    r->imsi_len = (uint8_t)imsi_len;
    r->mcc = mcc;
    r->mnc = mnc;
    r->mnc3 = (uint8_t)(mnc3 != 0);
    r->lac = lac;
    r->classmark1 = classmark1;
    r->state = REG_IDLE;
}

int reg_begin(gsm_reg_t *r, uint8_t out[LAPDM_BLOCK])
{
    uint8_t lu[LAPDM_N201];
    int n;

    if (r->state != REG_IDLE)
        return -1;
    n = mm_encode_lu_request(lu, MM_LU_TYPE_NORMAL, 7, r->mcc, r->mnc, r->mnc3,
                             r->lac, r->classmark1, r->imsi, r->imsi_len);
    if (n < 0 || n > LAPDM_N201)
        return -1;
    lapdm_init(&r->dl, 0);
    r->state = REG_WAIT_UA;
    return lapdm_establish(&r->dl, lu, n, out);
}

/* Send an L3 message up to the network as a LAPDm I-frame (overrides any L2 ack
 * that lapdm_receive just produced in out). */
static void send_l3(gsm_reg_t *r, const uint8_t *l3, int len, uint8_t out[LAPDM_BLOCK],
                    int *out_valid)
{
    if (lapdm_send(&r->dl, l3, len, out) == LAPDM_BLOCK)
        *out_valid = 1;
}

int reg_rx(gsm_reg_t *r, const uint8_t in[LAPDM_BLOCK], uint8_t out[LAPDM_BLOCK],
           int *out_valid)
{
    uint8_t l3[LAPDM_N201];
    int l3len = 0, ov = 0, deliver;

    deliver = lapdm_receive(&r->dl, in, l3, &l3len, out, &ov);
    *out_valid = ov;
    if (r->state == REG_WAIT_UA && r->dl.state == LAPDM_ESTABLISHED)
        r->state = REG_RUNNING;
    if (deliver != 1)
        return r->state;

    r->last_pd = (uint8_t)(l3[0] & 0x0f);
    r->last_mtype = l3len > 1 ? l3[1] : 0;
    r->l3_msgs++;

    if ((l3[0] & 0x0f) == MM_PD) {
        switch (mm_msg_type(l3, l3len)) {
        case MM_LU_ACCEPT:
            r->state = REG_ACCEPTED;
            /* a TMSI reallocation (mobile identity IE after the LAI) is confirmed */
            if (l3len > 7 && l3[7] == 0x17) {
                uint8_t c[2];

                send_l3(r, c, mm_encode_tmsi_realloc_complete(c), out, out_valid);
            }
            break;
        case MM_LU_REJECT:
            mm_parse_lu_reject(l3, l3len, &r->reject_cause);
            r->state = REG_REJECTED;
            break;
        case MM_AUTH_REQUEST:
            if (mm_parse_auth_request(l3, l3len, r->rand) == 0)
                r->state = REG_NEED_SRES;
            break;
        case MM_IDENTITY_REQUEST: {
            uint8_t resp[LAPDM_N201];

            send_l3(r, resp, mm_encode_identity_response(resp, MI_TYPE_IMSI, r->imsi,
                    r->imsi_len), out, out_valid);
            break;
        }
        default:
            break;
        }
    }
    return r->state;
}

int reg_sres(gsm_reg_t *r, const uint8_t sres[4], uint8_t out[LAPDM_BLOCK])
{
    uint8_t resp[6];

    if (r->state != REG_NEED_SRES)
        return -1;
    r->state = REG_RUNNING;
    return lapdm_send(&r->dl, resp, mm_encode_auth_response(resp, sres), out);
}

/* ---- self-test: a scripted location-update exchange ---------------------- */

static int net_iframe(uint8_t out[LAPDM_BLOCK], int ns, int nr, const uint8_t *l3, int len)
{
    uint8_t ctrl = (uint8_t)((nr & 7) << 5 | LAPDM_PF | (ns & 7) << 1);

    return lapdm_encode(out, 0, 0, ctrl, l3, len);  /* network command I-frame */
}

int reg_selftest(void)
{
    static const uint8_t imsi[15] = { 0, 0, 1, 0, 1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 };
    static const uint8_t sres[4] = { 0x11, 0x22, 0x33, 0x44 };
    gsm_reg_t r;
    uint8_t f[LAPDM_BLOCK], g[LAPDM_BLOCK];
    uint8_t ar[19] = { 0x05, 0x12, 0x07 };
    static const uint8_t accept[] = { 0x05, 0x02, 0x00, 0xf1, 0x10, 0x12, 0x34 };
    static const uint8_t reject[] = { 0x05, 0x04, 0x0b };
    int ov, ok = 0;

    for (int i = 0; i < 16; i++)
        ar[3 + i] = (uint8_t)(0xa0 + i);

    reg_init(&r, imsi, 15, 1, 1, 0, 0x1234, 0x33);
    /* 1: begin -> SABM carrying the MM LU request */
    reg_begin(&r, f);
    if (f[1] == (LAPDM_U_SABM | LAPDM_PF) && f[3] == MM_PD && f[4] == MM_LU_REQUEST) ok++;
    /* 2: state */
    if (r.state == REG_WAIT_UA) ok++;
    /* 3: UA -> RUNNING */
    lapdm_encode(g, 0, 1, LAPDM_U_UA | LAPDM_PF, 0, 0);
    reg_rx(&r, g, f, &ov);
    if (r.state == REG_RUNNING) ok++;
    /* 4: AUTH REQUEST (network I-frame NS=0) -> NEED_SRES, RAND captured */
    net_iframe(g, 0, 0, ar, 19);
    reg_rx(&r, g, f, &ov);
    if (r.state == REG_NEED_SRES && r.rand[0] == 0xa0 && r.rand[15] == 0xaf) ok++;
    /* 5: SRES -> AUTH RESPONSE I-frame */
    if (reg_sres(&r, sres, f) == LAPDM_BLOCK && f[3] == MM_PD && f[4] == MM_AUTH_RESPONSE &&
        f[5] == 0x11 && f[8] == 0x44) ok++;
    /* 6: back to RUNNING */
    if (r.state == REG_RUNNING) ok++;
    /* 7: LU ACCEPT (NS=1 acking our I-frame, NR=1) -> ACCEPTED */
    net_iframe(g, 1, 1, accept, (int)sizeof accept);
    reg_rx(&r, g, f, &ov);
    if (r.state == REG_ACCEPTED) ok++;
    /* 8: a reject in a fresh session -> REJECTED with the cause */
    reg_init(&r, imsi, 15, 1, 1, 0, 0x1234, 0x33);
    reg_begin(&r, f);
    lapdm_encode(g, 0, 1, LAPDM_U_UA | LAPDM_PF, 0, 0);
    reg_rx(&r, g, f, &ov);
    net_iframe(g, 0, 0, reject, (int)sizeof reject);
    reg_rx(&r, g, f, &ov);
    if (r.state == REG_REJECTED && r.reject_cause == 0x0b) ok++;

    return ok;
}
