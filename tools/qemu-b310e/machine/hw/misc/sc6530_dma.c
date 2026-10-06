/*
 * SC6530C DMA, reconstructed from e52q7a.bin, SHA256 5e44e085...00846c92.
 * NOR 0x32580 initializes 32 channels at 0x20100fc0 + id * 0x40
 * (ids 1..32), and registers interrupt 20. 0x32a68 programs the channel
 * registers. SC6530C standard channels use width fields [31:30]/[29:28]
 * and address-fix bits 20/21 (the live stock path at 0x32b8c). Full
 * channels use [27:26]/[25:24] and explicit transfer steps.
 * Implements normal transfers and VBC peripheral requests. Linked lists,
 * wrapping, byte swapping and non-VBC hardware requests are not yet modeled.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/bswap.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/core/cpu.h"
#include "target/arm/cpu.h"
#include "system/address-spaces.h"
#include "trace.h"

#define TYPE_SC6530_DMA "sc6530_dma"
OBJECT_DECLARE_SIMPLE_TYPE(Sc6530DmaState, SC6530_DMA)
size_t sc6530_dma_request(Object *obj, hwaddr destination, size_t bytes);

#define DMA_CHANNELS 32
#define DMA_SIZE 0x3000
#define UID_BASE 0x2000
#define UID_COUNT 64
#define CH_BASE 0x1000
#define CH_STRIDE 0x40
#define CH_PAUSE 0
#define CH_REQ 1
#define CH_CFG 2
#define CH_INT 3
#define CH_SRC 4
#define CH_DST 5
#define CH_FRAG 6
#define CH_BLOCK 7
#define CH_TRANS 8
#define CH_STEP 9

typedef struct Sc6530DmaChannel {
    uint32_t reg[16];
    uint32_t remaining;
    uint32_t block_done;
    uint32_t block_length;
} Sc6530DmaChannel;

struct Sc6530DmaState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    uint32_t global[CH_BASE / 4];
    uint32_t request_map[UID_COUNT]; /* one-based hardware request -> channel */
    Sc6530DmaChannel channel[DMA_CHANNELS];
};

static uint32_t sc6530_dma_status(Sc6530DmaState *s, bool masked)
{
    uint32_t status = 0;
    for (unsigned i = 0; i < DMA_CHANNELS; i++) {
        uint32_t intr = s->channel[i].reg[CH_INT];
        uint32_t raw = (intr >> 8) & 0x1f;
        if (masked) {
            raw &= intr & 0x1f;
        }
        if (raw) {
            status |= 1u << i;
        }
    }
    return status;
}

static void sc6530_dma_irq(Sc6530DmaState *s)
{
    qemu_set_irq(s->irq, sc6530_dma_status(s, true) != 0);
}

static int32_t sc6530_dma_step(uint32_t value)
{
    return value & 0x8000 ? -(int32_t)(value & 0x7fff) : value;
}

