/*
 * Spreadtrum SC6530C USB controller log+store no-op model.
 *
 * Todo 19 of .omo/plans/b310e-qemu-machine.md (Wave 4).
 *
 * The plan says: a no-op model so the stock OS's USB init and our os.bin's
 * usb_debug_init (EP2 poll 0x20380010) do not fault. The DEFAULT is the
 * log+store no-op; the EP2 SETUP-answer fallback is a contingent scope
 * exception (see the plan todo 19 + docs/b310e-qemu.md) that is NOT
 * implemented here - our guest's USB polls are all BOUNDED by design
 * (drivers/usb_debug.c: USB_TX_BUDGET 2M, USB_CONNECT_BUDGET 10M,
 * usb_debug_poll is a single non-blocking round), so a "host never
 * connected" model cannot hang it.
 *
 * Model:
 *   Region 0: register bank 0x20300000, 0x1000. Store+echo uint32 array.
 *   Region 1: FIFO window 0x20380000, 0x100 - the EP0/EP3/EP2 FIFOs
 *             (EP0 +0x0, EP3 +0x8, EP2 +0x10; the guest's EP2 OUT FIFO
 *             reads at 0x20380010). Store+echo too: reads return 0 until
 *             something writes (nothing ever does for EP2 = "host not
 *             connected" forever).
 *
 * The "host never connected" semantics fall out of the store+echo model
 * with no special-casing: the guest only ever WRITES the control/clear
 * registers (ENDPn_CTRL, INT_CTRL_*, INT_CLR_*, USB_CTRL, TIMEOUT_LMT) and
 * READS the status registers (INT_STS_ENDP0 0x6c, INT_STS_ENDP2 0x110,
 * INT_STS_ENDP3 0x150, INT_STS 0x18). Status offsets are never written, so
 * they read 0 forever: no SETUP_TRANS_END, no TRANSACTION_END, no
 * TRANSFER_END. usb_debug_init's connect wait burns its 10M budget and
 * returns; usb_debug_poll reads 0s and re-arms (logged no-ops); TX waits
 * time out on their 2M budgets and set s_link_down (all sends drop
 * immediately after - no spin, AGENTS.md). Read-modify-write chains on the
 * control registers stay stable (echo).
 *
 * If Wave 5 finds a STOCK-OS USB wait that is unbounded (poll-until-set on
 * a status bit), the record-the-decision-first exception applies: model the
 * EP2 SETUP answer (usb_debug_endp0_setup semantics) or add a benign-ready
 * entry. Until then: log+store only.
 *
 * Region geometry (guest register map from drivers/usb_debug.c, fpdoom
 * usbio.c ground truth):
 *   0x00 USB_CTRL, 0x18 INT_STS, 0x28 TIMEOUT_LMT,
 *   0x40 TR_SIZE_IN_ENDP0, 0x5c/0x60 REQ_SETUP_LOW/HIGH,
 *   0x64 ENDP0_CTRL, 0x68 INT_CTRL_ENDP0, 0x6c INT_STS_ENDP0,
 *   0x70 INT_CLR_ENDP0, 0x100 ENDP2_CTRL, 0x104 RCV_DATA_ENDP2,
 *   0x10c INT_CTRL_ENDP2, 0x110 INT_STS_ENDP2, 0x114 INT_CLR_ENDP2,
 *   0x140 ENDP3_CTRL, 0x148 TRANS_SIZE_ENDP3, 0x14c INT_CTRL_ENDP3,
 *   0x150 INT_STS_ENDP3, 0x154 INT_CLR_ENDP3,
 *   FIFOs: EP0 0x20380000, EP3 0x20380008, EP2 0x20380010.
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

#define TYPE_SC6530_USB "sc6530_usb"
OBJECT_DECLARE_SIMPLE_TYPE(Sc6530UsbState, SC6530_USB)

/* ---------------------------------------------------------------------- */
/* Region geometry                                                        */
/* ---------------------------------------------------------------------- */

#define SC6530_USB_BASE       0x20300000ULL
#define SC6530_USB_SIZE       0x1000
#define SC6530_USB_FIFO_BASE  0x20380000ULL
#define SC6530_USB_FIFO_SIZE  0x100

/* ---------------------------------------------------------------------- */
/* Device state                                                           */
/* ---------------------------------------------------------------------- */

struct Sc6530UsbState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/

    MemoryRegion usb_iomem;     /* register bank 0x20300000 */
    MemoryRegion fifo_iomem;    /* FIFO window 0x20380000 (EP0/3/2) */

    uint32_t usb_regs[SC6530_USB_SIZE / 4];
    uint32_t fifo_regs[SC6530_USB_FIFO_SIZE / 4];
};

/* ---------------------------------------------------------------------- */
/* Store+echo helpers (size-aware byte/word access on the arrays - the    */
/* same helpers the todo-12 sc6530_aux device uses; RMW-correct).         */
/* ---------------------------------------------------------------------- */

