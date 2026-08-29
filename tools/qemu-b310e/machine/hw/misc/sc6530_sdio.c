/*
 * Spreadtrum SC6530C SDIO0 controller log+store no-op model.
 *
 * Todo 19 of .omo/plans/b310e-qemu-machine.md (Wave 4).
 *
 * The plan says: a no-op model so the stock OS's SD init and our os.bin's
 * SD probe (demo_sd_task / os-diag-sd) do not fault. Store+echo uint32
 * bank at SDIO0 0x20700000 (0x1000). Reads return the last written value
 * and 0 for status-type registers that are never written = "no card
 * present" - benign: the guest's init completes with an absent card
 * instead of faulting. All SDIO waits in drivers/sdio.c are bounded
 * (SDIO_WAIT_BUDGET 1M, iteration-capped CMD8/ACMD41 loops), so a never-
 * completing card probe cannot hang the guest; it degrades to "no card".
 *
 * Guest register map (fpdoom sdio.c ground truth, drivers/AGENTS.md):
 * dma_addr / blk_size / arg / tr_mode / resp[4] / buf_port / state /
 * ctrl1 / ctrl2 / int_st / int_en / int_sig at 0x20700000 + low offsets.
 * Store+echo keeps the guest's read-modify-write chains stable (e.g. the
 * fpdoom pinmux RMW on 0x8c000250-264 lives in the todo-12 aux pinmux
 * bank, NOT here - do not map 0x8c here).
 *
 * If Wave 5 finds a STOCK-OS SD wait that is unbounded (poll-until-set on
 * a status bit), the todo-12 benign-ready table rule applies (answer the
 * known polled register with a benign value); until then: log+store only.
 *
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

    sc6530_sdio_regs_write(s->sdio_regs, offset, value, size);
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
