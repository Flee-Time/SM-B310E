/*
 * Spreadtrum SC6530C LCDC display controller + LCM DBI controller
 * (B310E-OS QEMU machine).
 *
 * Todo 16 of .omo/plans/b310e-qemu-machine.md (Wave 3): a MINIMAL display
 * model - the register banks are store+echo, and the only modeled behavior
 * is the LCDC refresh DMA: when the guest starts a refresh (drivers/lcd.c
 * lcd_show / lcdc_init_regs: `irq.en |= 1; ctrl |= 8; poll irq.raw; irq.clr
 * |= 1`), the framebuffer is copied out of PSRAM and pushed into a
 * QemuConsole, and the refresh-complete flag is set so the guest's poll
 * terminates.
 *
 * Devices (both in this file):
 *
 *   sc6530_lcdc @ 0x20d00000, 0x1000 - the LCDC register bank. Offsets are
 *     LOCKED by the _Static_asserts in drivers/lcd.c (lcdc_t layout, mirror
 *     of fpdoom syscode.h): ctrl @ 0x00 (bit 3 = start refresh), img struct
 *     @ 0x20 with img.y_base_addr @ 0x24 (the DMA source = fb address >> 2),
 *     irq @ 0x110: en / clr / status / raw @ 0x118/0x11c. The model:
 *       - ctrl write with bit 3 SET  -> refresh trigger: copy 128*160*2
 *         bytes from PSRAM at img.y_base_addr << 2 (read via the system
 *         address space, so BOTH the 0x34000000 and 0x04000000 windows
 *         work through the always-on alias), convert RGB565 -> x8r8g8b8
 *         into the console surface, qemu_console_update, then set irq.raw
 *         bit 0 (DMA done) so the guest's poll loop exits on the first
 *         read. Every other ctrl write (bit 0/1 enable, fmark, etc.) is a
 *         plain store+echo.
 *       - irq.raw 0x11c read        -> the pending bits (bit 0 = DMA done).
 *       - irq.status 0x118 read     -> raw & en (masked status).
 *       - irq.clr 0x114 write       -> write-1-to-clear of the raw bits.
 *       - everything else           -> store+echo (RMW chains stay stable).
 *     The console renders STATELESSLY from PSRAM on every gfx_update (the
 *     screendump path calls it via qemu_console_co_wait_update), so a
 *     screendump always captures the current framebuffer even without a
 *     refresh trigger since the last write.
 *
 *   sc6530_lcm @ 0x20800000, 0x1000 - the parallel DBI controller config
 *     bank (LCM_CR(0)/CR(0x10)/CR(0x14), drivers/lcd.c). Store+echo only:
 *     the panel init table execution (the 0x60000000 data window writes)
 *     is NOT modeled - the framebuffer DMA is what matters, and those
 *     window accesses fall into the todo-12 catch-all (logged, benign).
 *     CR(0) reads return 0 (bit 1 = busy clear), so the guest's
 *     lcm_wait_idle never spins.
 *
 * QemuConsole: created in realize (musicpal's pattern, hw/arm/musicpal.c),
 * 128x160 x8r8g8b8 (qemu_console_resize; the ST7735 panel is RGB565, we
 * convert in the render path). Headless: -display none + HMP screendump
 * <f>.png -f png (needs --enable-png, todo W2-9b).
 *
 * Trace: sc6530_lcdc_refresh(fb, pc) on every refresh trigger (fb = the
 * resolved framebuffer address, pc = the guest PC) - the refresh-detection
 * evidence channel. Enable with --trace "sc6530_lcdc_*".
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
#include "ui/console.h"
#include "system/address-spaces.h"
#include "trace.h"

#define TYPE_SC6530_LCDC "sc6530_lcdc"
OBJECT_DECLARE_SIMPLE_TYPE(Sc6530LcdcState, SC6530_LCDC)

#define TYPE_SC6530_LCM "sc6530_lcm"
OBJECT_DECLARE_SIMPLE_TYPE(Sc6530LcmState, SC6530_LCM)

/* ---------------------------------------------------------------------- */
/* Region geometry + register offsets (drivers/lcd.c _Static_asserts)     */
/* ---------------------------------------------------------------------- */

#define SC6530_LCDC_BASE         0x20d00000ULL
#define SC6530_LCDC_SIZE         0x1000
#define SC6530_LCM_BASE          0x20800000ULL
#define SC6530_LCM_SIZE          0x1000