static uint64_t sc6530_usb_regs_read(const uint32_t *regs, hwaddr offset,
                                     unsigned size)
{
    uint32_t word = regs[offset / 4];

    return extract32(word, (offset % 4) * 8, size * 8);
}

static void sc6530_usb_regs_write(uint32_t *regs, hwaddr offset,
                                  uint64_t value, unsigned size)
{
    uint32_t word = regs[offset / 4];
    uint32_t mask = (size == 4) ? 0xffffffffu : ((1u << (size * 8)) - 1);
    unsigned shift = (offset % 4) * 8;

    regs[offset / 4] = (word & ~(mask << shift)) |
                       ((uint32_t)value & mask) << shift;
}

static uint32_t sc6530_usb_guest_pc(void)
{
    CPUState *cs = current_cpu;

    if (cs) {
        return ARM_CPU(cs)->env.regs[15];
    }
    return 0;
}

/* ---------------------------------------------------------------------- */
/* Register bank: store+echo. Status reads return 0 (never written =      */
/* host never connected - see the header comment). Writes are logged.     */
/* ---------------------------------------------------------------------- */

static uint64_t sc6530_usb_read(void *opaque, hwaddr offset, unsigned size)
{
    Sc6530UsbState *s = opaque;

    return sc6530_usb_regs_read(s->usb_regs, offset, size);
}

static void sc6530_usb_write(void *opaque, hwaddr offset,
                             uint64_t value, unsigned size)
{
    Sc6530UsbState *s = opaque;

    sc6530_usb_regs_write(s->usb_regs, offset, value, size);
    qemu_log("sc6530_usb: write addr=0x%08" PRIx64 " val=0x%08" PRIx64
             " pc=0x%08" PRIx32 "\n",
             SC6530_USB_BASE + offset, value, sc6530_usb_guest_pc());
}

static const MemoryRegionOps sc6530_usb_ops = {
    .read  = sc6530_usb_read,
    .write = sc6530_usb_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 1, .max_access_size = 4 },
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

/* ---------------------------------------------------------------------- */
/* FIFO window: store+echo. The guest WRITES the EP0/EP3 IN FIFOs (data   */
/* it tries to transmit to the absent host) and READS the EP2 OUT FIFO    */
/* (0x20380010) - which returns 0 forever = "no HOST_CONNECT ever         */
/* arrived". That is exactly the "host not connected" model.              */
/* ---------------------------------------------------------------------- */

static uint64_t sc6530_usb_fifo_read(void *opaque, hwaddr offset,
                                     unsigned size)
{
    Sc6530UsbState *s = opaque;

    return sc6530_usb_regs_read(s->fifo_regs, offset, size);
}

static void sc6530_usb_fifo_write(void *opaque, hwaddr offset,
                                  uint64_t value, unsigned size)
{
    Sc6530UsbState *s = opaque;

    sc6530_usb_regs_write(s->fifo_regs, offset, value, size);
    qemu_log("sc6530_usb: fifo write addr=0x%08" PRIx64 " val=0x%08"
             PRIx64 " pc=0x%08" PRIx32 "\n",
             SC6530_USB_FIFO_BASE + offset, value, sc6530_usb_guest_pc());
}

static const MemoryRegionOps sc6530_usb_fifo_ops = {
    .read  = sc6530_usb_fifo_read,
    .write = sc6530_usb_fifo_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 1, .max_access_size = 4 },
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

/* ---------------------------------------------------------------------- */
/* SysBus device                                                          */
/* ---------------------------------------------------------------------- */

static void sc6530_usb_reset(DeviceState *dev)
{
    Sc6530UsbState *s = SC6530_USB(dev);

    memset(s->usb_regs, 0, sizeof(s->usb_regs));
    memset(s->fifo_regs, 0, sizeof(s->fifo_regs));
}

static void sc6530_usb_init(Object *obj)
{
    Sc6530UsbState *s = SC6530_USB(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->usb_iomem, obj, &sc6530_usb_ops, s,
                          "sc6530-usb", SC6530_USB_SIZE);
    sysbus_init_mmio(sbd, &s->usb_iomem);

    memory_region_init_io(&s->fifo_iomem, obj, &sc6530_usb_fifo_ops, s,
                          "sc6530-usb-fifo", SC6530_USB_FIFO_SIZE);
    sysbus_init_mmio(sbd, &s->fifo_iomem);
}

static void sc6530_usb_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, sc6530_usb_reset);
}

static const TypeInfo sc6530_usb_info = {
    .name          = TYPE_SC6530_USB,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Sc6530UsbState),
    .instance_init = sc6530_usb_init,
    .class_init    = sc6530_usb_class_init,
};

static void sc6530_usb_register_types(void)
{
    type_register_static(&sc6530_usb_info);
}

type_init(sc6530_usb_register_types)