static size_t sc6530_dma_transfer(Sc6530DmaState *s, unsigned id,
                                 size_t budget)
{
    Sc6530DmaChannel *c = &s->channel[id];
    uint32_t *r = c->reg;
    unsigned src_width = 1u << ((r[CH_FRAG] >> (id < 24 ? 30 : 26)) & 3);
    unsigned dst_width = 1u << ((r[CH_FRAG] >> (id < 24 ? 28 : 24)) & 3);
    uint32_t block = c->block_length;
    size_t done = 0;

    if (!(r[CH_CFG] & 1) || (r[CH_PAUSE] & 1) ||
        (s->global[0] & 1) || !c->remaining) {
        return 0;
    }
    if ((r[CH_CFG] & 0x10) || src_width > 4 || dst_width > 4 || !block) {
        qemu_log_mask(LOG_UNIMP, "sc6530_dma: unsupported config ch=%u\n",
                      id + 1);
        r[CH_INT] |= 1u << 12;
        r[CH_CFG] &= ~1u;
        sc6530_dma_irq(s);
        return 0;
    }
    while (c->remaining >= src_width && budget - done >= src_width) {
        uint8_t data[4] = { 0 };
        int32_t src_step = src_width, dst_step = dst_width;

        if (id >= 24) {
            src_step = sc6530_dma_step(r[CH_STEP] & 0xffff);
            dst_step = sc6530_dma_step(r[CH_STEP] >> 16);
        } else if (r[CH_FRAG] & (1u << 20)) {
            /* ADDR_FIX_SEL selects destination when set. */
            if (r[CH_FRAG] & (1u << 21)) {
                dst_step = 0;
            } else {
                src_step = 0;
            }
        }
        if (address_space_read(&address_space_memory, r[CH_SRC],
                               MEMTXATTRS_UNSPECIFIED, data, src_width) !=
            MEMTX_OK ||
            address_space_write(&address_space_memory, r[CH_DST],
                                MEMTXATTRS_UNSPECIFIED, data, dst_width) !=
            MEMTX_OK) {
            r[CH_INT] |= 1u << 12;
            r[CH_CFG] &= ~1u;
            break;
        }
        r[CH_SRC] += src_step;
        r[CH_DST] += dst_step;
        c->remaining -= src_width;
        c->block_done += src_width;
        done += src_width;
        if (c->block_done >= block) {
            c->block_done = 0;
            r[CH_INT] |= (1u << 8) | (1u << 9);
        }
        if (!c->remaining) {
            if (id >= 24) {
                r[CH_INT] |= 1u << 10;
            }
            r[CH_CFG] &= ~1u;
        }
    }
    if (id < 24) {
        /* SC6530C DMA r3p0 has a 17-bit remaining-byte counter. The
         * live stock getter 0x32e3c subtracts it from the previous read;
         * a constant block length makes the PCM producer wait forever. */
        r[CH_BLOCK] = c->remaining & 0x1ffff;
    }
    trace_sc6530_dma_transfer(id + 1, done, c->remaining);
    sc6530_dma_irq(s);
    return done;
}

/* VBC calls this when a playback bank is available for refill. */
size_t sc6530_dma_request(Object *obj, hwaddr destination, size_t bytes)
{
    Sc6530DmaState *s = SC6530_DMA(obj);
    /* Stock 0xa7b96 maps DA0 request15 at +0x2038 to channel4,
     * DA1 request16 at +0x203c to channel3 (all one-based). A destination
     * match alone bypasses this routing and lets an incomplete driver play. */
    unsigned request = destination == 0x82003000 ? 15 :
                       destination == 0x82003004 ? 16 : 0;
    if (!request) {
        return 0;
    }
    unsigned channel = s->request_map[request - 1];
    if (!channel || channel > DMA_CHANNELS ||
        s->channel[channel - 1].reg[CH_DST] != destination) {
        return 0;
    }
    return sc6530_dma_transfer(s, channel - 1, bytes);
}

static uint64_t sc6530_dma_read(void *opaque, hwaddr offset, unsigned size)
{
    Sc6530DmaState *s = opaque;
    uint32_t val;
    if (offset >= UID_BASE && offset < UID_BASE + sizeof(s->request_map)) {
        val = s->request_map[(offset - UID_BASE) / 4];
    } else if (offset >= CH_BASE && offset < CH_BASE + DMA_CHANNELS * CH_STRIDE) {
        unsigned ch = (offset - CH_BASE) / CH_STRIDE;
        unsigned reg = (offset % CH_STRIDE) / 4;
        val = s->channel[ch].reg[reg];
        if (reg == CH_PAUSE) {
            val = (val & 1) | ((val & 1) << 16);
        } else if (reg == CH_INT) {
            val |= (((val >> 8) & val & 0x1f) << 16);
        }
    } else if (offset < CH_BASE) {
        val = s->global[offset / 4];
        if ((offset & ~3) == 0) {
            val = (val & 1) | ((val & 1) << 16);
        } else if ((offset & ~3) == 12) {
            val = sc6530_dma_status(s, false);
        } else if ((offset & ~3) == 16) {
            /* Stock IRQ20 entry 0x324b2 reads BASE + 0x10. */
            val = sc6530_dma_status(s, true);
        } else if ((offset & ~3) == 0x14) {
            val = 0; /* no outstanding asynchronous requests */
        } else if ((offset & ~3) == 0x18) {
            val = 0;
            for (unsigned i = 0; i < DMA_CHANNELS; i++) {
                val |= (s->channel[i].reg[CH_CFG] & 1) << i;
            }
        }
    } else {
        val = 0; /* reserved gaps must not index past the channel array */
    }
    return (val >> ((offset & 3) * 8)) &
           (size == 4 ? UINT32_MAX : (1u << (size * 8)) - 1);
}

