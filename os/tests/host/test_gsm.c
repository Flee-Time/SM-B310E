/* Host test: LAPDm and the GSM message code (os/net/lapdm.c, rr.c, mm.c,
 * gsm_reg.c), which their headers call host-unit-testable.
 *
 *   - their own self-tests (lapdm_selftest, rr_selftest, mm_selftest,
 *     reg_selftest) pass every check;
 *   - known frames: a SABM with an L3 payload, a UA, an RR; an IMMEDIATE
 *     ASSIGNMENT decoded field by field (the 04.08 layout);
 *   - random and cut frames and messages: each input sits at the end of a
 *     readable page (a read past it is an access violation), each output in
 *     a guarded block of exactly its size; the decoded fields stay in range
 *     (SAPI 0-7, C/R 0-1, L3 length <= N201, timeslot and TSC 0-7, ARFCN <
 *     1024, TA < 64, MAIO/HSN < 64, link state and V(S)/V(R)/V(A) in range,
 *     the registration state one of the REG_* values), every frame sent is a
 *     well-formed 23-byte LAPDm frame;
 *   - the registration FSM fed random network frames (I-frames carrying
 *     random MM/RR messages of every length, U and S frames, junk), with
 *     random SRES / cipher-complete / keep-alive calls in between.
 *
 * Depends on the OS: the functions in os/lapdm.h, os/gsm_rr.h, os/gsm_mm.h,
 * os/gsm_reg.h (and the structures there).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "host.h"
#include "os/gsm_reg.h"
#include "os/kernel.h"

#ifdef HOST_PROVE
#define TN_MAX 4                        /* a wrong bound: the fuzzing must see it */
#else
#define TN_MAX 8
#endif

static unsigned checked;

static uint8_t rnd8(void)
{
    return (uint8_t)host_rand();
}

/* an output frame: well formed, as the MS sends it */
static void check_out(const uint8_t *f, const char *what)
{
    uint8_t sapi, ctrl, l3[LAPDM_N201];
    int cr, len = -1;

    CHECK(lapdm_decode(f, &sapi, &cr, &ctrl, l3, &len) == 0, "%s: a frame we sent does not decode",
          what);
    CHECK((f[0] & 0xe1) == 0x01, "%s: address octet %02x", what, f[0]);
    CHECK(len >= 0 && len <= LAPDM_N201 && (f[2] & 3) == 1, "%s: length octet %02x", what, f[2]);
    for (int i = 3 + (len > 0 ? len : 0); i < LAPDM_BLOCK; i++)
        if (f[i] != LAPDM_FILL) {
            CHECK(0, "%s: fill octet %d is %02x", what, i, f[i]);
            break;
        }
}

static void check_dl(const lapdm_dl_t *dl, const char *what)
{
    CHECK(dl->state <= LAPDM_RELEASING && dl->vs < 8 && dl->vr < 8 && dl->va < 8 &&
          dl->tx_len <= LAPDM_N201 && dl->tx_pending <= 1 && dl->rx_seg <= 1 && dl->sapi < 8,
          "%s: link state %u vs %u vr %u va %u tx_len %u pending %u seg %u", what, dl->state,
          dl->vs, dl->vr, dl->va, dl->tx_len, dl->tx_pending, dl->rx_seg);
}