/* Panel geometry: ST7735 128x160, RGB565 (16 bpp). */
#define SC6530_LCDC_W            128
#define SC6530_LCDC_H            160
#define SC6530_LCDC_FB_BYTES     (SC6530_LCDC_W * SC6530_LCDC_H * 2)

/* lcdc_t offsets (drivers/lcd.c:63-130, _Static_assert-locked). */
#define SC6530_LCDC_CTRL_OFF     0x00    /* bit 3 = start refresh */
#define SC6530_LCDC_IMG_Y_BASE   0x24    /* img.y_base_addr (fb >> 2) */
#define SC6530_LCDC_IRQ_EN_OFF   0x110
#define SC6530_LCDC_IRQ_CLR_OFF  0x114
#define SC6530_LCDC_IRQ_STS_OFF  0x118
#define SC6530_LCDC_IRQ_RAW_OFF  0x11c

/* ctrl bit 3: the refresh-start bit (fpdoom lcdc_base_t, syscode.c
 * sys_start_refresh: `lcdc->ctrl |= 8`). */
#define SC6530_LCDC_CTRL_REFRESH (1u << 3)

/* irq.raw bit 0: DMA/refresh-complete (the guest polls `irq.raw & 1`). */
#define SC6530_LCDC_IRQ_DMA_DONE (1u << 0)

/* ---------------------------------------------------------------------- */
/* Device state                                                           */
/* ---------------------------------------------------------------------- */

struct Sc6530LcdcState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/

    MemoryRegion iomem;       /* 0x20d00000 */
    QemuConsole *con;         /* the display console (128x160) */

    uint32_t regs[SC6530_LCDC_SIZE / 4];  /* store+echo bank */
    uint32_t irq_raw;         /* pending bits (bit 0 = DMA done) */
};

struct Sc6530LcmState {
    SysBusDevice parent_obj;

    /*< public >*/

    MemoryRegion iomem;       /* 0x20800000 */
    MemoryRegion data_iomem;  /* 0x60000000 data window */

    uint32_t regs[SC6530_LCM_SIZE / 4];   /* store+echo bank */
    int rdid_state;
};

/* ---------------------------------------------------------------------- */
/* LCDC: render the framebuffer from PSRAM into the console surface.      */
/* Reads 128*160*2 bytes via the SYSTEM address space at                 */
/* img.y_base_addr << 2 - the alias makes BOTH the 0x34000000 window      */
/* (our os.bin: lcd_fb) and the 0x04000000 window (the stock OS's runtime */
/* framebuffer) readable. RGB565 -> x8r8g8b8.                            */
/* ---------------------------------------------------------------------- */

static void sc6530_lcdc_render(Sc6530LcdcState *s)
{
    DisplaySurface *surface;
    uint32_t *dst;
    size_t stride_words;
    uint16_t fb[SC6530_LCDC_W * SC6530_LCDC_H];
    hwaddr fb_addr =
        (hwaddr)s->regs[SC6530_LCDC_IMG_Y_BASE >> 2] << 2;
    int y, x, i = 0;

    surface = qemu_console_surface(s->con);
    if (!surface) {
        return;
    }
    if (address_space_read(&address_space_memory, fb_addr,
                           MEMTXATTRS_UNSPECIFIED, fb,
                           sizeof(fb)) != MEMTX_OK) {
        return;
    }

    dst = surface_data(surface);
    stride_words = surface_stride(surface) / 4;
    for (y = 0; y < SC6530_LCDC_H; y++) {
        uint32_t *row = dst + (size_t)y * stride_words;

        for (x = 0; x < SC6530_LCDC_W; x++) {
            uint16_t p = fb[i++];
            uint32_t r5 = (p >> 11) & 0x1f;
            uint32_t g6 = (p >> 5) & 0x3f;
            uint32_t b5 = p & 0x1f;

            row[x] = (r5 << 19) | (g6 << 10) | (b5 << 3);
        }
    }
    qemu_console_update(s->con, 0, 0, SC6530_LCDC_W, SC6530_LCDC_H);
}

static uint32_t sc6530_lcdc_guest_pc(void)
{
    CPUState *cs = current_cpu;

    if (cs) {
        return ARM_CPU(cs)->env.regs[15];
    }
    return 0;
}

/* ---------------------------------------------------------------------- */
/* LCDC MMIO                                                              */
/* ---------------------------------------------------------------------- */

