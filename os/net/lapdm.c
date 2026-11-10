/* B310E OS - LAPDm (GSM 04.06) data link, MS side. See os/lapdm.h.
 * Self-contained stdint logic (no OS deps) so it unit-tests on the host.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/lapdm.h"

static void bzero_n(uint8_t *p, int n)
{
    for (int i = 0; i < n; i++)
        p[i] = 0;
}

static void copy_n(uint8_t *d, const uint8_t *s, int n)
{
    for (int i = 0; i < n; i++)
        d[i] = s[i];
}

/* ---- frame codec ---------------------------------------------------------- */

int lapdm_encode(uint8_t out[LAPDM_BLOCK], uint8_t sapi, int cr, uint8_t ctrl,
                 const uint8_t *l3, int l3len)
{
    if (l3len < 0 || l3len > LAPDM_N201)
        return -1;
    out[0] = LAPDM_ADDR(sapi, cr);
    out[1] = ctrl;
    out[2] = (uint8_t)((l3len & 0x3f) << 2 | 1u);       /* length | M=0 | EL=1 */
    for (int i = 0; i < l3len; i++)
        out[3 + i] = l3 ? l3[i] : 0;
    for (int i = 3 + l3len; i < LAPDM_BLOCK; i++)
        out[i] = LAPDM_FILL;
    return LAPDM_BLOCK;
}

int lapdm_decode(const uint8_t in[LAPDM_BLOCK], uint8_t *sapi, int *cr,
                 uint8_t *ctrl, uint8_t *l3, int *l3len)
{
    int len = (in[2] >> 2) & 0x3f;

    if (3 + len > LAPDM_BLOCK)
        return -1;
    if (sapi)
        *sapi = (uint8_t)LAPDM_ADDR_SAPI(in[0]);
    if (cr)
        *cr = LAPDM_ADDR_CR(in[0]);
    if (ctrl)
        *ctrl = in[1];
    if (l3 && l3len) {
        copy_n(l3, in + 3, len);
        *l3len = len;
    }
    return 0;
}

/* ---- MS-side datalink ------------------------------------------------------ */

void lapdm_init(lapdm_dl_t *dl, uint8_t sapi)
{
    bzero_n((uint8_t *)dl, (int)sizeof(*dl));
    dl->sapi = sapi;
    dl->state = LAPDM_IDLE;
}

int lapdm_establish(lapdm_dl_t *dl, const uint8_t *l3, int len, uint8_t out[LAPDM_BLOCK])
{
    if (dl->state != LAPDM_IDLE || len < 0 || len > LAPDM_N201)
        return -1;
    dl->vs = dl->vr = dl->va = 0;
    dl->tx_pending = 0;
    dl->state = LAPDM_ESTABLISHING;
    /* SABM is a command (MS C/R=0) with P=1; its payload is the first L3 message
     * (contention resolution). */
    return lapdm_encode(out, dl->sapi, 0, LAPDM_U_SABM | LAPDM_PF, l3, len);
}

int lapdm_send(lapdm_dl_t *dl, const uint8_t *l3, int len, uint8_t out[LAPDM_BLOCK])
{
    uint8_t ctrl;

    if (dl->state != LAPDM_ESTABLISHED || len < 0 || len > LAPDM_N201)
        return -1;
    if (dl->tx_pending)
        return 0;                       /* wait for the ack of the last I-frame */
    /* I: N(R) | P | N(S) | 0 ; command, P=1 to poll for the ack */
    ctrl = (uint8_t)((dl->vr & 7) << 5 | LAPDM_PF | (dl->vs & 7) << 1);
    copy_n(dl->tx, l3, len);
    dl->tx_len = (uint8_t)len;
    dl->tx_pending = 1;
    dl->vs = (uint8_t)((dl->vs + 1) & 7);
    return lapdm_encode(out, dl->sapi, 0, ctrl, l3, len);
}

/* Acknowledge our outstanding I-frame if n_r has advanced past V(A). */
static void ack_upto(lapdm_dl_t *dl, uint8_t n_r)
{
    dl->va = (uint8_t)(n_r & 7);
    if (dl->va == dl->vs)
        dl->tx_pending = 0;
}

