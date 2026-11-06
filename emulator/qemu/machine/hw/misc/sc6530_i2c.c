/*
 * Spreadtrum SC6530C I2C controller 1 (0x86001000) with the SM-B310E's
 * Richwave RTC6211S FM tuner (U101, 7-bit address 0x64) on the bus.
 *
 * Controller, from the stock i2c_phy_v5 path (NOR 0xa6ba4..0xa6e06):
 *   +0x00 CTL   b0 INT (command done), b3 IE, b4 EN
 *   +0x04 CMD   data 15:8; b0 INT_ACK, b1 NACK (master), b2 WRITE, b3 READ,
 *               b4 STOP, b5 START, b6 slave NACK (status), b7 BUSY (status)
 *   +0x08/+0x0c clock dividers, +0x10 reset
 * A CMD write with b0 set only acknowledges the interrupt (the driver
 * writes CMD|1); any other write executes one byte command at once.
 *
 * Tuner, from the stock RTC6211S driver: Si470x-style block framing
 * (writes start at register 0x02, reads at 0x0A, both wrap 0x0F -> 0x00)
 * with the RTC6213 register layout. reg00 = 0x1688, reg01[15:10] = 3.
 * reg08 != 0 selects a hidden bank; writes then go nowhere visible.
 * "fm-chip" selects the variant: "n" (default; the SM-B310E tested has it)
 * reads 0xFFFF everywhere but reg00 until a write stream sets reg00 =
 * 0x96AA ("turnon"), after which reg01[15:10] = 4; 0x16AA turns it off.
 * "s" answers at once with reg01[15:10] = 3.
 * Stations come from the "fm-stations" property (comma-separated 10 kHz
 * units, default "10000,9230" = 100.0 and 92.3 MHz); on a station RSSI
 * is 50 and stereo is set, elsewhere RSSI is 5.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/cutils.h"
#include "qapi/error.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "trace.h"

#define TYPE_SC6530_I2C "sc6530_i2c"
OBJECT_DECLARE_SIMPLE_TYPE(Sc6530I2cState, SC6530_I2C)

#define CMD_INT_ACK 0x01
#define CMD_WRITE   0x04
#define CMD_READ    0x08
#define CMD_STOP    0x10
#define CMD_START   0x20
#define CMD_RXNACK  0x40

#define FM_ADDR     0x64
#define FM_MAX_STATIONS 16

struct Sc6530I2cState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    uint32_t ctl, cmd, clkd0, clkd1;

    /* bus transaction */
    bool selected, reading;
    unsigned ptr, byte_lo;

    /* RTC6211 */
    uint16_t regs[16];
    char *stations_prop;
    char *chip_prop;
    bool chip_n, awake;
    unsigned stations[FM_MAX_STATIONS], station_count;
};

static unsigned fm_base(const Sc6530I2cState *s)
{
    switch ((s->regs[3] >> 12) & 3) {
    case 0: return 8750;
    case 3: return 6500;
    default: return 7600;
    }
}

static unsigned fm_space(const Sc6530I2cState *s)
{
    static const unsigned space[4] = { 20, 10, 5, 5 };
    return space[(s->regs[3] >> 10) & 3];
}

static bool fm_on_station(const Sc6530I2cState *s, unsigned f)
{
    for (unsigned i = 0; i < s->station_count; i++) {
        if (s->stations[i] == f) {
            return true;
        }
    }
    return false;
}

static void fm_settle(Sc6530I2cState *s, unsigned chan, bool failed)
{
    unsigned f = fm_base(s) + chan * fm_space(s);
    bool hit = fm_on_station(s, f);
    s->regs[0xa] = 0x4000 | (failed ? 0x2000 : 0) | (hit ? 0x0400 : 0) |
                   (chan & 0x3ff);
    s->regs[0xb] = hit ? 50 : 5;
    trace_sc6530_fm_tune(f, s->regs[0xb], failed);
}

static void fm_seek(Sc6530I2cState *s)
{
    unsigned space = fm_space(s), base = fm_base(s);
    unsigned top = base == 8750 ? 10800 : base == 6500 ? 7600 : 10800;
    unsigned chan = s->regs[0xa] & 0x3ff;
    bool up = s->regs[5] & 0x4000;
    for (unsigned n = 0; n < 1024; n++) {
        if (up) {
            chan++;
            if (base + chan * space > top) {
                break;
            }
        } else {
            if (!chan) {
                break;
            }
            chan--;
        }
        if (fm_on_station(s, base + chan * space)) {
            fm_settle(s, chan, false);
            return;
        }
    }
    fm_settle(s, s->regs[0xa] & 0x3ff, true);
}

static void fm_register_written(Sc6530I2cState *s, unsigned reg, uint16_t v)
{
    if (reg != 8 && s->regs[8] != 0) {
        return;                         /* hidden bank */
    }
    if (reg == 0 && s->chip_n) {
        trace_sc6530_fm_reg(reg, v);
        if (v == 0x96aa) {
            s->awake = true;
        } else if (v == 0x16aa) {
            s->awake = false;
        }
        return;
    }
    if (reg <= 1 || reg >= 0xa) {
        return;                         /* IDs and status are read-only */
    }
    s->regs[reg] = v;
    trace_sc6530_fm_reg(reg, v);
    if (reg == 3) {
        if (v & 0x8000) {
            fm_settle(s, v & 0x3ff, false);
        } else {
            s->regs[0xa] &= ~0x4000;    /* clearing TUNE clears STC */
        }
    } else if (reg == 5) {
        if (v & 0x8000) {
            fm_seek(s);
        } else {
            s->regs[0xa] &= ~0x6000;
        }
    }
}