static uint64_t sc6530_lcdc_read(void *opaque, hwaddr offset, unsigned size)
{
    Sc6530LcdcState *s = opaque;
    uint64_t val;
    uint32_t word;

    switch (offset) {
    case SC6530_LCDC_IRQ_RAW_OFF:
        /* Raw pending bits: the guest polls bit 0 (DMA done). Read-only. */
        return extract32(s->irq_raw, (offset % 4) * 8, size * 8);
    case SC6530_LCDC_IRQ_STS_OFF:
        /* Masked status: raw & en. */
        word = s->irq_raw & s->regs[SC6530_LCDC_IRQ_EN_OFF >> 2];
        return extract32(word, (offset % 4) * 8, size * 8);
    default:
        val = s->regs[offset / 4];
        return extract32(val, (offset % 4) * 8, size * 8);
    }
}

static void sc6530_lcdc_write(void *opaque, hwaddr offset,
                              uint64_t value, unsigned size)
{
    Sc6530LcdcState *s = opaque;
    uint32_t word = s->regs[offset / 4];
    uint32_t mask = (size == 4) ? 0xffffffffu : ((1u << (size * 8)) - 1);
    unsigned shift = (offset % 4) * 8;
    uint32_t newv = (word & ~(mask << shift)) |
                    (((uint32_t)value & mask) << shift);

    switch (offset) {
    case SC6530_LCDC_CTRL_OFF:
        if (newv & SC6530_LCDC_CTRL_REFRESH) {
            /* Start-refresh bit set (lcd_show's `ctrl |= 8`): copy the
             * framebuffer out of PSRAM and complete the DMA synchronously.
             * The guest then polls irq.raw bit 0 - set it so the poll
             * exits on the first read. */
            sc6530_lcdc_render(s);
            s->irq_raw |= SC6530_LCDC_IRQ_DMA_DONE;
            trace_sc6530_lcdc_refresh(
                (uint64_t)s->regs[SC6530_LCDC_IMG_Y_BASE >> 2] << 2,
                sc6530_lcdc_guest_pc());
        }
        break;
    case SC6530_LCDC_IRQ_CLR_OFF:
        /* Write-1-to-clear the pending bits (the guest's `irq.clr |= 1`). */
        s->irq_raw &= ~newv;
        break;
    default:
        break;
    }
    s->regs[offset / 4] = newv;
}

static const MemoryRegionOps sc6530_lcdc_ops = {
    .read  = sc6530_lcdc_read,
    .write = sc6530_lcdc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 1, .max_access_size = 4 },
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

/* ---------------------------------------------------------------------- */
/* LCDC console: gfx_update renders the CURRENT framebuffer (screendump   */
/* calls it via qemu_console_co_wait_update), invalidate is a no-op       */
/* (render is stateless).                                                 */
/* ---------------------------------------------------------------------- */

static void sc6530_lcdc_gfx_invalidate(void *opaque)
{
}

static bool sc6530_lcdc_gfx_update(void *opaque)
{
    Sc6530LcdcState *s = opaque;

    sc6530_lcdc_render(s);
    return true;
}

static const GraphicHwOps sc6530_lcdc_gfx_ops = {
    .invalidate  = sc6530_lcdc_gfx_invalidate,
    .gfx_update  = sc6530_lcdc_gfx_update,
};

/* ---------------------------------------------------------------------- */
/* LCM MMIO: store+echo config bank (LCM_CR(0)/CR(0x10)/CR(0x14) - the    */
/* DBI mode/timing words from drivers/lcd.c; the panel init table itself  */
/* is not modeled). CR(0) bit 1 (busy) never sets, so lcm_wait_idle       */
/* never spins.                                                          */
/* ---------------------------------------------------------------------- */

static uint64_t sc6530_lcm_read(void *opaque, hwaddr offset, unsigned size)
{
    Sc6530LcmState *s = opaque;
    uint32_t word = s->regs[offset / 4];

    return extract32(word, (offset % 4) * 8, size * 8);
}

static void sc6530_lcm_write(void *opaque, hwaddr offset,
                             uint64_t value, unsigned size)
{
    Sc6530LcmState *s = opaque;
    uint32_t word = s->regs[offset / 4];
    uint32_t mask = (size == 4) ? 0xffffffffu : ((1u << (size * 8)) - 1);
    unsigned shift = (offset % 4) * 8;

    s->regs[offset / 4] = (word & ~(mask << shift)) |
                          (((uint32_t)value & mask) << shift);
}

static const MemoryRegionOps sc6530_lcm_ops = {
    .read  = sc6530_lcm_read,
    .write = sc6530_lcm_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 1, .max_access_size = 4 },
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

/* ---------------------------------------------------------------------- */
/* SysBus devices                                                         */
/* ---------------------------------------------------------------------- */

