/*
 * Spreadtrum SC6530C Serial Flash Controller (SFC)
 *
 * Base: 0x20A00000, Size: 0x1000
 * Models the stock driver's software transactions, status polling and
 * writes to the private NOR backing store.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/sysbus.h"
#include "hw/core/cpu.h"
#include "target/arm/cpu.h"
#include "hw/core/qdev-properties.h"
#include "qom/object.h"
#include "system/memory.h"
#include "system/address-spaces.h"
#include "migration/vmstate.h"
#include "trace.h"

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
    uint32_t jedec_id;
    uint8_t status1;
    uint8_t status2;
    bool reset_enabled;
};

static uint64_t sc6530_sfc_read(void *opaque, hwaddr offset, unsigned size)
{
    Sc6530SfcState *s = SC6530_SFC(opaque);

    /* Stock 0x04010784 fills TX words a byte at a time. */
    if (size < 4 || (offset & 3)) {
        uint32_t word = sc6530_sfc_read(opaque, offset & ~3, 4);
        uint32_t mask = size == 4 ? UINT32_MAX : (1u << (size * 8)) - 1;
        return (word >> ((offset & 3) * 8)) & mask;
    }

    switch (offset) {
    case 0x00: return s->cmd_cfg;
    case 0x04: return s->soft_req;
    case 0x08: return s->tbuf_clr;
    case 0x0C: return s->int_clr;
    case 0x10: {
        /* SFC_STATUS: benign = 0x3 (ready|idle) - docs/b310e-qemu.md.
         * Returning 0x0 (busy) makes the BML's soft_req flow spin and
         * assert AST_BLUESCREEN (verified empirically: 0x3 -> boot
         * advances past the init.c:225 partition-check assert). */
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
            return s->cmd_buf[idx];
        }
        break;
    case 0x70 ... 0x78:
        if ((offset - 0x70) % 4 == 0) {
            int idx = (offset - 0x70) / 4;
            return s->type_buf[idx];
        }
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "sc6530_sfc: read at bad offset 0x%x\n", (int)offset);
        return 0;
    }
    return 0;
}

static uint64_t sc6530_sfc_guest_pc(void)
{
    CPUState *cs = current_cpu;

    if (cs) {
        return ARM_CPU(cs)->env.regs[15];
    }
    return 0;
}

static unsigned sc6530_sfc_bytes(Sc6530SfcState *s, unsigned slot)
{
    uint8_t type = s->type_buf[slot / 4] >> ((slot % 4) * 8);

    return (type & 1) && !(type & 0x20) ? ((type >> 3) & 3) + 1 : 0;
}

static void sc6530_sfc_program(Sc6530SfcState *s, uint32_t addr)
{
    uint8_t *nor;
    uint64_t size;
    uint32_t first = addr;

    if (!s->nor_mr || !(s->status1 & 2)) {
        return;
    }
    nor = memory_region_get_ram_ptr(s->nor_mr);
    size = memory_region_size(s->nor_mr);
    for (unsigned slot = 2; slot < 12; slot++) {
        unsigned count = sc6530_sfc_bytes(s, slot);
        for (unsigned byte = 0; byte < count; byte++) {
            /* Stock 0x04012c7e sends a native halfword without swapping;
             * its 0x55aa marker must read back as 0x55aa through XIP. */
            uint8_t value = s->cmd_buf[slot] >> (byte * 8);
            uint32_t target = (first & ~255u) | (addr++ & 255u);
            if (target < size) {
                nor[target] &= value;
            }
        }
    }
    memory_region_flush_rom_device(s->nor_mr, first & ~255u, 256);
    s->status1 &= ~2u;
}

static void sc6530_sfc_erase(Sc6530SfcState *s, uint32_t addr, unsigned length)
{
    uint64_t size;

    if (!s->nor_mr || !(s->status1 & 2)) {
        return;
    }
    size = memory_region_size(s->nor_mr);
    addr &= ~(length - 1);
    if (addr < size) {
        length = MIN(length, size - addr);
        memset((uint8_t *)memory_region_get_ram_ptr(s->nor_mr) + addr,
               0xff, length);
        memory_region_flush_rom_device(s->nor_mr, addr, length);
    }
    s->status1 &= ~2u;
}

