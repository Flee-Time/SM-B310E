/* SC6530C LCDC/DBI and the B310E ST7735 panel.
 * Refresh composes enabled image/OSD layers over the LCDC background,
 * using source pitch, position and the LCM crop, then streams the result
 * into the panel's CASET/RASET window. Panel pixels persist after the
 * guest frees its source buffers and across partial updates.
 * Register offsets and RGB565 layout are verified against stock e52q7a.
 * Stock icons use 32-bit RGB888 with inline pixel alpha; backgrounds use
 * RGB565. Unsupported formats/rotation are logged rather than misread.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/log.h"
#include "qemu/bitops.h"
#include "qemu/bswap.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
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
    uint8_t pixels[SC6530_LCDC_FB_BYTES]; /* latched RGB565 panel image */
    qemu_irq irq;
    uint16_t column_start, column_end, row_start, row_end;
    uint16_t gram_x, gram_y;
    bool gram_write;
    bool panel_sleep, display_on;
    bool panel_initialized;
};

struct Sc6530LcmState {
    SysBusDevice parent_obj;

    /*< public >*/

    MemoryRegion iomem;       /* 0x20800000 */
    MemoryRegion data_iomem;  /* 0x60000000 data window */

    uint32_t regs[SC6530_LCM_SIZE / 4];   /* store+echo bank */
    int rdid_state;
    Sc6530LcdcState *lcdc;
    uint8_t command, parameter_count, parameters[4];
    uint8_t pixel_high;
    bool have_pixel_high;
};

/* ---------------------------------------------------------------------- */
/* Render the last DMA copy, never the guest's potentially freed buffer. */
/* ---------------------------------------------------------------------- */

