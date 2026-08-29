/*
 * Spreadtrum SC6530C interrupt controller (B310E-OS QEMU machine).
 *
 * Models the SC6530 INTC at 0x80000000: 32 level-sensitive input lines,
 * one output SysBus IRQ wired directly to the ARM926 CPU's IRQ line in
 * hw/arm/b310e.c (no GIC). Register semantics are pinned by the guest-side
 * usage in the B310E-OS kernel (kernel/irq.c) and the hardware learnings
 * (.omo/notepads/b310e-custom-os/learnings.md):
 *
 *   +0x04 PENDING (RO): level of all 32 input lines, INCLUDING lines whose
 *         mask bit is clear. This is the documented hardware quirk behind
 *         the "1ms tick + key activity" hard freeze on the real phone: an
 *         unmasked peripheral line asserts PENDING the moment its source
 *         asserts, so the guest IRQ handler must skip lines that have no
 *         registered C handler (kernel/irq.c irq_dispatch_for_test).
 *         There is NO pending-clear register: the peripheral ISR
 *         deasserts its own source line (the timer writes its +0xc = 9),
 *         which drops the pending bit here.
 *   +0x08 ENABLE (WO): full-register write of the interrupt mask. The
 *         guest arms the 1ms timer line with MEM4(0x80000008) = 1u << 23
 *         and masks everything with MEM4(0x80000008) = 0 (kernel/irq.c
 *         sys_timer_start / sys_timer_pause) - a later write of 0 clears.
 *   +0x0c DISABLE (WO): write 1<<j CLEARS mask bit j. The learnings'
 *         "INT_CLEAR" trap: this register is INT_DISABLE, not an
 *         acknowledge - a guest writing it masks the line; PENDING is
 *         untouched (the peripheral ISR owns source-clearing).
 *
 * CPU IRQ output = (pending & enabled) != 0, level-sensitive.
 *
 * Template: hw/arm/musicpal.c mv88w8618_pic (the surviving ARM926EJ-S +
 * custom-PIC pattern in QEMU v11.1.0). Key divergence: the SC6530 PENDING
 * read returns the raw line levels (musicpal masks them into its STATUS),
 * and the DISABLE register does not clear the pending level.
 *
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
    qemu_irq parent_irq;
};

static void sc6530_intc_update(Sc6530IntcState *s)
{
    qemu_set_irq(s->parent_irq, (s->level & s->enabled) != 0);
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
    case SC6530_INTC_PENDING:
        /* Raw line levels, INCLUDING lines that are masked: the SC6530
         * INTC pending reflects disabled lines too (the learnings root
         * cause - fpdoom checks pending bit 25 for USB without ever
         * enabling that line). */
        return s->level;
    default:
        /* ENABLE/DISABLE are write-only on the real part. */
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
}

static void sc6530_intc_init(Object *obj)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    Sc6530IntcState *s = SC6530_INTC(obj);

    qdev_init_gpio_in(DEVICE(s), sc6530_intc_set_irq, 32);
    sysbus_init_irq(sbd, &s->parent_irq);
    memory_region_init_io(&s->iomem, obj, &sc6530_intc_ops, s,
                          "sc6530-intc", SC6530_INTC_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
}

static const VMStateDescription sc6530_intc_vmsd = {
    .name = "sc6530_intc",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(level, Sc6530IntcState),
        VMSTATE_UINT32(enabled, Sc6530IntcState),
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