static void sc6530_sfc_trigger(Sc6530SfcState *s)
{
    uint32_t opcode = s->cmd_buf[0] & 0xFF;
    int resp_slot = 7 - ((s->cmd_cfg >> 3) & 3);
    uint32_t pc = sc6530_sfc_guest_pc();

    if (resp_slot < 0 || resp_slot > 11) {
        resp_slot = 0;
    }

    trace_sc6530_sfc_command(opcode, s->cmd_buf[1], s->cmd_cfg, pc);

    switch (opcode) {
    case 0x9F:
        /* The serial receive shifter places the first byte in bits 31:24.
         * Stock 0x0401211c extracts manufacturer/type/capacity at 24/16/8. */
        s->cmd_buf[resp_slot] = s->jedec_id << 8;
        break;
    case 0x05:
        s->cmd_buf[resp_slot] = (uint32_t)s->status1 << 24;
        break;
    case 0x35:
        s->cmd_buf[resp_slot] = (uint32_t)s->status2 << 24;
        break;
    case 0x06:
        s->status1 |= 2; /* WEL, polled at stock 0x04011030. */
        break;
    case 0x04:
        s->status1 &= ~2u;
        break;
    case 0x01:
        if (s->status1 & 2) {
            s->status1 = s->cmd_buf[1] & ~3u;
            s->status2 = s->cmd_buf[2];
        }
        break;
    case 0x31:
        if (s->status1 & 2) {
            s->status2 = s->cmd_buf[1];
            s->status1 &= ~2u;
        }
        break;
    case 0x66:
        s->reset_enabled = true;
        break;
    case 0x99:
        if (s->reset_enabled) {
            s->status1 = s->status2 = 0;
            s->reset_enabled = false;
        }
        break;
    case 0x02:
    case 0x32:
        sc6530_sfc_program(s, s->cmd_buf[1] & 0xffffff);
        break;
    case 0x20:
        sc6530_sfc_erase(s, s->cmd_buf[1] & 0xffffff, 4096);
        break;
    case 0xd8:
        sc6530_sfc_erase(s, s->cmd_buf[1] & 0xffffff, 65536);
        break;
    case 0x03:
    case 0x0B:
    case 0x0C: {
        uint32_t addr = (s->cmd_buf[0] >> 8) & 0xFFFFFF;
        uint8_t buf[4] = { 0 };

        if (addr == 0 && s->cmd_buf[1] != 0) {
            addr = s->cmd_buf[1] & 0xFFFFFF;
        }
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

    if (size < 4 || (offset & 3)) {
        unsigned shift = (offset & 3) * 8;
        uint32_t mask = ((1u << (size * 8)) - 1) << shift;
        uint32_t old = sc6530_sfc_read(opaque, offset & ~3, 4);
        sc6530_sfc_write(opaque, offset & ~3,
                         (old & ~mask) | ((value << shift) & mask), 4);
        return;
    }

    switch (offset) {
    case 0x00:
        s->cmd_cfg = value;
        break;
    case 0x04:
        s->soft_req = value;
        if (value & 1) {
            sc6530_sfc_trigger(s);
            s->soft_req &= ~1u;
        }
        break;
    case 0x08:
        /* Stock 0x0400ff46 starts each transaction with TBUF_CLR bit 0.
         * Drop the old slot enables, or a short program also transmits
         * stale slots from a previous, longer program. */
        if (value & 1) {
            for (unsigned i = 0; i < 3; i++) {
                s->type_buf[i] &= ~0x01010101u;
            }
            s->tbuf_clr = value & ~1u;
        }
        break;
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
        }
        break;
    case 0x70 ... 0x78:
        if ((offset - 0x70) % 4 == 0) {
            int idx = (offset - 0x70) / 4;
            s->type_buf[idx] = value;
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
    s->status1 = s->status2 = 0;
    s->reset_enabled = false;
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
    /* The dump's TAIL/MAGH descriptor at 0x10320 specifies C8 60 17. */
    DEFINE_PROP_UINT32("jedec-id", Sc6530SfcState, jedec_id, 0xc86017),
    DEFINE_PROP_LINK("nor", Sc6530SfcState, nor_mr,
                     TYPE_MEMORY_REGION, MemoryRegion *),
};

static const VMStateDescription sc6530_sfc_vmsd = {
    .name = "sc6530_sfc",
    .version_id = 2,
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
        VMSTATE_UINT8_V(status1, Sc6530SfcState, 2),
        VMSTATE_UINT8_V(status2, Sc6530SfcState, 2),
        VMSTATE_BOOL_V(reset_enabled, Sc6530SfcState, 2),
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
