/*
 * SC6530C interrupt controller at 0x80000000, 32 level inputs.
 * IRQ: masked status +0, raw +4, enable R/W +8, disable strobe +12.
 * FIQ: the corresponding bank is at +0x20. Stock 0x1b920 preserves the
 * enable mask with a read/OR/write, 0x1ba20 routes sources to FIQ.
 * Each peripheral acknowledges and deasserts its own source.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"
#include "qemu/module.h"

#define TYPE_SC6530_INTC "sc6530_intc"
OBJECT_DECLARE_SIMPLE_TYPE(Sc6530IntcState, SC6530_INTC)

/* Register offsets from the SC6530 INTC base 0x80000000. */
#define SC6530_INTC_PENDING  0x04
#define SC6530_INTC_STATUS   0x00
#define SC6530_INTC_ENABLE   0x08
#define SC6530_INTC_DISABLE  0x0c

/* Region size: 4 KiB covers the register bank; nothing else lives at
 * 0x8000xxxx (the next peripheral region is the timer at 0x81000000). */
#define SC6530_INTC_SIZE     0x1000

struct Sc6530IntcState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/

    MemoryRegion iomem;
    uint32_t level;    /* raw input levels, one bit per line (PENDING) */
    uint32_t enabled;  /* interrupt mask (INT_ENABLE) */
    uint32_t fiq_enabled;
    qemu_irq parent_irq;
    qemu_irq parent_fiq;
};

static void sc6530_intc_update(Sc6530IntcState *s)
{
    qemu_set_irq(s->parent_irq, (s->level & s->enabled) != 0);
    qemu_set_irq(s->parent_fiq, (s->level & s->fiq_enabled) != 0);
}

static void sc6530_intc_set_irq(void *opaque, int irq, int level)
{
    Sc6530IntcState *s = opaque;

    if (level) {
        s->level |= 1u << irq;
    } else {
        s->level &= ~(1u << irq);
    }
    sc6530_intc_update(s);
}

static uint64_t sc6530_intc_read(void *opaque, hwaddr offset, unsigned size)
{
    Sc6530IntcState *s = opaque;

    switch (offset) {
    case SC6530_INTC_STATUS:
        /* Stock 0x1b874 reads the masked status to dispatch an ISR. */
        return s->level & s->enabled;
    case SC6530_INTC_PENDING:
        /* Raw line levels, INCLUDING lines that are masked: the SC6530
         * INTC pending reflects disabled lines too (the learnings root
         * cause - fpdoom checks pending bit 25 for USB without ever
         * enabling that line). */
        return s->level;
    case SC6530_INTC_ENABLE:
        /* Stock 0x1b920 preserves existing enables with a read/OR/write. */
        return s->enabled;
    case 0x20:
        return s->level & s->fiq_enabled;
    case 0x24:
        return s->level;
    case 0x28:
        return s->fiq_enabled;
    default:
        return 0;
    }
}

static void sc6530_intc_write(void *opaque, hwaddr offset,
                              uint64_t value, unsigned size)
{
    Sc6530IntcState *s = opaque;

    switch (offset) {
    case SC6530_INTC_ENABLE:
        /* Full-register write of the mask: sys_timer_start does
         * MEM4(INT_ENABLE) = 1u << 23 and sys_timer_pause does
         * MEM4(INT_ENABLE) = 0 (kernel/irq.c), so a later write of 0
         * must clear every bit. */
        s->enabled = (uint32_t)value;
        break;
    case SC6530_INTC_DISABLE:
        /* 1<<j clears mask bit j. The learnings' "INT_CLEAR" trap: this
         * register is INT_DISABLE, not an acknowledge - PENDING is left
         * untouched (the peripheral ISR clears its own source). */
        s->enabled &= ~(uint32_t)value;
        break;
    case 0x28:
        s->fiq_enabled = value;
        break;
    case 0x2c:
        s->fiq_enabled &= ~value;
        break;
    default:
        break;
    }
    sc6530_intc_update(s);
}

static const MemoryRegionOps sc6530_intc_ops = {
    .read = sc6530_intc_read,
    .write = sc6530_intc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void sc6530_intc_reset(DeviceState *d)
{
    Sc6530IntcState *s = SC6530_INTC(d);

    s->level = 0;
    s->enabled = 0;
    s->fiq_enabled = 0;
    sc6530_intc_update(s);
}

static void sc6530_intc_init(Object *obj)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    Sc6530IntcState *s = SC6530_INTC(obj);

    qdev_init_gpio_in(DEVICE(s), sc6530_intc_set_irq, 32);
    sysbus_init_irq(sbd, &s->parent_irq);
    sysbus_init_irq(sbd, &s->parent_fiq);
    memory_region_init_io(&s->iomem, obj, &sc6530_intc_ops, s,
                          "sc6530-intc", SC6530_INTC_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
}

static const VMStateDescription sc6530_intc_vmsd = {
    .name = "sc6530_intc",
    .version_id = 2,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(level, Sc6530IntcState),
        VMSTATE_UINT32(enabled, Sc6530IntcState),
        VMSTATE_UINT32_V(fiq_enabled, Sc6530IntcState, 2),
        VMSTATE_END_OF_LIST()
    }
};

static void sc6530_intc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "Spreadtrum SC6530 interrupt controller";
    device_class_set_legacy_reset(dc, sc6530_intc_reset);
    dc->vmsd = &sc6530_intc_vmsd;
}

static const TypeInfo sc6530_intc_info = {
    .name = TYPE_SC6530_INTC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Sc6530IntcState),
    .instance_init = sc6530_intc_init,
    .class_init = sc6530_intc_class_init,
};

static void sc6530_intc_register_types(void)
{
    type_register_static(&sc6530_intc_info);
}

type_init(sc6530_intc_register_types)
