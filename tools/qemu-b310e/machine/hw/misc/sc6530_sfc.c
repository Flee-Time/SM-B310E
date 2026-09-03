/*
 * Spreadtrum SC6530C Serial Flash Controller (SFC)
 *
 * Base: 0x20A00000, Size: 0x1000
 * Models the SFC minimal behavior to unblock BML flash init.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "qom/object.h"
#include "system/memory.h"
#include "system/address-spaces.h"
#include "migration/vmstate.h"

#define TYPE_SC6530_SFC "sc6530_sfc"
OBJECT_DECLARE_SIMPLE_TYPE(Sc6530SfcState, SC6530_SFC)

#define SC6530_SFC_SIZE 0x1000

struct Sc6530SfcState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    MemoryRegion *nor_mr; /* QOM link to the machine's NOR region */

    uint32_t cmd_cfg;
    uint32_t soft_req;
    uint32_t tbuf_clr;
    uint32_t int_clr;
    uint32_t cs_timing_cfg;
    uint32_t rd_sample_cfg;
    uint32_t clk_cfg;
    uint32_t cs_cfg;
    uint32_t endian_cfg;
    uint32_t io_dly_cfg;
    uint32_t wp_hld_init;

    uint32_t cmd_buf[12]; /* 0x40 - 0x6C */
    uint32_t type_buf[3]; /* 0x70 - 0x78 */
};

static uint64_t sc6530_sfc_read(void *opaque, hwaddr offset, unsigned size)
{
    Sc6530SfcState *s = SC6530_SFC(opaque);

    switch (offset) {
    case 0x00: qemu_log("sc6530_sfc: r cmd_cfg=0x%08x\n", s->cmd_cfg); return s->cmd_cfg;
    case 0x04: qemu_log("sc6530_sfc: r soft_req=0x%08x\n", s->soft_req); return s->soft_req;
    case 0x08: return s->tbuf_clr;
    case 0x0C: return s->int_clr;
    case 0x10: {
        /* SFC_STATUS: benign = 0x3 (ready|idle) - docs/b310e-qemu.md.
         * Returning 0x0 (busy) makes the BML's soft_req flow spin and
         * assert AST_BLUESCREEN (verified empirically: 0x3 -> boot
         * advances past the init.c:225 partition-check assert). */
        qemu_log("sc6530_sfc: r status=0x3\n");
        return 0x3;
    }
    case 0x14: return s->cs_timing_cfg;
    case 0x18: return s->rd_sample_cfg;
    case 0x1C: return s->clk_cfg;
    case 0x20: return s->cs_cfg;
    case 0x24: return s->endian_cfg;
    case 0x28: return s->io_dly_cfg;
    case 0x2C: return s->wp_hld_init;
    case 0x40 ... 0x6C:
        if ((offset - 0x40) % 4 == 0) {
            int idx = (offset - 0x40) / 4;
            qemu_log("sc6530_sfc: r cmd_buf[%d]=0x%08x\n", idx, s->cmd_buf[idx]);
            return s->cmd_buf[idx];
        }
        break;
    case 0x70 ... 0x78:
        if ((offset - 0x70) % 4 == 0) {
            int idx = (offset - 0x70) / 4;
            qemu_log("sc6530_sfc: r type_buf[%d]=0x%08x\n", idx, s->type_buf[idx]);
            return s->type_buf[idx];
        }
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "sc6530_sfc: read at bad offset 0x%x\n", (int)offset);
        return 0;
    }
    return 0;
}

static void sc6530_sfc_trigger(Sc6530SfcState *s)
{
    uint32_t opcode = s->cmd_buf[0] & 0xFF;
    int resp_slot = 7 - ((s->cmd_cfg >> 3) & 3);

    if (resp_slot < 0 || resp_slot > 11) {
        resp_slot = 0;
    }

    qemu_log("sc6530_sfc: TRIGGER opcode=0x%02x resp_slot=%d cmd_cfg=0x%08x\n",
             opcode, resp_slot, s->cmd_cfg);

    switch (opcode) {
    case 0x9F:
        s->cmd_buf[resp_slot] = 0xEF4017;
        qemu_log("sc6530_sfc: JEDEC ID -> cmd_buf[%d]=0xEF4017\n", resp_slot);
        break;
    case 0x05:
        s->cmd_buf[resp_slot] = 0x00;
        qemu_log("sc6530_sfc: READ STATUS -> cmd_buf[%d]=0x00\n", resp_slot);
        break;
    case 0x35:
        s->cmd_buf[resp_slot] = 0x00;
        qemu_log("sc6530_sfc: READ STATUS 2 -> cmd_buf[%d]=0x00\n", resp_slot);
        break;
    case 0x03:
    case 0x0B:
    case 0x0C: {
        uint32_t addr = (s->cmd_buf[0] >> 8) & 0xFFFFFF;
        uint8_t buf[4] = { 0 };

        if (addr == 0 && s->cmd_buf[1] != 0) {
            addr = s->cmd_buf[1] & 0xFFFFFF;
        }
        qemu_log("sc6530_sfc: SPI READ 0x%02x @0x%06x -> cmd_buf[%d]\n",
                 opcode, addr, resp_slot);
        if (address_space_read(&address_space_memory, addr,
                               MEMTXATTRS_UNSPECIFIED, buf, 4) == MEMTX_OK) {
            s->cmd_buf[resp_slot] = buf[0] | (buf[1] << 8) |
                                    (buf[2] << 16) | (buf[3] << 24);
        } else {
            s->cmd_buf[resp_slot] = 0;
        }
        break;
    }
    default:
        break;
    }
}

