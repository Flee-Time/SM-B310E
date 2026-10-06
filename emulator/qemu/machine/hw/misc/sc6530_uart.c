/*
 * Spreadtrum SC6530 UART1 debug console
 *
 * Ground truth (Spreadtrum ARM_UART1, vendor-SDK verified):
 *
 * Region: 0x84000000, size 0x100
 *
 * Offset | Name              | Behavior to model
 * 0x00   | ARM_UART_TXD      | write byte -> TX (see LOGGING)
 * 0x04   | ARM_UART_RXD      | read -> 0 (no input)
 * 0x08   | ARM_UART_STS0     | read -> US_TX_EMPTY = 0x0002 (bit 1 set; TX always empty)
 * 0x0C   | ARM_UART_STS1     | read -> 0 (bits 8-15 = US1_TX_FIFOCNT = 0, TX fifo never full)
 * 0x10   | ARM_UART_IEN      | store+echo
 * 0x14   | ARM_UART_ICLR     | store+echo
 * 0x18   | ARM_UART_CTL0     | store+echo
 * 0x1C   | ARM_UART_CTL1     | store+echo
 * 0x20   | ARM_UART_CTL2     | store+echo
 * 0x24   | ARM_UART_CLKD0    | store+echo
 * 0x28   | ARM_UART_CLKD1    | store+echo
 * 0x2C   | ARM_UART_STS2     | store+echo
 * 0x30   | ARM_UART_DSP_WAIT | store+echo
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/sysbus.h"
#include "hw/core/cpu.h"
#include "target/arm/cpu.h"
#include "migration/vmstate.h"
#include "trace.h"

#define TYPE_SC6530_UART "sc6530_uart"
OBJECT_DECLARE_SIMPLE_TYPE(Sc6530UartState, SC6530_UART)

#define SC6530_UART_SIZE 0x100

struct Sc6530UartState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;

    uint32_t ien;
    uint32_t iclr;
    uint32_t ctl0;
    uint32_t ctl1;
    uint32_t ctl2;
    uint32_t clkd0;
    uint32_t clkd1;
    uint32_t sts2;
    uint32_t dsp_wait;

    uint8_t line_buf[128];
    int line_len;
};

static uint32_t sc6530_uart_guest_pc(void)
{
    CPUState *cs = current_cpu;

    if (cs) {
        return ARM_CPU(cs)->env.regs[15];
    }
    return 0;
}

static uint64_t sc6530_uart_read(void *opaque, hwaddr offset, unsigned size)
{
    Sc6530UartState *s = SC6530_UART(opaque);

    switch (offset) {
    case 0x04: /* ARM_UART_RXD */
        return 0;
    case 0x08: /* ARM_UART_STS0 */
        return 0x0002; /* US_TX_EMPTY (bit 1) */
    case 0x0C: /* ARM_UART_STS1 */
        return 0;      /* US1_TX_FIFOCNT = 0 (bits 8-15) */
    case 0x10: /* ARM_UART_IEN */
        return s->ien;
    case 0x14: /* ARM_UART_ICLR */
        return s->iclr;
    case 0x18: /* ARM_UART_CTL0 */
        return s->ctl0;
    case 0x1C: /* ARM_UART_CTL1 */
        return s->ctl1;
    case 0x20: /* ARM_UART_CTL2 */
        return s->ctl2;
    case 0x24: /* ARM_UART_CLKD0 */
        return s->clkd0;
    case 0x28: /* ARM_UART_CLKD1 */
        return s->clkd1;
    case 0x2C: /* ARM_UART_STS2 */
        return s->sts2;
    case 0x30: /* ARM_UART_DSP_WAIT */
        return s->dsp_wait;
    default:
        return 0;
    }
}

static void sc6530_uart_flush_line(Sc6530UartState *s, uint32_t pc)
{
    if (s->line_len > 0) {
        GString *str = g_string_new(NULL);
        int i;
        for (i = 0; i < s->line_len; i++) {
            uint8_t ch = s->line_buf[i];
            if (ch >= 32 && ch <= 126 && ch != '\\' && ch != '"') {
                g_string_append_c(str, ch);
            } else {
                g_string_append_printf(str, "\\x%02x", ch);
            }
        }
        qemu_log("sc6530_uart: TX line \"%s\" pc=0x%08x\n", str->str, pc);
        g_string_free(str, TRUE);
        s->line_len = 0;
    }
}

