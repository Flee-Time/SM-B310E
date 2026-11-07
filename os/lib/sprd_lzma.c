/* B310E OS — decoder for the SC6530C's LZMA1 variant (os/sprd_lzma.h).
 *
 * A port of the project's QEMU model of the SC6530C LZMA engine
 * (emulator/qemu/machine/hw/misc/sc6530_lzma.c), which implements the
 * LZMA1 state machine described by the public-domain LZMA SDK with the
 * stock stream's differences: plain literals after matches. No vendor
 * code is included.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/kernel.h"
#include "os/sprd_lzma.h"

enum {
    P_MATCH = 0, P_REP = 192, P_REP_G0 = 204, P_REP_G1 = 216,
    P_REP_G2 = 228, P_REP_LONG = 240, P_SLOT = 432, P_SPECIAL = 688,
    P_ALIGN = 802, P_LEN = 818, P_REP_LEN = 1332, P_LITERAL = 1846,
};

#define HEADER_LEN 18           /* 13-byte header, then the coder's 5 */
#define SIZE_LIMIT (16u << 20)

typedef struct {
    const uint8_t *src;
    size_t size, pos;
    uint32_t range, code;
    uint16_t *prob;
    int error;
} rc_t;

static void normalize(rc_t *d)
{
    if (d->range < (1u << 24)) {
        if (d->pos == d->size) {
            d->error = 1;
            return;
        }
        d->range <<= 8;
        d->code = (d->code << 8) | d->src[d->pos++];
    }
}

static unsigned bit(rc_t *d, unsigned index)
{
    uint16_t *p = &d->prob[index];
    uint32_t bound;

    normalize(d);
    bound = (d->range >> 11) * *p;
    if (d->code < bound) {
        d->range = bound;
        *p += (2048 - *p) >> 5;
        return 0;
    }
    d->range -= bound;
    d->code -= bound;
    *p -= *p >> 5;
    return 1;
}

static unsigned tree(rc_t *d, unsigned base, unsigned bits)
{
    unsigned symbol = 1;

    for (unsigned i = 0; i < bits; i++)
        symbol = (symbol << 1) | bit(d, base + symbol);
    return symbol - (1u << bits);
}

static unsigned reverse(rc_t *d, unsigned base, unsigned bits)
{
    unsigned symbol = 1, value = 0;

    for (unsigned i = 0; i < bits; i++) {
        unsigned b = bit(d, base + symbol);

        symbol = (symbol << 1) | b;
        value |= b << i;
    }
    return value;
}

static unsigned direct(rc_t *d, unsigned bits)
{
    unsigned value = 0;

    for (unsigned i = 0; i < bits; i++) {
        unsigned b;

        normalize(d);
        d->range >>= 1;
        b = d->code >= d->range;
        if (b)
            d->code -= d->range;
        value = (value << 1) | b;
    }
    return value;
}

static unsigned length(rc_t *d, unsigned base, unsigned pos)
{
    if (!bit(d, base))
        return tree(d, base + 2 + pos * 8, 3);
    if (!bit(d, base + 1))
        return 8 + tree(d, base + 130 + pos * 8, 3);
    return 16 + tree(d, base + 258, 8);
}

uint32_t sprd_lzma_size(const uint8_t *src, size_t len)
{
    uint32_t lo, hi;
    unsigned lc, lp;

    if (len < HEADER_LEN || src[0] >= 225 || src[13] != 0)
        return 0;
    lc = src[0] % 9;
    lp = (src[0] / 9) % 5;
    if (lc + lp > 4)
        return 0;
    lo = src[5] | (uint32_t)src[6] << 8 | (uint32_t)src[7] << 16 | (uint32_t)src[8] << 24;
    hi = src[9] | (uint32_t)src[10] << 8 | (uint32_t)src[11] << 16 | (uint32_t)src[12] << 24;
    if (hi || !lo || lo > SIZE_LIMIT)
        return 0;
    return lo;
}