static void sc6530_lcdc_render(Sc6530LcdcState *s)
{
    DisplaySurface *surface;
    uint32_t *dst;
    size_t stride_words;
    int y, x, i = 0;

    surface = qemu_console_surface(s->con);
    if (!surface) {
        return;
    }
    dst = surface_data(surface);
    stride_words = surface_stride(surface) / 4;
    for (y = 0; y < SC6530_LCDC_H; y++) {
        uint32_t *row = dst + (size_t)y * stride_words;

        for (x = 0; x < SC6530_LCDC_W; x++) {
            uint16_t p = lduw_le_p(s->pixels + 2 * i++);
            if (s->panel_sleep || !s->display_on) {
                p = 0;
            }
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

static void sc6530_panel_pixel(Sc6530LcdcState *s, uint16_t pixel)
{
    if (!s->gram_write) {
        return;
    }
    if (s->gram_x < SC6530_LCDC_W && s->gram_y < SC6530_LCDC_H) {
        stw_le_p(s->pixels + 2 * (s->gram_y * SC6530_LCDC_W + s->gram_x), pixel);
    }
    if (++s->gram_x > s->column_end) {
        s->gram_x = s->column_start;
        if (++s->gram_y > s->row_end) {
            s->gram_y = s->row_start;
        }
    }
}

static uint32_t sc6530_rgb565(uint16_t pixel)
{
    return ((pixel & 0xf800) << 8) | ((pixel & 0x07e0) << 5) |
           ((pixel & 0x001f) << 3);
}

static uint16_t sc6530_to565(uint32_t rgb)
{
    return ((rgb >> 8) & 0xf800) | ((rgb >> 5) & 0x07e0) |
           ((rgb >> 3) & 0x001f);
}

/* Image and OSD blocks share CTRL/BASE/SIZE/PITCH/POS offsets. A disabled
 * block must never read its old, potentially freed source allocation. */
static uint32_t sc6530_layer_pixel(Sc6530LcdcState *s, unsigned base,
                                   unsigned x, unsigned y, uint32_t under)
{
    uint32_t *r = &s->regs[base / 4];
    uint32_t ctrl = r[0], format = (ctrl >> 4) & 15;
    unsigned w = r[3] & 0xfff, h = (r[3] >> 16) & 0xfff;
    unsigned dx = r[5] & 0xfff, dy = (r[5] >> 16) & 0xfff;
    unsigned pitch = r[4] & 0xfff, endian = (ctrl >> 8) & 3;
    unsigned alpha = 255, sx, sy;
    uint32_t rgb;
    uint8_t bytes[4];
    hwaddr addr;

    if (!(ctrl & 1) || x < dx || y < dy || x - dx >= w || y - dy >= h || !pitch) {
        return under;
    }
    sx = x - dx;
    sy = y - dy;
    if ((format != 3 && format != 5) || (ctrl & 0x1c00)) {
        return under;
    }
    addr = (hwaddr)r[1] << 2;
    if (format == 3) {
        uint32_t pixel;
        address_space_read(&address_space_memory, addr + 4 * (sy * pitch + sx),
                           MEMTXATTRS_UNSPECIFIED, bytes, 4);
        /* The LCDC consumes A,R,G,B bytes from a DMA word. Endian 0 is
         * native ARGB; 1 reverses the bytes; 2 swaps the two halfwords. */
        pixel = endian == 1 || endian == 3 ? ldl_be_p(bytes) : ldl_le_p(bytes);
        if (endian >= 2) {
            pixel = (pixel << 16) | (pixel >> 16);
        }
        rgb = pixel & 0xffffff;
        alpha = pixel >> 24;
    } else {
        unsigned index = sy * pitch + sx;
        uint16_t pixel;
        /* DMA word order 2 sends low halfword first, MSB byte first:
         * stock's 0x4251/0x0255 therefore read native little-endian RGB565.
         * Order 0 sends the high halfword first; order 1 reverses bytes. */
        addr += 2 * (endian == 0 || endian == 3 ? index ^ 1 : index);
        address_space_read(&address_space_memory, addr,
                           MEMTXATTRS_UNSPECIFIED, bytes, 2);
        pixel = endian == 1 || endian == 3 ? lduw_be_p(bytes) : lduw_le_p(bytes);
        rgb = sc6530_rgb565(pixel);
    }
    if (ctrl & (1 << 15)) {
        rgb = (rgb & 0x00ff00) | ((rgb & 255) << 16) | (rgb >> 16);
    }
    if (base != 0x20) {
        unsigned select = (ctrl >> 2) & 3;
        if ((ctrl & 2) && (format == 3 ? rgb == (r[8] & 0xffffff) :
                          sc6530_to565(rgb) == sc6530_to565(r[8]))) {
            return under;
        }
        {
            /* RGB565 pixel alpha is a separate byte plane on OSD1.
             * RGB888 carries alpha in the fourth byte of each pixel. */
            hwaddr alpha_addr = ((hwaddr)r[2] << 2) + sy * pitch + sx;
            if (format == 5 && select != 1) {
                unsigned alpha_endian = (ctrl >> 13) & 3;
                static const unsigned order[] = {3, 0, 1, 2};
                alpha_addr = ((hwaddr)r[2] << 2) +
                             ((sy * pitch + sx) ^ order[alpha_endian]);
                address_space_read(&address_space_memory, alpha_addr,
                                   MEMTXATTRS_UNSPECIFIED, bytes, 1);
                alpha = bytes[0];
            }
        }
        if (select == 1) {
            alpha = r[6] & 255;
        } else if (select == 2) {
            alpha = alpha * (r[6] & 255) / 255;
        }
        rgb = (((((rgb >> 16) & 255) * alpha + ((under >> 16) & 255) * (255-alpha)) / 255) << 16) |
              (((((rgb >> 8) & 255) * alpha + ((under >> 8) & 255) * (255-alpha)) / 255) << 8) |
              (((rgb & 255) * alpha + (under & 255) * (255-alpha)) / 255);
    }
    return rgb;
}

static void sc6530_lcdc_refresh_panel(Sc6530LcdcState *s)
{
    uint32_t cap_ctrl = s->regs[0xe0 / 4];
    bool capture = cap_ctrl & 1;
    unsigned size_reg = capture ? 0xec / 4 : 3;
    unsigned start_reg = capture ? 0xe8 / 4 : 2;
    unsigned width = s->regs[size_reg] & 0xfff;
    unsigned height = (s->regs[size_reg] >> 16) & 0xfff;
    unsigned x0 = s->regs[start_reg] & 0xfff;
    unsigned y0 = (s->regs[start_reg] >> 16) & 0xfff;
    unsigned disp_w = s->regs[1] & 0xfff, disp_h = (s->regs[1] >> 16) & 0xfff;
    static const unsigned layers[] = { 0x20, 0xb0, 0x80, 0x50 };
    uint16_t frame[SC6530_LCDC_W * SC6530_LCDC_H];

    /* The physical output area bounds work even for corrupt guest sizes. */
    width = MIN(width, SC6530_LCDC_W);
    height = MIN(height, SC6530_LCDC_H);
    if (capture && (((cap_ctrl >> 1) & 3) != 2 || (cap_ctrl & 0x700))) {
        qemu_log_mask(LOG_UNIMP, "sc6530_lcdc: unsupported capture ctrl %x\n", cap_ctrl);
        return;
    }
    for (unsigned i = 0; i < ARRAY_SIZE(layers); i++) {
        uint32_t ctrl = s->regs[layers[i] / 4];
        unsigned format = (ctrl >> 4) & 15;
        if ((ctrl & 1) && ((format != 3 && format != 5) || (ctrl & 0x1c00))) {
            qemu_log_mask(LOG_UNIMP, "sc6530_lcdc: unsupported layer %x ctrl %x\n",
                          layers[i], ctrl);
        }
    }
    for (unsigned y = 0; y < height; y++) {
        for (unsigned x = 0; x < width; x++) {
            uint32_t rgb = s->regs[4] & 0xffffff;
            if (x + x0 < disp_w && y + y0 < disp_h) {
                for (unsigned i = 0; i < ARRAY_SIZE(layers); i++) {
                    if (capture && (cap_ctrl & (1 << 16)) && layers[i] == 0x20) {
                        continue;
                    }
                    rgb = sc6530_layer_pixel(s, layers[i], x + x0, y + y0, rgb);
                }
            }
            frame[y * width + x] = sc6530_to565(rgb);
        }
    }
    if (capture) {
        unsigned pitch = s->regs[0xf0 / 4] & 0xfff;
        unsigned endian = (cap_ctrl >> 3) & 3;
        hwaddr base = (hwaddr)s->regs[0xe4 / 4] << 2;
        if (!pitch) {
            return;
        }
        /* Latch the complete source before capture writes: stock composes
         * into its own OSD buffer, then displays it with IMG disabled. */
        for (unsigned y = 0; y < height; y++) {
            for (unsigned x = 0; x < width; x++) {
                unsigned index = y * pitch + x;
                uint8_t bytes[2];
                uint16_t pixel = frame[y * width + x];
                if (cap_ctrl & (1 << 15)) {
                    pixel = (pixel & 0x07e0) | ((pixel & 31) << 11) | (pixel >> 11);
                }
                if (endian == 1 || endian == 3) {
                    stw_be_p(bytes, pixel);
                } else {
                    stw_le_p(bytes, pixel);
                }
                address_space_write(&address_space_memory,
                                    base + 2 * (endian == 0 || endian == 3 ? index ^ 1 : index),
                                    MEMTXATTRS_UNSPECIFIED, bytes, 2);
            }
        }
    } else {
        for (unsigned i = 0; i < width * height; i++) {
            sc6530_panel_pixel(s, frame[i]);
        }
    }
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
    trace_sc6530_lcdc_write(SC6530_LCDC_BASE + offset, newv,
                           sc6530_lcdc_guest_pc());

    switch (offset) {
    case SC6530_LCDC_CTRL_OFF:
        if (newv & SC6530_LCDC_CTRL_REFRESH) {
            sc6530_lcdc_refresh_panel(s);
            sc6530_lcdc_render(s);
            s->irq_raw |= SC6530_LCDC_IRQ_DMA_DONE;
            trace_sc6530_lcdc_refresh(
                (uint64_t)s->regs[SC6530_LCDC_IMG_Y_BASE >> 2] << 2,
                sc6530_lcdc_guest_pc());
            /* START is a command strobe, not a persistent enable. Stock
             * later read/OR/writes CTRL to wake the controller; retaining
             * START would incorrectly DMA the already-freed old buffer. */
            newv &= ~SC6530_LCDC_CTRL_REFRESH;
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
    qemu_set_irq(s->irq, (s->irq_raw &
                          s->regs[SC6530_LCDC_IRQ_EN_OFF >> 2]) != 0);
}

static const MemoryRegionOps sc6530_lcdc_ops = {
    .read  = sc6530_lcdc_read,
    .write = sc6530_lcdc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 1, .max_access_size = 4 },
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

/* ---------------------------------------------------------------------- */
/* gfx_update redraws retained pixels, also when a screendump requests it. */
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
    memset(s->pixels, 0, sizeof(s->pixels));
    s->column_start = s->row_start = s->gram_x = s->gram_y = 0;
    s->column_end = SC6530_LCDC_W - 1;
    s->row_end = SC6530_LCDC_H - 1;
    s->gram_write = false;
    /* The board's loader handoff skips the ROM's panel initialization.
     * Keep that initial state explicit; later sleep commands still apply. */
    s->panel_sleep = !s->panel_initialized;
    s->display_on = s->panel_initialized;
    s->irq_raw = 0;
    qemu_set_irq(s->irq, 0);
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

    sysbus_init_irq(SYS_BUS_DEVICE(s), &s->irq);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &sc6530_lcdc_ops, s,
                          "sc6530-lcdc", SC6530_LCDC_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
}

static const Property sc6530_lcdc_properties[] = {
    DEFINE_PROP_BOOL("panel-initialized", Sc6530LcdcState, panel_initialized, false),
};

static void sc6530_lcdc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "Spreadtrum SC6530 LCDC display controller";
    dc->realize = sc6530_lcdc_realize;
    device_class_set_props(dc, sc6530_lcdc_properties);
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
    Sc6530LcdcState *panel = s->lcdc;
    trace_sc6530_lcm_command(offset, val, sc6530_lcdc_guest_pc());
    qemu_log_mask(LOG_UNIMP, "sc6530_lcm_data: w addr=0x%lx val=0x%lx\n", (long)offset, (long)val);


    if (offset == 0) {
        s->command = val & 255;
        s->parameter_count = 0;
        s->have_pixel_high = false;
        s->rdid_state = s->command == 4 ? 1 : 0;
        if (panel) {
            switch (s->command) {
            case 0x10: panel->panel_sleep = true; break;
            case 0x11: panel->panel_sleep = false; break;
            case 0x28: panel->display_on = false; break;
            case 0x29: panel->display_on = true; break;
            default: break;
            }
            sc6530_lcdc_render(panel);
            panel->gram_write = s->command == 0x2c;
            if (panel->gram_write) {
                panel->gram_x = panel->column_start;
                panel->gram_y = panel->row_start;
            }
        }
    } else if (offset == 0x20000 && panel) {
        if ((s->command == 0x2a || s->command == 0x2b) && s->parameter_count < 4) {
            s->parameters[s->parameter_count++] = val & 255;
            if (s->parameter_count == 4) {
                uint16_t start = (s->parameters[0] << 8) | s->parameters[1];
                uint16_t end = (s->parameters[2] << 8) | s->parameters[3];
                /* Invalid windows consume no unbounded host work. */
                if (end >= start) {
                    if (s->command == 0x2a) {
                        panel->column_start = start;
                        panel->column_end = end;
                    } else {
                        panel->row_start = start;
                        panel->row_end = end;
                    }
                }
            }
        } else if (s->command == 0x2c) {
            /* The stock config uses an 8-bit DBI bus. Halfword/word MMIO
             * writes still put the low byte on that bus. */
            if (!s->have_pixel_high) {
                s->pixel_high = val & 255;
                s->have_pixel_high = true;
            } else {
                sc6530_panel_pixel(panel, (s->pixel_high << 8) | (val & 255));
                s->have_pixel_high = false;
            }
        }
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
    s->command = s->parameter_count = 0;
    s->have_pixel_high = false;
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
    static const Property props[] = {
        DEFINE_PROP_LINK("lcdc", Sc6530LcmState, lcdc, TYPE_SC6530_LCDC,
                         Sc6530LcdcState *),
    };
    device_class_set_props(dc, props);
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