static void sc6530_lcdc_reset(DeviceState *dev)
{
    Sc6530LcdcState *s = SC6530_LCDC(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->irq_raw = 0;
}

static void sc6530_lcdc_realize(DeviceState *dev, Error **errp)
{
    Sc6530LcdcState *s = SC6530_LCDC(dev);

    /* The only graphic console in the machine: screendump picks it up as
     * console index 0. 128x160 x8r8g8b8 surface (musicpal pattern). */
    s->con = qemu_graphic_console_create(dev, 0, &sc6530_lcdc_gfx_ops, s);
    qemu_console_resize(s->con, SC6530_LCDC_W, SC6530_LCDC_H);
}

static void sc6530_lcdc_init(Object *obj)
{
    Sc6530LcdcState *s = SC6530_LCDC(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &sc6530_lcdc_ops, s,
                          "sc6530-lcdc", SC6530_LCDC_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
}

static void sc6530_lcdc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "Spreadtrum SC6530 LCDC display controller";
    dc->realize = sc6530_lcdc_realize;
    device_class_set_legacy_reset(dc, sc6530_lcdc_reset);
}

static const TypeInfo sc6530_lcdc_info = {
    .name          = TYPE_SC6530_LCDC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Sc6530LcdcState),
    .instance_init = sc6530_lcdc_init,
    .class_init    = sc6530_lcdc_class_init,
};


/* ---------------------------------------------------------------------- */
/* LCM DATA WINDOW @ 0x60000000 (0x40000 size)                            */
/* Stock panel driver (ST7735S) sends RDID (0x04) to 0x60000000 and reads */
/* three bytes from 0x60020000: 0x7c, 0x89, 0xf0. If the ID is wrong, it  */
/* bails out and never configures the LCDC img base.                      */
/* ---------------------------------------------------------------------- */

static uint64_t sc6530_lcm_data_read(void *opaque, hwaddr offset, unsigned size)
{
    Sc6530LcmState *s = opaque;
    uint32_t val = 0;
    qemu_log_mask(LOG_UNIMP, "sc6530_lcm_data: r addr=0x%lx state=%d\n", (long)offset, s->rdid_state);

    if (offset == 0x20000) {
        if (s->rdid_state == 1) {
            val = 0x00; /* dummy read */
            s->rdid_state = 2;
        } else if (s->rdid_state == 2) {
            val = 0x7c;
            s->rdid_state = 3;
        } else if (s->rdid_state == 3) {
            val = 0x89;
            s->rdid_state = 4;
        } else if (s->rdid_state == 4) {
            val = 0xf0;
            s->rdid_state = 0;
        }
    }
    return val;
}

static void sc6530_lcm_data_write(void *opaque, hwaddr offset, uint64_t val, unsigned size)
{
    Sc6530LcmState *s = opaque;
    qemu_log_mask(LOG_UNIMP, "sc6530_lcm_data: w addr=0x%lx val=0x%lx\n", (long)offset, (long)val);


    if (offset == 0 && val == 0x04) {
        s->rdid_state = 1;
    } else {
        s->rdid_state = 0;
    }
}

static const MemoryRegionOps sc6530_lcm_data_ops = {
    .read  = sc6530_lcm_data_read,
    .write = sc6530_lcm_data_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 1, .max_access_size = 4 },
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static void sc6530_lcm_reset(DeviceState *dev)
{
    Sc6530LcmState *s = SC6530_LCM(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->rdid_state = 0;
}

static void sc6530_lcm_init(Object *obj)
{
    Sc6530LcmState *s = SC6530_LCM(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &sc6530_lcm_ops, s,
                          "sc6530-lcm", SC6530_LCM_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);

    memory_region_init_io(&s->data_iomem, obj, &sc6530_lcm_data_ops, s,
                          "sc6530-lcm-data", 0x40000);
    sysbus_init_mmio(sbd, &s->data_iomem);
}

static void sc6530_lcm_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "Spreadtrum SC6530 LCM DBI controller";
    device_class_set_legacy_reset(dc, sc6530_lcm_reset);
}

static const TypeInfo sc6530_lcm_info = {
    .name          = TYPE_SC6530_LCM,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Sc6530LcmState),
    .instance_init = sc6530_lcm_init,
    .class_init    = sc6530_lcm_class_init,
};

static void sc6530_lcdc_register_types(void)
{
    type_register_static(&sc6530_lcdc_info);
    type_register_static(&sc6530_lcm_info);
}

type_init(sc6530_lcdc_register_types)
