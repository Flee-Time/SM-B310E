/* B310E OS - LAPDm (GSM 04.06), the data link over SDCCH/SACCH toward an
 * OsmocomBB-style client stack. MS side: establish with SABM (contention
 * resolution carries the first L3 message), transfer L3 as I-frames with
 * V(S)/V(R)/V(A) sequencing and RR acks, release with DISC/UA.
 *
 * The core (frame codec + datalink state) is self-contained stdint logic with no
 * OS dependencies, so it unit-tests on the host. RR/MM ride on top of it.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef OS_LAPDM_H
#define OS_LAPDM_H

#include <stdint.h>

#define LAPDM_BLOCK     23              /* an SDCCH block is one LAPDm frame */
#define LAPDM_N201      20              /* max L3 payload on SDCCH (SAPI 0) */
#define LAPDM_FILL      0x2bu           /* idle/fill octet */

/* Address octet: spare(b8)=0 | LPD(b7-6)=00 | SAPI(b5-3) | C/R(b2) | EA(b1)=1. */
#define LAPDM_ADDR(sapi, cr)    (uint8_t)(((sapi) & 7) << 2 | ((cr) & 1) << 1 | 1u)
#define LAPDM_ADDR_SAPI(a)      (((a) >> 2) & 7)
#define LAPDM_ADDR_CR(a)        (((a) >> 1) & 1)

/* Control octet. U: MMM|P/F|MM|11. S: N(R)|P/F|SS|01. I: N(R)|P|N(S)|0. */
#define LAPDM_U_UI      0x03u
#define LAPDM_U_DM      0x0fu
#define LAPDM_U_DISC    0x43u
#define LAPDM_U_UA      0x63u
#define LAPDM_U_SABM    0x2fu
#define LAPDM_S_RR      0x01u
#define LAPDM_S_RNR     0x05u
#define LAPDM_S_REJ     0x09u
#define LAPDM_PF        0x10u           /* poll/final bit (b5) */

#define LAPDM_CTRL_IS_U(c)      (((c) & 3) == 3)
#define LAPDM_CTRL_IS_S(c)      (((c) & 3) == 1)
#define LAPDM_CTRL_IS_I(c)      (((c) & 1) == 0)
#define LAPDM_I_NS(c)           (((c) >> 1) & 7)
#define LAPDM_I_NR(c)           (((c) >> 5) & 7)
#define LAPDM_S_NR(c)           (((c) >> 5) & 7)
#define LAPDM_U_BITS(c)         ((c) & ~LAPDM_PF)       /* the frame type, P/F off */

enum { LAPDM_IDLE, LAPDM_ESTABLISHING, LAPDM_ESTABLISHED, LAPDM_RELEASING };

typedef struct {
    uint8_t sapi;
    uint8_t state;
    uint8_t vs, vr, va;                 /* V(S), V(R), V(A), mod 8 */
    uint8_t tx[LAPDM_N201];             /* the last I-frame payload, for resend */
    uint8_t tx_len;
    uint8_t tx_pending;                 /* an unacked I-frame is outstanding */
} lapdm_dl_t;

/* ---- frame codec (pure) ---- */
/* Build a 23-byte SDCCH frame; returns LAPDM_BLOCK. l3len <= LAPDM_N201. */
int lapdm_encode(uint8_t out[LAPDM_BLOCK], uint8_t sapi, int cr, uint8_t ctrl,
                 const uint8_t *l3, int l3len);
/* Decode a 23-byte frame. Fills sapi/cr/ctrl and copies the L3 payload to l3
 * (l3len set). Returns 0, or -1 on a malformed frame. */
int lapdm_decode(const uint8_t in[LAPDM_BLOCK], uint8_t *sapi, int *cr,
                 uint8_t *ctrl, uint8_t *l3, int *l3len);

/* ---- MS-side datalink ---- */
void lapdm_init(lapdm_dl_t *dl, uint8_t sapi);
/* Start establishment: SABM carrying the first L3 message (contention
 * resolution). Writes the frame to out; returns LAPDM_BLOCK or -1. */
int lapdm_establish(lapdm_dl_t *dl, const uint8_t *l3, int len, uint8_t out[LAPDM_BLOCK]);
/* Queue+encode an L3 message as an I-frame (only when ESTABLISHED and no I-frame
 * is outstanding). Returns LAPDM_BLOCK, 0 if it must wait, or -1. */
int lapdm_send(lapdm_dl_t *dl, const uint8_t *l3, int len, uint8_t out[LAPDM_BLOCK]);
/* Feed a received frame. If it carries L3 to deliver up, copies it to l3/l3len.
 * If a frame must be sent in reply (UA ack, RR), writes it to out and sets
 * *out_valid. Returns 1 if l3 was delivered, 0 otherwise, -1 on error. */
int lapdm_receive(lapdm_dl_t *dl, const uint8_t in[LAPDM_BLOCK], uint8_t *l3,
                  int *l3len, uint8_t out[LAPDM_BLOCK], int *out_valid);
/* Build an idle RR keep-alive/ack frame (receiver ready, N(R)=V(R), P=0) to hold
 * the SDCCH uplink active between L3 messages, the way the stock MS does through
 * the whole auth/cipher/accept exchange. Returns LAPDM_BLOCK, or 0 if the link is
 * not established. Touches no sequence state, so it is safe to send any time. */
int lapdm_fill(lapdm_dl_t *dl, uint8_t out[LAPDM_BLOCK]);

/* Self-test: returns the number of checks passed (LAPDM_SELFTEST_TOTAL on all). */
#define LAPDM_SELFTEST_TOTAL 13
int lapdm_selftest(void);

#endif
