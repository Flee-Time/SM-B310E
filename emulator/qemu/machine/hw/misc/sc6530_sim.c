/*
 * Spreadtrum SC6530C SIM controller (0x85000000) with a test GSM card.
 *
 * Register offsets come from the stock sim_drv.c (NOR 0x36400..0x37400):
 *   +00 TX, +04 RX, +08 STS0, +0c STS1, +10 IE, +14 ICLR, +18 CTL0,
 *   +1c CTL1, +20/+34 RX/TX clock divider, +24 retransmit, +28 guard,
 *   +2c/+38 watchdog, +30 IMSK, +3c CTL2 (FIFO marks), +40 STS2
 *   (RX count [6:0], TX count [14:8]).
 * CTL0: b6 card RST, b9 RX enable, b10 TX enable; CTL1 b11 clock enable.
 * The slot is selected in APB 0x8b0001e0 bits 3:2.
 *
 * The card answers a cold reset (RST rising while the clock runs) with an
 * ATR, then speaks GSM 11.11 T=0: SELECT (A4), READ BINARY (B0) and GET
 * RESPONSE (C0) over MF 3F00, EF_ICCID 2FE2, DF_GSM 7F20, EF_IMSI 6F07.
 * Transmission is instant: bytes the guest writes reach the card at once
 * and replies appear in the RX FIFO.
 *
 * Properties: sim-slots (bitmask of slots holding the card; default 0,
 * no card, so stock firmware, whose driver needs IRQ 10, is unaffected),
 * sim-pin (true: EF_IMSI answers 98 04), sim-iccid, sim-imsi (digits).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "system/address-spaces.h"
#include "trace.h"

#define TYPE_SC6530_SIM "sc6530_sim"
OBJECT_DECLARE_SIMPLE_TYPE(Sc6530SimState, SC6530_SIM)

#define SIM_FIFO 64

struct Sc6530SimState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    uint32_t regs[0x44 / 4];
    uint8_t rx[SIM_FIFO];
    unsigned rx_head, rx_count;

    /* card */
    uint8_t cmd[8];
    unsigned cmd_len, expect_data;
    uint16_t file;
    uint32_t slots;
    bool pin;
    char *iccid_prop, *imsi_prop;
};

static void rx_push(Sc6530SimState *s, uint8_t b)
{
    if (s->rx_count < SIM_FIFO) {
        s->rx[(s->rx_head + s->rx_count++) % SIM_FIFO] = b;
    }
}

static void rx_push_all(Sc6530SimState *s, const uint8_t *b, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        rx_push(s, b[i]);
    }
}

static unsigned slot_selected(void)
{
    return (address_space_ldl_le(&address_space_memory, 0x8b0001e0,
                                 MEMTXATTRS_UNSPECIFIED, NULL) >> 2) & 3;
}

static bool card_here(Sc6530SimState *s)
{
    return (s->slots >> slot_selected()) & 1;
}

/* BCD with swapped nibbles, 0xF padded. */
static unsigned bcd(const char *digits, uint8_t *out, unsigned max,
                    int first_nibble)
{
    unsigned n = 0, k = 0;
    int nib[64];
    if (first_nibble >= 0) {
        nib[k++] = first_nibble;
    }
    for (const char *p = digits; *p && k < 64; p++) {
        nib[k++] = *p - '0';
    }
    while (n < max && 2 * n < k) {
        int lo = nib[2 * n], hi = 2 * n + 1 < k ? nib[2 * n + 1] : 0xf;
        out[n++] = (hi << 4) | lo;
    }
    while (n < max) {
        out[n++] = 0xff;
    }
    return n;
}

static void card_file_data(Sc6530SimState *s, uint8_t *data, unsigned len)
{
    memset(data, 0xff, len);
    if (s->file == 0x2fe2) {
        bcd(s->iccid_prop ? s->iccid_prop : "8901260123456789012",
            data, len, -1);
    } else if (s->file == 0x6f07) {
        const char *imsi = s->imsi_prop ? s->imsi_prop : "001010123456789";
        data[0] = (strlen(imsi) + 2) / 2;
        /* First nibble is the parity/type digit (9 = odd, IMSI). */
        bcd(imsi, data + 1, len - 1, 9);
    }
}

static void card_command(Sc6530SimState *s)
{
    uint8_t ins = s->cmd[1], p3 = s->cmd[4];
    static const uint8_t sw_ok[2] = { 0x90, 0x00 };

    trace_sc6530_sim_apdu(s->cmd[0], ins, p3, s->file);
    switch (ins) {
    case 0xa4:                          /* SELECT: ACK, then 2 data bytes */
        rx_push(s, ins);
        s->expect_data = 2;
        return;
    case 0xb0: {                        /* READ BINARY */
        uint8_t data[256];
        if (s->file == 0x6f07 && s->pin) {
            rx_push(s, 0x98);
            rx_push(s, 0x04);
            break;
        }
        if (s->file != 0x2fe2 && s->file != 0x6f07) {
            rx_push(s, 0x94);
            rx_push(s, 0x00);
            break;
        }
        card_file_data(s, data, p3);
        rx_push(s, ins);
        rx_push_all(s, data, p3);
        rx_push_all(s, sw_ok, 2);
        break;
    }
    case 0xc0: {                        /* GET RESPONSE */
        uint8_t data[256] = { 0 };
        rx_push(s, ins);
        rx_push_all(s, data, p3);
        rx_push_all(s, sw_ok, 2);
        break;
    }
    default:
        rx_push(s, 0x6d);
        rx_push(s, 0x00);
    }
    s->cmd_len = 0;
}

