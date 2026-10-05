/*
 * SC6530C SDIO0 at 0x20700000: absent-card model.
 * Software resets self-clear, internal clock becomes stable, commands
 * time out and interrupt status is W1C. No card storage or DMA is modeled.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/log.h"
#include "qemu/bitops.h"
#include "hw/core/sysbus.h"
#include "hw/core/cpu.h"
#include "target/arm/cpu.h"
#include "trace.h"

#define TYPE_SC6530_SDIO "sc6530_sdio"
OBJECT_DECLARE_SIMPLE_TYPE(Sc6530SdioState, SC6530_SDIO)

/* ---------------------------------------------------------------------- */
/* Region geometry                                                        */
/* ---------------------------------------------------------------------- */

#define SC6530_SDIO_BASE   0x20700000ULL
#define SC6530_SDIO_SIZE   0x1000

/* ---------------------------------------------------------------------- */
/* Device state                                                           */
/* ---------------------------------------------------------------------- */

struct Sc6530SdioState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/

    MemoryRegion sdio_iomem;    /* SDIO0 0x20700000 */

    uint32_t sdio_regs[SC6530_SDIO_SIZE / 4];
};

/* ---------------------------------------------------------------------- */
/* Store+echo helpers (same family as the todo-12 sc6530_aux device;      */
/* RMW-correct).                                                          */
/* ---------------------------------------------------------------------- */

static uint64_t sc6530_sdio_regs_read(const uint32_t *regs, hwaddr offset,
                                      unsigned size)
{
    uint32_t word = regs[offset / 4];

    return extract32(word, (offset % 4) * 8, size * 8);
}

static void sc6530_sdio_regs_write(uint32_t *regs, hwaddr offset,
                                   uint64_t value, unsigned size)
{
    uint32_t word = regs[offset / 4];
    uint32_t mask = (size == 4) ? 0xffffffffu : ((1u << (size * 8)) - 1);
    unsigned shift = (offset % 4) * 8;

    regs[offset / 4] = (word & ~(mask << shift)) |
                       ((uint32_t)value & mask) << shift;
}

static uint32_t sc6530_sdio_guest_pc(void)
{
    CPUState *cs = current_cpu;

    if (cs) {
        return ARM_CPU(cs)->env.regs[15];
    }
    return 0;
}

/* ---------------------------------------------------------------------- */
/* Bank: store+echo. Status reads return 0 until written = "no card"      */
/* (the guest's init completes without a card; bounded waits time out).   */
/* ---------------------------------------------------------------------- */

static uint64_t sc6530_sdio_read(void *opaque, hwaddr offset, unsigned size)
{
    Sc6530SdioState *s = opaque;

    return sc6530_sdio_regs_read(s->sdio_regs, offset, size);
}

static void sc6530_sdio_write(void *opaque, hwaddr offset,
                              uint64_t value, unsigned size)
{
    Sc6530SdioState *s = opaque;

    if ((offset & ~3) == 0x30) {
        uint32_t clear = (uint32_t)value << ((offset & 3) * 8);
        s->sdio_regs[0x30 / 4] &= ~clear; /* interrupt status W1C */
        return;
    }
    if ((offset & ~3) == 0x24) {
        return; /* present state is read-only, no card inserted */
    }
    sc6530_sdio_regs_write(s->sdio_regs, offset, value, size);
    if ((offset & ~3) == 0x2c) {
        /* Stock 0xb55c8 polls software-reset b24 until it clears.
         * Command/data resets b25/b26 also self-clear. */
        s->sdio_regs[0x2c / 4] &= ~0x07000000u;
        /* SDHCI internal-clock-stable follows internal-clock-enable. */
        s->sdio_regs[0x2c / 4] &= ~2u;
        if (s->sdio_regs[0x2c / 4] & 1) {
            s->sdio_regs[0x2c / 4] |= 2;
        }
    }
    if ((offset & ~3) == 0xc && offset + size > 0xe) {
        /* An absent card cannot answer a command: command timeout + error. */
        s->sdio_regs[0x30 / 4] |= 0x00018000;
    }
    qemu_log("sc6530_sdio: write addr=0x%08" PRIx64 " val=0x%08" PRIx64
             " pc=0x%08" PRIx32 "\n",
             SC6530_SDIO_BASE + offset, value, sc6530_sdio_guest_pc());
}

static const MemoryRegionOps sc6530_sdio_ops = {
    .read  = sc6530_sdio_read,
    .write = sc6530_sdio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 1, .max_access_size = 4 },
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

/* ---------------------------------------------------------------------- */
/* SysBus device                                                          */
/* ---------------------------------------------------------------------- */

static void sc6530_sdio_reset(DeviceState *dev)
{
    Sc6530SdioState *s = SC6530_SDIO(dev);

    memset(s->sdio_regs, 0, sizeof(s->sdio_regs));
}

static void sc6530_sdio_init(Object *obj)
{
    Sc6530SdioState *s = SC6530_SDIO(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->sdio_iomem, obj, &sc6530_sdio_ops, s,
                          "sc6530-sdio", SC6530_SDIO_SIZE);
    sysbus_init_mmio(sbd, &s->sdio_iomem);
}

static void sc6530_sdio_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, sc6530_sdio_reset);
}

static const TypeInfo sc6530_sdio_info = {
    .name          = TYPE_SC6530_SDIO,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Sc6530SdioState),
    .instance_init = sc6530_sdio_init,
    .class_init    = sc6530_sdio_class_init,
};

static void sc6530_sdio_register_types(void)
{
    type_register_static(&sc6530_sdio_info);
}

type_init(sc6530_sdio_register_types)