int sprd_lzma_decode(const uint8_t *src, size_t len, uint8_t *out, size_t wanted)
{
    unsigned lc, lp, pb, state = 0, probs;
    uint32_t rep[4] = { 1, 1, 1, 1 };
    size_t pos = 0;
    rc_t d = { .src = src, .size = len, .pos = HEADER_LEN, .range = 0xffffffffu };
    uint32_t total = sprd_lzma_size(src, len);
    int rc = SPRD_LZMA_EDATA;

    if (!total || !wanted || wanted > total)
        return SPRD_LZMA_EHEADER;
    lc = src[0] % 9;
    lp = (src[0] / 9) % 5;
    pb = src[0] / 45;
    probs = P_LITERAL + (768u << (lc + lp));
    d.prob = kmalloc(probs * sizeof(uint16_t));
    if (!d.prob)
        return SPRD_LZMA_ENOMEM;
    for (unsigned i = 0; i < probs; i++)
        d.prob[i] = 1024;
    d.code = (uint32_t)src[14] << 24 | (uint32_t)src[15] << 16 | (uint32_t)src[16] << 8 | src[17];

    while (pos < wanted && !d.error) {
        unsigned ps = pos & ((1u << pb) - 1);
        unsigned len_;

        if (!bit(&d, P_MATCH + state * 16 + ps)) {
            unsigned previous = pos ? out[pos - 1] : 0;
            unsigned context = ((pos & ((1u << lp) - 1)) << lc) | (previous >> (8 - lc));
            unsigned base = P_LITERAL + context * 768, symbol = 1;

            while (symbol < 256)
                symbol = (symbol << 1) | bit(&d, base + symbol);
            out[pos++] = (uint8_t)symbol;
            state = state < 4 ? 0 : state < 10 ? state - 3 : state - 6;
            continue;
        }
        if (bit(&d, P_REP + state)) {
            if (!bit(&d, P_REP_G0 + state)) {
                if (!bit(&d, P_REP_LONG + state * 16 + ps)) {
                    if (rep[0] > pos)
                        goto done;
                    out[pos] = out[pos - rep[0]];
                    pos++;
                    state = state < 7 ? 9 : 11;
                    continue;
                }
            } else {
                unsigned which = !bit(&d, P_REP_G1 + state) ? 1 :
                                 !bit(&d, P_REP_G2 + state) ? 2 : 3;
                uint32_t distance = rep[which];

                for (unsigned i = which; i; i--)
                    rep[i] = rep[i - 1];
                rep[0] = distance;
            }
            len_ = length(&d, P_REP_LEN, ps) + 2;
            state = state < 7 ? 8 : 11;
        } else {
            unsigned slot, distance, bits;

            len_ = length(&d, P_LEN, ps);
            slot = tree(&d, P_SLOT + (len_ < 3 ? len_ : 3) * 64, 6);
            distance = slot;
            if (slot >= 4) {
                bits = (slot >> 1) - 1;
                distance = (2 | (slot & 1)) << bits;
                if (slot < 14) {
                    distance += reverse(&d, P_SPECIAL + distance - slot - 1, bits);
                } else {
                    distance += direct(&d, bits - 4) << 4;
                    distance += reverse(&d, P_ALIGN, 4);
                }
            }
            rep[3] = rep[2];
            rep[2] = rep[1];
            rep[1] = rep[0];
            rep[0] = distance + 1;
            len_ += 2;
            state = state < 7 ? 7 : 10;
        }
        if (!rep[0] || rep[0] > pos)
            goto done;
        for (unsigned i = 0; i < len_ && pos < wanted; i++, pos++)
            out[pos] = out[pos - rep[0]];
    }
    if (pos == wanted && !d.error)
        rc = SPRD_LZMA_OK;
done:
    kfree(d.prob);
    return rc;
}
