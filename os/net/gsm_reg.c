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

void reg_set_imei(gsm_reg_t *r, const uint8_t *digits, int n)
{
    if (n < 0)
        n = 0;
    if (n > (int)sizeof r->imei)
        n = (int)sizeof r->imei;
    for (int i = 0; i < n; i++)
        r->imei[i] = (uint8_t)(digits[i] & 0x0f);
    r->imei_len = (uint8_t)n;
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
            int type = l3len > 2 ? (l3[2] & 0x07) : MI_TYPE_IMSI;

            /* Answer with the identity the network asked for. Before, we always
             * sent the IMSI - so an IMEI request (an EIR / equipment check, common
             * right after the first LU from an IMSI) got the wrong identity and the
             * exchange stalled. IMEISV = the IMEI's first 14 digits + a 2-digit SVN
             * (00, we do not track it). Falls back to the IMSI when no IMEI is set. */
            if ((type == MI_TYPE_IMEI || type == MI_TYPE_IMEISV) && r->imei_len >= 14) {
                if (type == MI_TYPE_IMEISV) {
                    uint8_t sv[16];

                    for (int i = 0; i < 14; i++)
                        sv[i] = r->imei[i];
                    sv[14] = 0;
                    sv[15] = 0;                 /* SVN 00 */
                    send_l3(r, resp, mm_encode_identity_response(resp, MI_TYPE_IMEISV,
                            sv, 16), out, out_valid);
                } else {
                    send_l3(r, resp, mm_encode_identity_response(resp, MI_TYPE_IMEI,
                            r->imei, r->imei_len), out, out_valid);
                }
            } else {
                send_l3(r, resp, mm_encode_identity_response(resp, MI_TYPE_IMSI, r->imsi,
                        r->imsi_len), out, out_valid);
            }
            break;
        }
        default:
            break;
        }
    } else if ((l3[0] & 0x0f) == RR_PD) {
        /* CIPHERING MODE COMMAND: record the algorithm and start flag; the caller
         * enables A5 on the DSP (with Kc) and answers with CIPHERING MODE COMPLETE
         * (reg_cipher_complete), which must go out already enciphered. */
        if (l3[1] == RR_CIPH_MODE_CMD && l3len >= 3) {
            r->ciph_sc = (uint8_t)(l3[2] & 1);
            r->ciph_alg = (uint8_t)((l3[2] >> 1) & 7);
            r->state = REG_NEED_CIPHER;
        } else if (l3[1] == RR_CLASSMARK_ENQUIRY) {
            /* The network asked for our classmark rather than honouring the early
             * one (early classmark sending is cell-controlled via SI3). Answer with
             * CLASSMARK CHANGE (A5/1 only) so it ciphers with A5/1 - the only
             * algorithm this DSP does. Without this answer TT never learns we lack
             * A5/3 and commands A5/3, which we cannot complete. */
            uint8_t msg[6];
            send_l3(r, msg, rr_encode_classmark_change(msg, r->classmark1), out, out_valid);
        }
    }
    return r->state;
}

/* After the caller has enabled ciphering on the DSP, build CIPHERING MODE
 * COMPLETE as an I-frame on the SDCCH. 0 when not awaiting it. */
int reg_cipher_complete(gsm_reg_t *r, uint8_t out[LAPDM_BLOCK])
{
    uint8_t resp[2];
    int n;

    if (r->state != REG_NEED_CIPHER)
        return 0;
    /* The CIPHERING MODE COMMAND we are answering acknowledges our AUTHENTICATION
     * RESPONSE at L3, so clear any stale L2 tx-pending - on this lossy link the N(R)
     * ack can be lost or the sequence desynced (seen: va ahead of vs), and the
     * COMPLETE is a mandatory response that must go out. Advance the state only once
     * the send succeeds, so a window-blocked attempt retries instead of vanishing. */
    r->dl.tx_pending = 0;
    n = lapdm_send(&r->dl, resp, rr_encode_ciph_mode_complete(resp), out);
    if (n > 0)
        r->state = REG_RUNNING;
    return n;
}

/* Early CLASSMARK CHANGE: advertise A5/1 only so the network ciphers with A5/1
 * (the algorithm our DSP does), before it sends CIPHERING MODE COMMAND. Sent once
 * the link is up; returns the block length, or 0 if not established yet. */
int reg_classmark_change(gsm_reg_t *r, uint8_t out[LAPDM_BLOCK])
{
    uint8_t msg[6];

    if (r->dl.state != LAPDM_ESTABLISHED)
        return 0;
    r->dl.tx_pending = 0;               /* mandatory early send; don't let a stale ack block it */
    return lapdm_send(&r->dl, msg, rr_encode_classmark_change(msg, r->classmark1), out);
}

/* Idle keep-alive: an RR frame (N(R)=V(R)) that holds the SDCCH uplink active
 * between L3 steps, the way the stock MS does through the whole exchange. The
 * caller sends it on every uplink opportunity it would otherwise leave empty.
 * Returns the block length, or 0 when the link is not established. */
int reg_keepalive(gsm_reg_t *r, uint8_t out[LAPDM_BLOCK])
{
    return lapdm_fill(&r->dl, out);
}

int reg_sres(gsm_reg_t *r, const uint8_t sres[4], uint8_t out[LAPDM_BLOCK])
{
    uint8_t resp[6];
    int n;

    if (r->state != REG_NEED_SRES)
        return -1;
    r->dl.tx_pending = 0;               /* mandatory response; see reg_cipher_complete */
    n = lapdm_send(&r->dl, resp, mm_encode_auth_response(resp, sres), out);
    if (n > 0)
        r->state = REG_RUNNING;
    return n;
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

    /* 9-10: IDENTITY REQUEST is answered by the requested identity type */
    {
        static const uint8_t idreq_imei[3] = { 0x05, MM_IDENTITY_REQUEST, MI_TYPE_IMEI };
        static const uint8_t idreq_imsi[3] = { 0x05, MM_IDENTITY_REQUEST, MI_TYPE_IMSI };
        static const uint8_t imei[15] = { 3, 5, 6, 9, 3, 8, 0, 3, 5, 6, 4, 3, 8, 0, 9 };

        reg_init(&r, imsi, 15, 1, 1, 0, 0x1234, 0x33);
        reg_set_imei(&r, imei, 15);
        reg_begin(&r, f);
        lapdm_encode(g, 0, 1, LAPDM_U_UA | LAPDM_PF, 0, 0);
        reg_rx(&r, g, f, &ov);                           /* -> ESTABLISHED */
        net_iframe(g, 0, 0, idreq_imei, 3);
        reg_rx(&r, g, f, &ov);
        if (ov && f[3] == MM_PD && f[4] == MM_IDENTITY_RESPONSE &&
            (f[6] & 0x07) == MI_TYPE_IMEI && (f[6] >> 4) == 3) ok++;
        r.dl.tx_pending = 0;                             /* let the next response go */
        net_iframe(g, 1, 1, idreq_imsi, 3);
        reg_rx(&r, g, f, &ov);
        if (ov && f[4] == MM_IDENTITY_RESPONSE && (f[6] & 0x07) == MI_TYPE_IMSI) ok++;
    }

    return ok;
}