static void known_vectors(void)
{
    static const uint8_t l3[] = { 0x05, 0x08, 0x70 };
    uint8_t f[LAPDM_BLOCK], sapi, ctrl, body[LAPDM_N201];
    int cr, len;

    host_context("known frames");
    CHECK(lapdm_selftest() == LAPDM_SELFTEST_TOTAL, "lapdm_selftest %d of %d", lapdm_selftest(),
          LAPDM_SELFTEST_TOTAL);
    CHECK(rr_selftest() == RR_SELFTEST_TOTAL, "rr_selftest %d of %d", rr_selftest(),
          RR_SELFTEST_TOTAL);
    CHECK(mm_selftest() == MM_SELFTEST_TOTAL, "mm_selftest %d of %d", mm_selftest(),
          MM_SELFTEST_TOTAL);
    CHECK(reg_selftest() == REG_SELFTEST_TOTAL, "reg_selftest %d of %d", reg_selftest(),
          REG_SELFTEST_TOTAL);
    /* SABM, SAPI 0, command (C/R 0 from the MS), P=1, carrying 3 octets (04.06) */
    CHECK(lapdm_encode(f, 0, 0, LAPDM_U_SABM | LAPDM_PF, l3, 3) == LAPDM_BLOCK &&
          f[0] == 0x01 && f[1] == 0x3f && f[2] == 0x0d && !memcmp(f + 3, l3, 3) && f[6] == 0x2b &&
          f[22] == 0x2b, "SABM: %02x %02x %02x", f[0], f[1], f[2]);
    CHECK(lapdm_encode(f, 3, 1, LAPDM_U_UA | LAPDM_PF, NULL, 0) == LAPDM_BLOCK &&
          f[0] == 0x0f && f[1] == 0x73 && f[2] == 0x01, "UA SAPI 3: %02x %02x %02x", f[0], f[1],
          f[2]);
    CHECK(lapdm_encode(f, 0, 0, 0, l3, LAPDM_N201 + 1) == -1, "a payload over N201 is refused");
    lapdm_encode(f, 0, 1, (uint8_t)(5 << 5 | LAPDM_S_RR), NULL, 0);
    CHECK(lapdm_decode(f, &sapi, &cr, &ctrl, body, &len) == 0 && sapi == 0 && cr == 1 &&
          LAPDM_CTRL_IS_S(ctrl) && LAPDM_S_NR(ctrl) == 5 && len == 0, "RR N(R)=5 decodes");
    {
        /* IMMEDIATE ASSIGNMENT: SDCCH/8 subchannel 2 on TN 1, TSC 5, ARFCN 54,
         * request reference RA 0x3f, T1' 3 T3 17 T2 9, TA 7 */
        uint8_t ia[23] = { 0x2d, 0x06, 0x3f, 0x00, 0x51, 0xa0, 0x36, 0x3f, 0x1a, 0x29, 0x07,
                           0x00 };
        rr_chan_desc_t d;

        for (int i = 12; i < 23; i++)
            ia[i] = 0x2b;
        CHECK(rr_msg_type(ia) == RR_IMM_ASSIGN, "the message type");
        CHECK(rr_parse_imm_assign(ia, &d) == 0 && d.chan_type == RR_CH_SDCCH8 && d.subch == 2 &&
              d.tn == 1 && d.tsc == 5 && d.arfcn == 54 && !d.hopping && d.ra == 0x3f &&
              d.ta == 7 && d.ref_fn == (3 << 11 | 17 << 5 | 9),
              "IMMEDIATE ASSIGNMENT: type %u subch %u tn %u tsc %u arfcn %u hop %u ra %02x ta %u "
              "ref %04x", d.chan_type, d.subch, d.tn, d.tsc, d.arfcn, d.hopping, d.ra, d.ta,
              d.ref_fn);
        ia[1] = 0x05;
        CHECK(rr_parse_imm_assign(ia, &d) == -1, "not RR: refused");
    }
    {
        /* LOCATION UPDATING REQUEST, IMSI 001010123456789, MCC 001 MNC 01 LAC 0x1234 */
        static const uint8_t imsi[15] = { 0, 0, 1, 0, 1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 };
        static const uint8_t want[] = { 0x05, 0x08, 0x70, 0x00, 0xf1, 0x10, 0x12, 0x34, 0x33,
                                        0x08, 0x09, 0x10, 0x10, 0x10, 0x32, 0x54, 0x76, 0x98 };
        uint8_t out[24];
        int n = mm_encode_lu_request(out, MM_LU_TYPE_NORMAL, 7, 1, 1, 0, 0x1234, 0x33, imsi, 15);

        CHECK(n == (int)sizeof(want) && !memcmp(out, want, sizeof(want)),
              "LU REQUEST: %d bytes, %02x %02x %02x .. %02x", n, out[0], out[1], out[2],
              out[n > 0 ? n - 1 : 0]);
    }
}

