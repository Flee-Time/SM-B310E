/*
 * SC6530C timer0/1: stock 0x37b78..0x37d22, register table at 0x0422e3b8.
 * Low-frequency timers use 32768 Hz and IRQs 4/5. Timer2 is separate.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/core/ptimer.h"
#include "migration/vmstate.h"
#include "trace.h"

#define TYPE_SC6530_GPT "sc6530_gpt"
OBJECT_DECLARE_SIMPLE_TYPE(Sc6530GptState, SC6530_GPT)

typedef struct Sc6530GptChannel {
    ptimer_state *timer;
    qemu_irq irq;
    uint32_t load;
    uint32_t control;
    bool enabled;
    bool pending;
} Sc6530GptChannel;

struct Sc6530GptState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    Sc6530GptChannel channel[2];
};

static void sc6530_gpt_irq(Sc6530GptChannel *c)
{
    qemu_set_irq(c->irq, c->pending && c->enabled);
}

static void sc6530_gpt_tick(void *opaque)
{
    Sc6530GptChannel *c = opaque;
    c->pending = true;
    sc6530_gpt_irq(c);
}

static void sc6530_gpt_run(Sc6530GptChannel *c)
{
    ptimer_transaction_begin(c->timer);
    ptimer_stop(c->timer);
    ptimer_set_limit(c->timer, MAX(c->load, 1u), 1);
    ptimer_set_freq(c->timer, 32768);
    if (c->control & 0x80) {
        ptimer_run(c->timer, !(c->control & 0x40));
    }
    ptimer_transaction_commit(c->timer);
}

static uint64_t sc6530_gpt_read(void *opaque, hwaddr offset, unsigned size)
{
    Sc6530GptState *s = opaque;
    Sc6530GptChannel *c = &s->channel[offset / 0x20];
    switch (offset & 0x1f) {
    case 0: return c->load;
    case 4:
    case 0x10: return ptimer_get_count(c->timer);
    case 8: return c->control;
    case 12: return c->enabled | (c->pending ? 6 : 0);
    default: return 0;
    }
}

static void sc6530_gpt_write(void *opaque, hwaddr offset,
                             uint64_t value, unsigned size)
{
    Sc6530GptState *s = opaque;
    Sc6530GptChannel *c = &s->channel[offset / 0x20];
    trace_sc6530_gpt_write(offset, value);
    switch (offset & 0x1f) {
    case 0:
        c->load = value;
        sc6530_gpt_run(c);
        break;
    case 8:
        /* Stock stops a timer before reading its remaining count. A stop
         * must freeze that count, not reload LOAD and lose elapsed time. */
        if (!(value & 0x80)) {
            ptimer_transaction_begin(c->timer);
            ptimer_stop(c->timer);
            ptimer_transaction_commit(c->timer);
        } else if (!(c->control & 0x80)) {
            c->control = value;
            sc6530_gpt_run(c);
        } else if ((value ^ c->control) & 0x40) {
            ptimer_transaction_begin(c->timer);
            ptimer_run(c->timer, !(value & 0x40));
            ptimer_transaction_commit(c->timer);
        }
        c->control = value;
        break;
    case 12:
        c->enabled = value & 1;
        if (value & 8) {
            c->pending = false;
        }
        sc6530_gpt_irq(c);
        break;
    }
}

static const MemoryRegionOps sc6530_gpt_ops = {
    .read = sc6530_gpt_read,
    .write = sc6530_gpt_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void sc6530_gpt_reset(DeviceState *dev)
{
    Sc6530GptState *s = SC6530_GPT(dev);
    for (unsigned i = 0; i < 2; i++) {
        Sc6530GptChannel *c = &s->channel[i];
        c->load = c->control = 0;
        c->enabled = c->pending = false;
        sc6530_gpt_run(c);
        sc6530_gpt_irq(c);
    }
}

static void sc6530_gpt_init(Object *obj)
{
    Sc6530GptState *s = SC6530_GPT(obj);
    for (unsigned i = 0; i < 2; i++) {
        Sc6530GptChannel *c = &s->channel[i];
        sysbus_init_irq(SYS_BUS_DEVICE(s), &c->irq);
        c->timer = ptimer_init(sc6530_gpt_tick, c, PTIMER_POLICY_LEGACY);
    }
    memory_region_init_io(&s->iomem, obj, &sc6530_gpt_ops, s,
                          "sc6530-timer01", 0x40);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
}

static void sc6530_gpt_finalize(Object *obj)
{
    Sc6530GptState *s = SC6530_GPT(obj);
    for (unsigned i = 0; i < 2; i++) {
        ptimer_free(s->channel[i].timer);
    }
}

static const VMStateDescription sc6530_gpt_channel_vmsd = {
    .name = "sc6530-gpt/channel",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_PTIMER(timer, Sc6530GptChannel),
        VMSTATE_UINT32(load, Sc6530GptChannel),
        VMSTATE_UINT32(control, Sc6530GptChannel),
        VMSTATE_BOOL(enabled, Sc6530GptChannel),
        VMSTATE_BOOL(pending, Sc6530GptChannel),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription sc6530_gpt_vmsd = {
    .name = "sc6530-gpt",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_ARRAY(channel, Sc6530GptState, 2, 1,
                             sc6530_gpt_channel_vmsd, Sc6530GptChannel),
        VMSTATE_END_OF_LIST()
    }
};

static void sc6530_gpt_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    device_class_set_legacy_reset(dc, sc6530_gpt_reset);
    dc->vmsd = &sc6530_gpt_vmsd;
}

static const TypeInfo sc6530_gpt_info = {
    .name = TYPE_SC6530_GPT,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Sc6530GptState),
    .instance_init = sc6530_gpt_init,
    .instance_finalize = sc6530_gpt_finalize,
    .class_init = sc6530_gpt_class_init,
};

static void sc6530_gpt_register_types(void)
{
    type_register_static(&sc6530_gpt_info);
}
type_init(sc6530_gpt_register_types)