static void sc6530_dma_write(void *opaque, hwaddr offset,
                             uint64_t value, unsigned size)
{
    Sc6530DmaState *s = opaque;
    unsigned shift = (offset & 3) * 8;
    uint32_t mask = size == 4 ? UINT32_MAX : (1u << (size * 8)) - 1;
    uint32_t *word;

    trace_sc6530_dma_write(0x20100000 + offset, value,
                          current_cpu ? ARM_CPU(current_cpu)->env.regs[15] : 0);
    if (offset >= UID_BASE && offset < UID_BASE + sizeof(s->request_map)) {
        word = &s->request_map[(offset - UID_BASE) / 4];
        *word = (*word & ~(mask << shift)) | ((value & mask) << shift);
        return;
    }

    if (offset < CH_BASE) {
        word = &s->global[offset / 4];
        *word = (*word & ~(mask << shift)) | ((value & mask) << shift);
        return;
    }
    if (offset >= CH_BASE + DMA_CHANNELS * CH_STRIDE) {
        return;
    }
    unsigned ch = (offset - CH_BASE) / CH_STRIDE;
    unsigned reg = (offset % CH_STRIDE) / 4;
    Sc6530DmaChannel *c = &s->channel[ch];
    uint32_t old = c->reg[reg];
    uint32_t val = (old & ~(mask << shift)) | ((value & mask) << shift);

    if (reg == CH_INT) {
        /* W1C strobes [28:24], raw [12:8], masks [20:16], enables [4:0]. */
        c->reg[reg] = ((old & 0x1f00) & ~((val >> 16) & 0x1f00)) |
                      (val & 0x1f);
        sc6530_dma_irq(s);
        return;
    }
    c->reg[reg] = val;
    if (reg == CH_CFG && !(old & 1) && (val & 1)) {
        c->block_length = c->reg[CH_BLOCK] & (ch < 24 ? 0x1ffff : 0xffff);
        c->remaining = ch < 24 ? c->block_length : c->reg[CH_TRANS];
        c->block_done = 0;
    }
    if (reg == CH_REQ && (val & 1)) {
        /* Software requests are bounded by the programmed transfer length. */
        if (c->remaining <= 16 * 1024 * 1024) {
            sc6530_dma_transfer(s, ch, c->remaining);
        }
        c->reg[CH_REQ] = 0;
    }
}

static const MemoryRegionOps sc6530_dma_ops = {
    .read = sc6530_dma_read,
    .write = sc6530_dma_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4,
               .unaligned = false },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static void sc6530_dma_reset(DeviceState *dev)
{
    Sc6530DmaState *s = SC6530_DMA(dev);
    memset(s->global, 0, sizeof(s->global));
    memset(s->request_map, 0, sizeof(s->request_map));
    memset(s->channel, 0, sizeof(s->channel));
    sc6530_dma_irq(s);
}

static void sc6530_dma_init(Object *obj)
{
    Sc6530DmaState *s = SC6530_DMA(obj);
    memory_region_init_io(&s->iomem, obj, &sc6530_dma_ops, s,
                          "sc6530-dma", DMA_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static void sc6530_dma_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->desc = "SC6530C normal DMA and VBC requests";
    device_class_set_legacy_reset(dc, sc6530_dma_reset);
}

static const TypeInfo sc6530_dma_info = {
    .name = TYPE_SC6530_DMA,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Sc6530DmaState),
    .instance_init = sc6530_dma_init,
    .class_init = sc6530_dma_class_init,
};

static void sc6530_dma_register_types(void)
{
    type_register_static(&sc6530_dma_info);
}
type_init(sc6530_dma_register_types)
