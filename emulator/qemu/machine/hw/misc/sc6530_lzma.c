/*
 * SC6530C LZMA accelerator at 0x20e00000. Register behavior reconstructed
 * from stock NOR 0x69f50..0x6a100, corroborated by the v5 register map.
 * The stock stream uses 11-bit LZMA1 probabilities and plain literals even
 * after matches. Ordinary liblzma expects matched literals and rejects it.
 * Decoder implements the LZMA1 state machine described by the public-domain
 * LZMA SDK. No vendor driver or encoder source is included here.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "system/address-spaces.h"

#define TYPE_SC6530_LZMA "sc6530_lzma"
OBJECT_DECLARE_SIMPLE_TYPE(Sc6530LzmaState, SC6530_LZMA)

#define LZMA_LIMIT (16 * 1024 * 1024)
enum {
    P_MATCH = 0, P_REP = 192, P_REP_G0 = 204, P_REP_G1 = 216,
    P_REP_G2 = 228, P_REP_LONG = 240, P_SLOT = 432, P_SPECIAL = 688,
    P_ALIGN = 802, P_LEN = 818, P_REP_LEN = 1332, P_LITERAL = 1846,
};

typedef struct RangeDecoder {
    const uint8_t *src;
    size_t size, pos;
    uint32_t range, code;
    uint16_t *prob;
    bool error;
} RangeDecoder;

static void range_normalize(RangeDecoder *d)
{
    if (d->range < (1u << 24)) {
        if (d->pos == d->size) {
            d->error = true;
            return;
        }
        d->range <<= 8;
        d->code = (d->code << 8) | d->src[d->pos++];
    }
}

static unsigned range_bit(RangeDecoder *d, unsigned index)
{
    uint16_t *p = &d->prob[index];
    uint32_t bound;

    range_normalize(d);
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

static unsigned range_tree(RangeDecoder *d, unsigned base, unsigned bits)
{
    unsigned symbol = 1;
    for (unsigned i = 0; i < bits; i++) {
        symbol = (symbol << 1) | range_bit(d, base + symbol);
    }
    return symbol - (1u << bits);
}

static unsigned range_reverse(RangeDecoder *d, unsigned base, unsigned bits)
{
    unsigned symbol = 1, value = 0;
    for (unsigned i = 0; i < bits; i++) {
        unsigned bit = range_bit(d, base + symbol);
        symbol = (symbol << 1) | bit;
        value |= bit << i;
    }
    return value;
}

static unsigned range_direct(RangeDecoder *d, unsigned bits)
{
    unsigned value = 0;
    for (unsigned i = 0; i < bits; i++) {
        unsigned bit;
        range_normalize(d);
        d->range >>= 1;
        bit = d->code >= d->range;
        if (bit) {
            d->code -= d->range;
        }
        value = (value << 1) | bit;
    }
    return value;
}

static unsigned range_length(RangeDecoder *d, unsigned base, unsigned pos)
{
    if (!range_bit(d, base)) {
        return range_tree(d, base + 2 + pos * 8, 3);
    }
    if (!range_bit(d, base + 1)) {
        return 8 + range_tree(d, base + 130 + pos * 8, 3);
    }
    return 16 + range_tree(d, base + 258, 8);
}

static bool sc6530_lzma_decode(const uint8_t *src, size_t size,
                               uint8_t *out, size_t wanted, size_t *consumed)
{
    unsigned lc, lp, pb, state = 0;
    uint32_t rep[4] = { 1, 1, 1, 1 };
    size_t pos = 0;
    RangeDecoder d = { .src = src, .size = size, .pos = 18,
                       .range = UINT32_MAX };
    bool success = false;

    if (size < 18 || src[0] >= 225 || src[13] != 0) {
        return false;
    }
    lc = src[0] % 9;
    lp = (src[0] / 9) % 5;
    pb = src[0] / 45;
    if (lc > 8 || lc + lp > 4 || wanted > LZMA_LIMIT) {
        return false;
    }
    unsigned probs = P_LITERAL + (768 << (lc + lp));
    d.prob = g_new(uint16_t, probs);
    for (unsigned i = 0; i < probs; i++) {
        d.prob[i] = 1024;
    }
    d.code = ldl_be_p(src + 14);
    while (pos < wanted && !d.error) {
        unsigned ps = pos & ((1u << pb) - 1);
        unsigned length;
        if (!range_bit(&d, P_MATCH + state * 16 + ps)) {
            unsigned previous = pos ? out[pos - 1] : 0;
            unsigned context = ((pos & ((1u << lp) - 1)) << lc) |
                               (previous >> (8 - lc));
            unsigned base = P_LITERAL + context * 768, symbol = 1;
            while (symbol < 256) {
                symbol = (symbol << 1) | range_bit(&d, base + symbol);
            }
            out[pos++] = symbol;
            state = state < 4 ? 0 : state < 10 ? state - 3 : state - 6;
            continue;
        }
        if (range_bit(&d, P_REP + state)) {
            if (!range_bit(&d, P_REP_G0 + state)) {
                if (!range_bit(&d, P_REP_LONG + state * 16 + ps)) {
                    if (rep[0] > pos) {
                        goto done;
                    }
                    out[pos] = out[pos - rep[0]];
                    pos++;
                    state = state < 7 ? 9 : 11;
                    continue;
                }
            } else {
                unsigned which = !range_bit(&d, P_REP_G1 + state) ? 1 :
                                 !range_bit(&d, P_REP_G2 + state) ? 2 : 3;
                uint32_t distance = rep[which];
                for (unsigned i = which; i; i--) {
                    rep[i] = rep[i - 1];
                }
                rep[0] = distance;
            }
            length = range_length(&d, P_REP_LEN, ps) + 2;
            state = state < 7 ? 8 : 11;
        } else {
            unsigned slot, distance, direct;
            length = range_length(&d, P_LEN, ps);
            slot = range_tree(&d, P_SLOT + MIN(length, 3u) * 64, 6);
            distance = slot;
            if (slot >= 4) {
                direct = (slot >> 1) - 1;
                distance = (2 | (slot & 1)) << direct;
                if (slot < 14) {
                    distance += range_reverse(&d, P_SPECIAL + distance - slot - 1,
                                               direct);
                } else {
                    distance += range_direct(&d, direct - 4) << 4;
                    distance += range_reverse(&d, P_ALIGN, 4);
                }
            }
            rep[3] = rep[2];
            rep[2] = rep[1];
            rep[1] = rep[0];
            rep[0] = distance + 1;
            length += 2;
            state = state < 7 ? 7 : 10;
        }
        if (!rep[0] || rep[0] > pos) {
            goto done;
        }
        for (unsigned i = 0; i < length && pos < wanted; i++) {
            out[pos] = out[pos - rep[0]];
            pos++;
        }
    }
    success = pos == wanted && !d.error;
done:
    *consumed = d.pos;
    g_free(d.prob);
    return success;
}

struct Sc6530LzmaState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    uint32_t reg[13];
    uint32_t raw;
};

static void sc6530_lzma_irq(Sc6530LzmaState *s)
{
    qemu_set_irq(s->irq, (s->raw & s->reg[2] & 31) != 0);
}

static void sc6530_lzma_run(Sc6530LzmaState *s)
{
    uint32_t *r = s->reg;
    g_autofree uint8_t *src = NULL, *out = NULL;
    uint64_t unpacked;
    size_t wanted, consumed = 0, source_len = r[5];

    r[0] &= ~1u;
    r[10] = r[11] = r[12] = 0;
    /* Stock's DSP loader sets SRC_LEN=0 (NOR 0xa043c). Bound that stream
     * to its contiguous RAM/ROM mapping, never to arbitrary MMIO. */
    if (!source_len) {
        hwaddr translated, length = LZMA_LIMIT;
        MemoryRegion *mr = address_space_translate(&address_space_memory,
            r[3], &translated, &length, false, MEMTXATTRS_UNSPECIFIED);
        if (!memory_region_is_ram(mr) && !memory_region_is_romd(mr)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sc6530_lzma: unbounded source is MMIO %s\n",
                          memory_region_name(mr));
            s->raw |= 2;
            goto done;
        }
        /* address_space_translate clamps RAM lengths, but not ROMD. */
        source_len = MIN(length, memory_region_size(mr) - translated);
    }
    if (source_len < 18 || source_len > LZMA_LIMIT || !r[6] || r[6] > LZMA_LIMIT ||
        r[9] > LZMA_LIMIT - r[6]) {
        s->raw |= 8; /* destination length error */
        goto done;
    }
    src = g_malloc(source_len);
    if (address_space_read(&address_space_memory, r[3], MEMTXATTRS_UNSPECIFIED,
                           src, source_len) != MEMTX_OK) {
        s->raw |= 2;
        goto done;
    }
    unpacked = ldq_le_p(src + 5);
    if (!unpacked || unpacked > LZMA_LIMIT || r[9] >= unpacked) {
        s->raw |= 4;
        goto done;
    }
    r[12] = unpacked;
    wanted = MIN(unpacked, (uint64_t)r[9] + r[6]);
    if ((r[0] & 12) == 12) {
        wanted = MIN(unpacked, r[8]);
    }
    if (wanted <= r[9] || ((r[0] & 4) && wanted > r[8])) {
        s->raw |= 8;
        goto done;
    }
    out = g_malloc(wanted);
    if (!sc6530_lzma_decode(src, source_len, out, wanted, &consumed)) {
        s->raw |= 4;
        goto done;
    }
    if ((r[0] & 4) &&
        address_space_write(&address_space_memory, r[7], MEMTXATTRS_UNSPECIFIED,
                            out, wanted) != MEMTX_OK) {
        s->raw |= 2;
        goto done;
    }
    r[10] = wanted;
    r[11] = MIN(r[6], wanted - r[9]);
    if (address_space_write(&address_space_memory, r[4], MEMTXATTRS_UNSPECIFIED,
                            out + r[9], r[11]) != MEMTX_OK) {
        s->raw |= 2;
        goto done;
    }
    s->raw |= 1;