static void sc6530_sfc_write(void *opaque, hwaddr offset,
                             uint64_t value, unsigned size)
{
    Sc6530SfcState *s = SC6530_SFC(opaque);

    switch (offset) {
    case 0x00:
        s->cmd_cfg = value;
        qemu_log("sc6530_sfc: cmd_cfg write 0x%08x\n", (uint32_t)value);
        sc6530_sfc_trigger(s);
        break;
    case 0x04:
        s->soft_req = value;
        qemu_log("sc6530_sfc: soft_req write 0x%08x\n", (uint32_t)value);
        sc6530_sfc_trigger(s);
        break;
    case 0x08: s->tbuf_clr = value; break;
    case 0x0C: s->int_clr = value; break;
    case 0x14: s->cs_timing_cfg = value; break;
    case 0x18: s->rd_sample_cfg = value; break;
    case 0x1C: s->clk_cfg = value; break;
    case 0x20: s->cs_cfg = value; break;
    case 0x24: s->endian_cfg = value; break;
    case 0x28: s->io_dly_cfg = value; break;
    case 0x2C: s->wp_hld_init = value; break;
    case 0x40 ... 0x6C:
        if ((offset - 0x40) % 4 == 0) {
            int idx = (offset - 0x40) / 4;
            s->cmd_buf[idx] = value;
            qemu_log("sc6530_sfc: cmd_buf[%d] write 0x%08x\n", idx, (uint32_t)value);
            /* The guest's driver starts the flash command when the opcode
             * lands in cmd_buf[0] (no soft_req follows for READ commands in
             * the observed trace - the JEDEC's soft_req is the only one).
             * Execute immediately so the response is ready when the guest
             * polls type_buf. */
            if (idx == 0 && (value & 0xff) != 0) {
                sc6530_sfc_trigger(s);
            }
        }
        break;
    case 0x70 ... 0x78:
        if ((offset - 0x70) % 4 == 0) {
            s->type_buf[(offset - 0x70) / 4] = value;
        }
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "sc6530_sfc: write at bad offset 0x%x\n", (int)offset);
        break;
    }
}

static const MemoryRegionOps sc6530_sfc_ops = {
    .read = sc6530_sfc_read,
    .write = sc6530_sfc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static void sc6530_sfc_reset(DeviceState *dev)
{
    Sc6530SfcState *s = SC6530_SFC(dev);

    s->cmd_cfg = 0;
    s->soft_req = 0;
    s->tbuf_clr = 0;
    s->int_clr = 0;
    s->cs_timing_cfg = 0;
    s->rd_sample_cfg = 0;
    s->clk_cfg = 0;
    s->cs_cfg = 0;
    s->endian_cfg = 0;
    s->io_dly_cfg = 0;
    s->wp_hld_init = 0;
    
    int i;
    for (i = 0; i < 12; i++) {
        s->cmd_buf[i] = 0;
    }
    for (i = 0; i < 3; i++) {
        s->type_buf[i] = 0;
    }
}

static void sc6530_sfc_init(Object *obj)
{
    Sc6530SfcState *s = SC6530_SFC(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &sc6530_sfc_ops, s,
                          "sc6530-sfc", SC6530_SFC_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
}

static const Property sc6530_sfc_properties[] = {
    DEFINE_PROP_LINK("nor", Sc6530SfcState, nor_mr,
                     TYPE_MEMORY_REGION, MemoryRegion *),
};

static const VMStateDescription sc6530_sfc_vmsd = {
    .name = "sc6530_sfc",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(cmd_cfg, Sc6530SfcState),
        VMSTATE_UINT32(soft_req, Sc6530SfcState),
        VMSTATE_UINT32(tbuf_clr, Sc6530SfcState),
        VMSTATE_UINT32(int_clr, Sc6530SfcState),
        VMSTATE_UINT32(cs_timing_cfg, Sc6530SfcState),
        VMSTATE_UINT32(rd_sample_cfg, Sc6530SfcState),
        VMSTATE_UINT32(clk_cfg, Sc6530SfcState),
        VMSTATE_UINT32(cs_cfg, Sc6530SfcState),
        VMSTATE_UINT32(endian_cfg, Sc6530SfcState),
        VMSTATE_UINT32(io_dly_cfg, Sc6530SfcState),
        VMSTATE_UINT32(wp_hld_init, Sc6530SfcState),
        VMSTATE_UINT32_ARRAY(cmd_buf, Sc6530SfcState, 12),
        VMSTATE_UINT32_ARRAY(type_buf, Sc6530SfcState, 3),
        VMSTATE_END_OF_LIST()
    }
};

static void sc6530_sfc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, sc6530_sfc_reset);
    device_class_set_props(dc, sc6530_sfc_properties);
    dc->vmsd = &sc6530_sfc_vmsd;
}

static const TypeInfo sc6530_sfc_info = {
    .name          = TYPE_SC6530_SFC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Sc6530SfcState),
    .instance_init = sc6530_sfc_init,
    .class_init    = sc6530_sfc_class_init,
};

static void sc6530_sfc_register_types(void)
{
    type_register_static(&sc6530_sfc_info);
}

type_init(sc6530_sfc_register_types)