static void sc6530_uart_write(void *opaque, hwaddr offset,
                              uint64_t value, unsigned size)
{
    Sc6530UartState *s = SC6530_UART(opaque);
    uint32_t pc;
    uint8_t byte;

    switch (offset) {
    case 0x00: /* ARM_UART_TXD */
        byte = value & 0xff;
        pc = sc6530_uart_guest_pc();

        if (byte >= 32 && byte <= 126) {
            qemu_log("sc6530_uart: TX byte=0x%02x '%c' pc=0x%08x\n", byte, byte, pc);
        } else {
            qemu_log("sc6530_uart: TX byte=0x%02x pc=0x%08x\n", byte, pc);
        }
        trace_sc6530_uart_tx(byte, pc);

        if (s->line_len < 128) {
            s->line_buf[s->line_len++] = byte;
        }

        if (byte == '\n' || s->line_len == 128) {
            sc6530_uart_flush_line(s, pc);
        }
        break;
    case 0x10: /* ARM_UART_IEN */
        s->ien = value;
        break;
    case 0x14: /* ARM_UART_ICLR */
        s->iclr = value;
        break;
    case 0x18: /* ARM_UART_CTL0 */
        s->ctl0 = value;
        break;
    case 0x1C: /* ARM_UART_CTL1 */
        s->ctl1 = value;
        break;
    case 0x20: /* ARM_UART_CTL2 */
        s->ctl2 = value;
        break;
    case 0x24: /* ARM_UART_CLKD0 */
        s->clkd0 = value;
        break;
    case 0x28: /* ARM_UART_CLKD1 */
        s->clkd1 = value;
        break;
    case 0x2C: /* ARM_UART_STS2 */
        s->sts2 = value;
        break;
    case 0x30: /* ARM_UART_DSP_WAIT */
        s->dsp_wait = value;
        break;
    default:
        break;
    }
}

static const MemoryRegionOps sc6530_uart_ops = {
    .read = sc6530_uart_read,
    .write = sc6530_uart_write,
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

static void sc6530_uart_reset(DeviceState *dev)
{
    Sc6530UartState *s = SC6530_UART(dev);

    s->ien = 0;
    s->iclr = 0;
    s->ctl0 = 0;
    s->ctl1 = 0;
    s->ctl2 = 0;
    s->clkd0 = 0;
    s->clkd1 = 0;
    s->sts2 = 0;
    s->dsp_wait = 0;
    s->line_len = 0;
}

static void sc6530_uart_init(Object *obj)
{
    Sc6530UartState *s = SC6530_UART(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &sc6530_uart_ops, s,
                          "sc6530-uart", SC6530_UART_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
}

static const VMStateDescription sc6530_uart_vmsd = {
    .name = "sc6530_uart",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(ien, Sc6530UartState),
        VMSTATE_UINT32(iclr, Sc6530UartState),
        VMSTATE_UINT32(ctl0, Sc6530UartState),
        VMSTATE_UINT32(ctl1, Sc6530UartState),
        VMSTATE_UINT32(ctl2, Sc6530UartState),
        VMSTATE_UINT32(clkd0, Sc6530UartState),
        VMSTATE_UINT32(clkd1, Sc6530UartState),
        VMSTATE_UINT32(sts2, Sc6530UartState),
        VMSTATE_UINT32(dsp_wait, Sc6530UartState),
        VMSTATE_UINT8_ARRAY(line_buf, Sc6530UartState, 128),
        VMSTATE_INT32(line_len, Sc6530UartState),
        VMSTATE_END_OF_LIST()
    }
};

static void sc6530_uart_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, sc6530_uart_reset);
    dc->vmsd = &sc6530_uart_vmsd;
}

static const TypeInfo sc6530_uart_info = {
    .name          = TYPE_SC6530_UART,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Sc6530UartState),
    .instance_init = sc6530_uart_init,
    .class_init    = sc6530_uart_class_init,
};

static void sc6530_uart_register_types(void)
{
    type_register_static(&sc6530_uart_info);
}

type_init(sc6530_uart_register_types)