done:
    qemu_log_mask(LOG_GUEST_ERROR,
                  "sc6530_lzma: src=%08x len=%u dst=%08x size=%u start=%u ctrl=%x out=%u status=%x consumed=%zu\n",
                  r[3], r[5], r[4], r[6], r[9], r[0], r[11], s->raw, consumed);
    sc6530_lzma_irq(s);
}

static uint64_t sc6530_lzma_read(void *opaque, hwaddr offset, unsigned size)
{
    Sc6530LzmaState *s = opaque;
    unsigned index = offset / 4;
    uint32_t value = index < 13 ? s->reg[index] : 0;
    if (index == 1) {
        value = s->reg[0];
    } else if (index == 2) {
        value = (s->reg[2] & 31) | (s->raw << 24) |
                ((s->raw & s->reg[2] & 31) << 16);
    }
    return (value >> ((offset & 3) * 8)) &
           (size == 4 ? UINT32_MAX : (1u << (size * 8)) - 1);
}

static void sc6530_lzma_write(void *opaque, hwaddr offset,
                             uint64_t value, unsigned size)
{
    Sc6530LzmaState *s = opaque;
    unsigned index = offset / 4, shift = (offset & 3) * 8;
    uint32_t mask = size == 4 ? UINT32_MAX : (1u << (size * 8)) - 1;
    if (index >= 13) {
        return;
    }
    value = (s->reg[index] & ~(mask << shift)) | ((value & mask) << shift);
    if (index == 2) {
        s->raw &= ~((value >> 8) & 31);
        s->reg[2] = value & 31;
        sc6530_lzma_irq(s);
    } else if (index != 1) {
        s->reg[index] = value;
        if (index == 0 && (value & 1)) {
            sc6530_lzma_run(s);
        }
    }
}

static const MemoryRegionOps sc6530_lzma_ops = {
    .read = sc6530_lzma_read,
    .write = sc6530_lzma_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static void sc6530_lzma_reset(DeviceState *dev)
{
    Sc6530LzmaState *s = SC6530_LZMA(dev);
    memset(s->reg, 0, sizeof(s->reg));
    s->raw = 0;
    sc6530_lzma_irq(s);
}

static void sc6530_lzma_init(Object *obj)
{
    Sc6530LzmaState *s = SC6530_LZMA(obj);
    memory_region_init_io(&s->iomem, obj, &sc6530_lzma_ops, s,
                          "sc6530-lzma", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static void sc6530_lzma_class_init(ObjectClass *klass, const void *data)
{
    device_class_set_legacy_reset(DEVICE_CLASS(klass), sc6530_lzma_reset);
}

static const TypeInfo sc6530_lzma_info = {
    .name = TYPE_SC6530_LZMA,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Sc6530LzmaState),
    .instance_init = sc6530_lzma_init,
    .class_init = sc6530_lzma_class_init,
};

static void sc6530_lzma_register_types(void)
{
    type_register_static(&sc6530_lzma_info);
}
type_init(sc6530_lzma_register_types)