static int fm_byte(Sc6530I2cState *s, bool write, uint8_t data)
{
    unsigned reg = s->ptr & 15;
    if (write) {
        if (!s->byte_lo) {
            s->byte_lo = 0x100 | data;
        } else {
            fm_register_written(s, reg,
                                (uint16_t)((s->byte_lo & 0xff) << 8 | data));
            s->byte_lo = 0;
            s->ptr++;
        }
        return 0;
    }
    uint16_t word = s->chip_n && !s->awake && reg != 0 ? 0xffff : s->regs[reg];
    uint8_t value = s->byte_lo ? word & 0xff : word >> 8;
    if (s->byte_lo) {
        s->byte_lo = 0;
        s->ptr++;
    } else {
        s->byte_lo = 1;
    }
    return value;
}

static void sc6530_i2c_execute(Sc6530I2cState *s, uint32_t cmd)
{
    uint8_t data = cmd >> 8;
    bool nack = false;

    if (cmd & CMD_START) {
        s->selected = (data >> 1) == FM_ADDR;
        s->reading = data & 1;
        s->ptr = s->reading ? 0xa : 0x2;
        s->byte_lo = 0;
        nack = !s->selected;
    } else if (cmd & CMD_WRITE) {
        if (s->selected && !s->reading) {
            fm_byte(s, true, data);
        } else {
            nack = true;
        }
    } else if (cmd & CMD_READ) {
        data = s->selected && s->reading ? fm_byte(s, false, 0) : 0xff;
    }
    if (cmd & CMD_STOP) {
        s->selected = false;
    }
    s->cmd = (cmd & 0x3f & ~CMD_INT_ACK) | (nack ? CMD_RXNACK : 0) |
             ((uint32_t)data << 8);
    s->ctl |= 1;
    trace_sc6530_i2c_cmd(cmd & 0xffff, data, nack);
}

static uint64_t sc6530_i2c_read(void *opaque, hwaddr offset, unsigned size)
{
    Sc6530I2cState *s = opaque;
    switch (offset) {
    case 0x00: return s->ctl;
    case 0x04: return s->cmd;          /* BUSY never set: commands are instant */
    case 0x08: return s->clkd0;
    case 0x0c: return s->clkd1;
    default:   return 0;
    }
}

static void sc6530_i2c_write(void *opaque, hwaddr offset, uint64_t value,
                             unsigned size)
{
    Sc6530I2cState *s = opaque;
    switch (offset) {
    case 0x00:
        s->ctl = (s->ctl & 1) | (value & ~1u);
        break;
    case 0x04:
        if (value & CMD_INT_ACK) {
            s->ctl &= ~1u;
            s->cmd &= ~CMD_INT_ACK;
        } else {
            sc6530_i2c_execute(s, value);
        }
        break;
    case 0x08:
        s->clkd0 = value;
        break;
    case 0x0c:
        s->clkd1 = value;
        break;
    case 0x10:
        if (value & 1) {
            s->ctl = s->cmd = 0;
            s->selected = false;
        }
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "sc6530_i2c: write +0x%" HWADDR_PRIx "\n",
                      offset);
    }
}

static const MemoryRegionOps sc6530_i2c_ops = {
    .read = sc6530_i2c_read,
    .write = sc6530_i2c_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void sc6530_i2c_reset(DeviceState *dev)
{
    Sc6530I2cState *s = SC6530_I2C(dev);
    s->ctl = s->cmd = s->clkd0 = s->clkd1 = 0;
    s->selected = false;
    memset(s->regs, 0, sizeof(s->regs));
    s->regs[0] = 0x1688;
    s->regs[1] = (s->chip_n ? 4u : 3u) << 10;
    s->awake = false;
}

static void sc6530_i2c_realize(DeviceState *dev, Error **errp)
{
    Sc6530I2cState *s = SC6530_I2C(dev);
    const char *p = s->stations_prop ? s->stations_prop : "10000,9230";
    s->chip_n = !s->chip_prop || strcmp(s->chip_prop, "s") != 0;
    s->station_count = 0;
    while (*p && s->station_count < FM_MAX_STATIONS) {
        unsigned long v;
        const char *end;
        if (qemu_strtoul(p, &end, 10, &v) < 0) {
            error_setg(errp, "fm-stations: bad list");
            return;
        }
        s->stations[s->station_count++] = v;
        p = *end == ',' ? end + 1 : end;
    }
}

static void sc6530_i2c_init(Object *obj)
{
    Sc6530I2cState *s = SC6530_I2C(obj);
    memory_region_init_io(&s->iomem, obj, &sc6530_i2c_ops, s,
                          "sc6530-i2c1", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static const Property sc6530_i2c_properties[] = {
    DEFINE_PROP_STRING("fm-stations", Sc6530I2cState, stations_prop),
    DEFINE_PROP_STRING("fm-chip", Sc6530I2cState, chip_prop),
};

static void sc6530_i2c_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->desc = "Spreadtrum SC6530 I2C1 + Richwave RTC6211N/S FM tuner";
    dc->realize = sc6530_i2c_realize;
    device_class_set_props(dc, sc6530_i2c_properties);
    device_class_set_legacy_reset(dc, sc6530_i2c_reset);
}

static const TypeInfo sc6530_i2c_info = {
    .name = TYPE_SC6530_I2C,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Sc6530I2cState),
    .instance_init = sc6530_i2c_init,
    .class_init = sc6530_i2c_class_init,
};

static void sc6530_i2c_register_types(void)
{
    type_register_static(&sc6530_i2c_info);
}

type_init(sc6530_i2c_register_types)