int lapdm_receive(lapdm_dl_t *dl, const uint8_t in[LAPDM_BLOCK], uint8_t *l3,
                  int *l3len, uint8_t out[LAPDM_BLOCK], int *out_valid)
{
    uint8_t ctrl, body[LAPDM_N201];
    int cr, blen = 0, deliver = 0;

    if (out_valid)
        *out_valid = 0;
    if (lapdm_decode(in, 0, &cr, &ctrl, body, &blen) < 0)
        return -1;

    if (LAPDM_CTRL_IS_U(ctrl)) {
        switch (LAPDM_U_BITS(ctrl)) {
        case LAPDM_U_UA:                /* establish confirm (echoes the SABM L3) */
            if (dl->state == LAPDM_ESTABLISHING)
                dl->state = LAPDM_ESTABLISHED;
            else if (dl->state == LAPDM_RELEASING)
                dl->state = LAPDM_IDLE;
            break;
        case LAPDM_U_DM:                /* link refused / released */
            dl->state = LAPDM_IDLE;
            break;
        case LAPDM_U_DISC:              /* network releases: answer UA (response) */
            dl->state = LAPDM_IDLE;
            if (out_valid) {
                lapdm_encode(out, dl->sapi, 1, LAPDM_U_UA | LAPDM_PF, 0, 0);
                *out_valid = 1;
            }
            break;
        default:
            break;
        }
    } else if (LAPDM_CTRL_IS_S(ctrl)) { /* RR/RNR/REJ: N(R) acks our I-frame */
        ack_upto(dl, (uint8_t)LAPDM_S_NR(ctrl));
    } else {                            /* I-frame: L3 data from the network */
        /* The network only sends I-frames once it considers the link up, so an
         * I-frame confirms establishment even if our UA was lost to a bad CRC.
         * Without this a dropped UA left us in ESTABLISHING, where lapdm_send
         * refuses to send - so the AUTHENTICATION RESPONSE never went out (est=0
         * with auth seen, ul stuck at SABM + the RR ack) and the network never
         * moved on to CIPHERING MODE COMMAND / ACCEPT. */
        if (dl->state == LAPDM_ESTABLISHING)
            dl->state = LAPDM_ESTABLISHED;
        ack_upto(dl, (uint8_t)LAPDM_I_NR(ctrl));
        if (LAPDM_I_NS(ctrl) == dl->vr) {
            dl->vr = (uint8_t)((dl->vr + 1) & 7);
            if (l3 && l3len) {
                copy_n(l3, body, blen);
                *l3len = blen;
                deliver = 1;
            }
        }
        /* acknowledge with RR (response, F=1, N(R)=V(R)) */
        if (out_valid) {
            uint8_t rr = (uint8_t)((dl->vr & 7) << 5 | LAPDM_PF | LAPDM_S_RR);
            lapdm_encode(out, dl->sapi, 1, rr, 0, 0);
            *out_valid = 1;
        }
    }
    return deliver;
}

/* ---- self-test ------------------------------------------------------------ */

int lapdm_selftest(void)
{
    int ok = 0;
    uint8_t f[LAPDM_BLOCK], g[LAPDM_BLOCK], body[LAPDM_N201];
    uint8_t sapi;
    int cr, len;
    uint8_t ctrl;
    lapdm_dl_t dl;
    static const uint8_t req[] = { 0x05, 0x08, 0x70 };  /* a toy L3 message */

    /* 1: address octet for SAPI 0, command */
    if (LAPDM_ADDR(0, 0) == 0x01) ok++;
    /* 2: address octet for SAPI 3, response */
    if (LAPDM_ADDR(3, 1) == 0x0f) ok++;
    /* 3: encode/decode round-trip carries the payload */
    lapdm_encode(f, 0, 0, LAPDM_U_SABM | LAPDM_PF, req, (int)sizeof req);
    if (lapdm_decode(f, &sapi, &cr, &ctrl, body, &len) == 0 &&
        len == (int)sizeof req && body[0] == 0x05 && body[2] == 0x70) ok++;
    /* 4: length octet = len<<2 | 1 */
    if (f[2] == (uint8_t)((sizeof req) << 2 | 1)) ok++;
    /* 5: fill padding after the payload */
    if (f[3 + sizeof req] == LAPDM_FILL && f[LAPDM_BLOCK - 1] == LAPDM_FILL) ok++;
    /* 6: establish produces a SABM|P command with the payload */
    lapdm_init(&dl, 0);
    lapdm_establish(&dl, req, (int)sizeof req, f);
    if (f[1] == (LAPDM_U_SABM | LAPDM_PF) && dl.state == LAPDM_ESTABLISHING) ok++;
    /* 7: receiving UA confirms establishment */
    lapdm_encode(g, 0, 1, LAPDM_U_UA | LAPDM_PF, 0, 0);
    if (lapdm_receive(&dl, g, body, &len, f, &cr) == 0 && dl.state == LAPDM_ESTABLISHED) ok++;
    /* 8: send an I-frame: N(S)=0, V(S) advances, pending set */
    if (lapdm_send(&dl, req, (int)sizeof req, f) == LAPDM_BLOCK &&
        LAPDM_CTRL_IS_I(f[1]) && LAPDM_I_NS(f[1]) == 0 && dl.vs == 1 && dl.tx_pending) ok++;
    /* 9: a second send waits until the first is acked */
    if (lapdm_send(&dl, req, (int)sizeof req, g) == 0) ok++;
    /* 10: RR with N(R)=1 acks it, clearing pending */
    lapdm_encode(g, 0, 1, (uint8_t)(1 << 5 | LAPDM_PF | LAPDM_S_RR), 0, 0);
    lapdm_receive(&dl, g, body, &len, f, &cr);
    if (!dl.tx_pending && dl.va == 1) ok++;
    /* 11: a network I-frame (N(S)=0) is delivered, V(R) advances, RR replied */
    lapdm_encode(g, 0, 0, (uint8_t)(0 << 5 | LAPDM_PF | (0 << 1)), req, (int)sizeof req);
    if (lapdm_receive(&dl, g, body, &len, f, &cr) == 1 && dl.vr == 1 && cr &&
        LAPDM_CTRL_IS_S(f[1]) && LAPDM_S_NR(f[1]) == 1) ok++;
    /* 12: network DISC is answered with UA and drops to IDLE */
    lapdm_encode(g, 0, 0, LAPDM_U_DISC | LAPDM_PF, 0, 0);
    lapdm_receive(&dl, g, body, &len, f, &cr);
    if (dl.state == LAPDM_IDLE && cr && LAPDM_U_BITS(f[1]) == LAPDM_U_UA) ok++;

    return ok;
}