static void fuzz_codec(unsigned rounds)
{
    for (unsigned r = 0; r < rounds; r++) {
        uint8_t *in = host_edge(LAPDM_BLOCK), sapi = 0xee, ctrl;
        uint8_t *l3 = kmalloc(LAPDM_N201);
        int cr = -1, len = -1, rc;
        rr_chan_desc_t d;

        for (int i = 0; i < LAPDM_BLOCK; i++)
            in[i] = rnd8();
        host_dump(in, LAPDM_BLOCK);
        host_context("lapdm_decode / rr_parse_imm_assign, random frame %u", r);
        rc = lapdm_decode(in, &sapi, &cr, &ctrl, l3, &len);
        if (rc == 0)
            CHECK(sapi < 8 && (cr == 0 || cr == 1) && len >= 0 && len <= LAPDM_N201,
                  "decoded sapi %u cr %d len %d", sapi, cr, len);
        else
            CHECK(rc == -1 && ((in[2] >> 2) & 0x3f) > LAPDM_N201, "refused a frame of length %d",
                  (in[2] >> 2) & 0x3f);
        kfree(l3);
        if (r & 1)
            in[1] = 0x06, in[2] = RR_IMM_ASSIGN;
        memset(&d, 0xee, sizeof(d));
        if (rr_parse_imm_assign(in, &d) == 0)
            CHECK(d.chan_type <= RR_CH_UNKNOWN && d.tn < TN_MAX && d.tsc < 8 && d.arfcn < 1024 &&
                  d.ta < 64 && d.maio < 64 && d.hsn < 64 && d.hopping <= 1 &&
                  (d.chan_type != RR_CH_SDCCH4 || d.subch < 4) && d.subch < 8,
                  "IMMEDIATE ASSIGNMENT fields: type %u subch %u tn %u tsc %u arfcn %u ta %u maio "
                  "%u hsn %u", d.chan_type, d.subch, d.tn, d.tsc, d.arfcn, d.ta, d.maio, d.hsn);
        rr_msg_type(in);
        /* MM parsers on every length, the message at the page's end */
        for (int n = 0; n <= 23; n++) {
            uint8_t *m = host_edge((size_t)n ? (size_t)n : 1), rand_[16], cause;

            for (int i = 0; i < n; i++)
                m[i] = rnd8();
            if (n >= 2 && (r & 3)) {
                m[0] = (uint8_t)((m[0] & 0xf0) | MM_PD);
                m[1] = (uint8_t)((m[1] & 0xc0) | ((r >> 2) & 1 ? MM_AUTH_REQUEST : MM_LU_REJECT));
            }
            host_context("mm parsers, %d random bytes (round %u)", n, r);
            host_dump(m, (size_t)n);
            rc = mm_msg_type(m, n);
            CHECK(rc >= -1 && rc < 64, "mm_msg_type %d", rc);
            mm_parse_lu_reject(m, n, &cause);
            mm_parse_auth_request(m, n, rand_);
        }
        checked++;
        if (host_failures > 10)
            return;
    }
    host_dump(NULL, 0);
}

/* a random frame from the network to the registration FSM */
static void net_frame(uint8_t *f, unsigned r)
{
    uint8_t l3[LAPDM_N201];
    int len = (int)(host_rand() % (LAPDM_N201 + 1)), kind = (int)(host_rand() % 8);

    for (int i = 0; i < len; i++)
        l3[i] = rnd8();
    if (len >= 2 && host_rand() % 4) {
        static const uint8_t mm[] = { MM_LU_ACCEPT, MM_LU_REJECT, MM_AUTH_REQUEST, MM_AUTH_REJECT,
                                      MM_IDENTITY_REQUEST, MM_TMSI_REALLOC_CMD, 0x3f };
        static const uint8_t rr[] = { RR_CIPH_MODE_CMD, RR_CLASSMARK_ENQUIRY, RR_CHAN_RELEASE,
                                      RR_IMM_ASSIGN };

        if (host_rand() & 1) {
            l3[0] = MM_PD;
            l3[1] = mm[host_rand() % sizeof(mm)];
        } else {
            l3[0] = RR_PD;
            l3[1] = rr[host_rand() % sizeof(rr)];
        }
    }
    switch (kind) {
    case 0:                             /* junk */
        for (int i = 0; i < LAPDM_BLOCK; i++)
            f[i] = rnd8();
        break;
    case 1:
        lapdm_encode(f, 0, 1, LAPDM_U_UA | LAPDM_PF, l3, len);
        break;
    case 2: {
        static const uint8_t u[] = { LAPDM_U_DM, LAPDM_U_DISC, LAPDM_U_UI, LAPDM_U_SABM };

        lapdm_encode(f, 0, 0, u[host_rand() % 4] | (host_rand() & 1 ? LAPDM_PF : 0), NULL, 0);
        break;
    }
    case 3:
        lapdm_encode(f, 0, 1, (uint8_t)((host_rand() & 7) << 5 | LAPDM_S_RR), NULL, 0);
        break;
    default: {                          /* an I-frame, N(S) right most of the time */
        uint8_t ctrl = (uint8_t)((host_rand() & 7) << 5 | LAPDM_PF | (host_rand() & 7) << 1);

        lapdm_encode(f, (uint8_t)(host_rand() % 8 ? 0 : 3), 0, ctrl, l3, len);
        if (host_rand() % 6 == 0)
            f[2] |= 2;                  /* the M bit */
        if (host_rand() % 8 == 0)
            f[2] = rnd8();              /* any length octet */
        break;
    }
    }
    (void)r;
}