static void card_byte(Sc6530SimState *s, uint8_t b)
{
    if (!card_here(s)) {
        return;
    }
    if (s->expect_data) {
        s->cmd[5 + 2 - s->expect_data] = b;
        if (--s->expect_data == 0) {
            uint16_t fid = s->cmd[5] << 8 | s->cmd[6];
            bool known = fid == 0x3f00 || fid == 0x7f20 ||
                         fid == 0x2fe2 || fid == 0x6f07;
            if (known) {
                s->file = fid;
            }
            rx_push(s, known ? 0x9f : 0x94);
            rx_push(s, known ? 0x0f : 0x04);
            s->cmd_len = 0;
        }
        return;
    }
    s->cmd[s->cmd_len++] = b;
    if (s->cmd_len == 5) {
        card_command(s);
    }
}

static void card_reset(Sc6530SimState *s)
{
    static const uint8_t atr[] = {
        0x3b, 0x9e, 0x95, 0x80, 0x1f, 0xc7, 0x80, 0x31, 0xe0, 0x73, 0xfe,
        0x21, 0x1b, 0x66, 0xd0, 0x01, 0x76, 0x21, 0x12, 0x00, 0xb2,
    };
    s->rx_head = s->rx_count = 0;
    s->cmd_len = s->expect_data = 0;
    s->file = 0x3f00;
    if (card_here(s)) {
        rx_push_all(s, atr, sizeof(atr));
    }
    trace_sc6530_sim_reset(slot_selected(), card_here(s));
}

static uint64_t sc6530_sim_read(void *opaque, hwaddr offset, unsigned size)
{
    Sc6530SimState *s = opaque;
    switch (offset) {
    case 0x04:
        if (s->rx_count) {
            uint8_t b = s->rx[s->rx_head];
            s->rx_head = (s->rx_head + 1) % SIM_FIFO;
            s->rx_count--;
            return b;
        }
        return 0;
    case 0x40:
        return s->rx_count & 0x7f;      /* TX FIFO always empty */
    default:
        return offset < sizeof(s->regs) ? s->regs[offset / 4] : 0;
    }
}

static void sc6530_sim_write(void *opaque, hwaddr offset, uint64_t value,
                             unsigned size)
{
    Sc6530SimState *s = opaque;
    uint32_t old;

    if (offset >= sizeof(s->regs)) {
        return;
    }
    old = s->regs[offset / 4];
    s->regs[offset / 4] = value;
    if (offset == 0x00) {
        if (s->regs[0x18 / 4] & 0x400) {  /* TX enabled */
            card_byte(s, value);
        }
    } else if (offset == 0x18) {
        if ((value & 0x10) && !(old & 0x10)) {
            s->rx_head = s->rx_count = 0;  /* RX FIFO reset */
        }
        if ((value & 0x40) && !(old & 0x40) && (s->regs[0x1c / 4] & 0x800)) {
            card_reset(s);
        }
    }
}

static const MemoryRegionOps sc6530_sim_ops = {
    .read = sc6530_sim_read,
    .write = sc6530_sim_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void sc6530_sim_reset(DeviceState *dev)
{
    Sc6530SimState *s = SC6530_SIM(dev);
    memset(s->regs, 0, sizeof(s->regs));
    s->rx_head = s->rx_count = 0;
    s->cmd_len = s->expect_data = 0;
}

static void sc6530_sim_init(Object *obj)
{
    Sc6530SimState *s = SC6530_SIM(obj);
    memory_region_init_io(&s->iomem, obj, &sc6530_sim_ops, s, "sc6530-sim",
                          0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static const Property sc6530_sim_properties[] = {
    DEFINE_PROP_UINT32("sim-slots", Sc6530SimState, slots, 0),
    DEFINE_PROP_BOOL("sim-pin", Sc6530SimState, pin, false),
    DEFINE_PROP_STRING("sim-iccid", Sc6530SimState, iccid_prop),
    DEFINE_PROP_STRING("sim-imsi", Sc6530SimState, imsi_prop),
};

static void sc6530_sim_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->desc = "Spreadtrum SC6530 SIM controller + test GSM card";
    device_class_set_props(dc, sc6530_sim_properties);
    device_class_set_legacy_reset(dc, sc6530_sim_reset);
}

static const TypeInfo sc6530_sim_info = {
    .name = TYPE_SC6530_SIM,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Sc6530SimState),
    .instance_init = sc6530_sim_init,
    .class_init = sc6530_sim_class_init,
};

static void sc6530_sim_register_types(void)
{
    type_register_static(&sc6530_sim_info);
}

type_init(sc6530_sim_register_types)