static void fuzz_reg(unsigned runs)
{
    static const uint8_t imsi[16] = { 2, 8, 6, 0, 1, 1, 2, 3, 4, 5, 6, 7, 8, 9, 0, 1 };
    static const uint8_t imei[16] = { 3, 5, 6, 9, 3, 8, 0, 3, 5, 6, 4, 3, 8, 0, 9, 9 };

    for (unsigned run = 0; run < runs; run++) {
        gsm_reg_t *g = kmalloc(sizeof(*g));
        uint8_t *out = kmalloc(LAPDM_BLOCK);
        int steps = 10 + (int)(host_rand() % 60), ov;

        reg_init(g, imsi, (int)(host_rand() % 17), 286, 1, 0, (uint16_t)host_rand(), 0x33);
        if (host_rand() & 1)
            reg_set_imei(g, imei, (int)(host_rand() % 17));
        if (host_rand() % 3 == 0)
            reg_set_tmsi(g, host_rand());
        host_context("registration run %u: reg_begin", run);
        CHECK(reg_begin(g, out) == LAPDM_BLOCK, "reg_begin");
        check_out(out, "the SABM");
        for (int s = 0; s < steps; s++) {
            uint8_t *in = host_edge(LAPDM_BLOCK);
            int st;

            net_frame(in, run);
            host_dump(in, LAPDM_BLOCK);
            host_context("registration run %u, step %d, state %u", run, s, g->state);
            memset(out, 0x2b, LAPDM_BLOCK);
            ov = -1;
            st = reg_rx(g, in, out, &ov);
            CHECK(st == g->state && st <= REG_AUTH_REJECTED && (ov == 0 || ov == 1),
                  "reg_rx: state %d, out_valid %d", st, ov);
            if (ov == 1)
                check_out(out, "a reply");
            check_dl(&g->dl, "after reg_rx");
            switch (host_rand() % 6) {
            case 0: {
                static const uint8_t sres[4] = { 1, 2, 3, 4 };
                int n = reg_sres(g, sres, out);

                if (n == LAPDM_BLOCK)
                    check_out(out, "the AUTHENTICATION RESPONSE");
                break;
            }
            case 1:
                if (reg_cipher_complete(g, out) == LAPDM_BLOCK)
                    check_out(out, "the CIPHERING MODE COMPLETE");
                break;
            case 2:
                if (reg_keepalive(g, out) == LAPDM_BLOCK)
                    check_out(out, "a keep-alive");
                break;
            case 3:
                if (reg_classmark_change(g, out) == LAPDM_BLOCK)
                    check_out(out, "the CLASSMARK CHANGE");
                break;
            default:
                break;
            }
            check_dl(&g->dl, "after a call");
            CHECK(g->state <= REG_AUTH_REJECTED && g->imsi_len <= 16 && g->imei_len <= 16,
                  "state %u imsi_len %u imei_len %u", g->state, g->imsi_len, g->imei_len);
            if (host_failures > 10)
                break;
        }
        kfree(out);                     /* the guards: nothing written past 23 bytes */
        kfree(g);
        checked++;
        if (host_failures > 10)
            return;
    }
    host_dump(NULL, 0);
}

int main(int argc, char **argv)
{
    host_init(argc, argv);
    host_srand(0x67736d31u);
    known_vectors();
    fuzz_codec(host_long ? 200000 : 20000);
    fuzz_reg(host_long ? 50000 : 4000);
    printf("gsm: %u random frames and registration runs\n", checked);
    return host_done();
}
