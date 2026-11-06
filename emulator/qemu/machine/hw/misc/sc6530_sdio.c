/* SC6530C SDIO0 has the SDHCI register/command layout used by the stock
 * driver at 0xb5200 and the hardware-tested polled Rockbox driver.
 * Reuse QEMU's SDHCI command, PIO/SDMA/ADMA, reset, IRQ and card protocol
 * implementation rather than returning successful commands without media.
 * B310E attaches an optional sd-card block backend to the inherited sd-bus.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "hw/sd/sdhci.h"
#include "hw/core/qdev-properties.h"
#include "qapi/error.h"

#define TYPE_SC6530_SDIO "sc6530_sdio"
OBJECT_DECLARE_SIMPLE_TYPE(Sc6530SdioState, SC6530_SDIO)

struct Sc6530SdioState {
    SDHCIState parent_obj;
    MemoryRegion slot_status;
    /* Fault injection for the guest's error recovery: every Nth CMD24
     * (single-block write) is not sent to the card and fails with a data
     * CRC error instead, as a write lost on the phone's bus would. */
    uint32_t fail_write_every;
    uint32_t writes, failed;
    /* Slow card: every Nth CMD24 keeps transfer-complete hidden for
     * write_busy_ms of virtual time, as a card programming or doing its
     * housekeeping does (seen as ~1 s stalls on the phone). */
    uint32_t write_busy_every, write_busy_ms;
    int64_t busy_until;
    const MemoryRegionOps *sdhci_ops;
    MemoryRegionOps ops;
};

#define SDHC_TRNMOD   0x0c
#define SDHC_CMDREG   0x0e
#define SDHC_NIS_ERR  0x8000
#define SDHC_EIS_DATACRC 0x0020
#define SDHC_NORINTSTS 0x30
#define SDHC_NIS_TRSCMP 0x0002

static uint64_t sc6530_sdio_read(void *opaque, hwaddr offset, unsigned size)
{
    Sc6530SdioState *s = SC6530_SDIO(opaque);
    uint64_t val = s->sdhci_ops->read(opaque, offset, size);

    if (offset == SDHC_NORINTSTS && s->busy_until &&
        qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) < s->busy_until) {
        val &= ~(uint64_t)SDHC_NIS_TRSCMP;
    }
    return val;
}

static void sc6530_sdio_write(void *opaque, hwaddr offset, uint64_t val,
                              unsigned size)
{
    Sc6530SdioState *s = SC6530_SDIO(opaque);
    int cmd = -1;

    if (offset == SDHC_TRNMOD && size == 4) {
        cmd = (val >> 24) & 0x3f;
    } else if (offset == SDHC_CMDREG && size >= 2) {
        cmd = (val >> 8) & 0x3f;
    }
    if (cmd == 24) {
        s->writes++;
        s->busy_until = 0;
    }
    if (cmd == 24 && s->write_busy_every &&
        s->writes % s->write_busy_every == 0) {
        s->busy_until = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) +
                        s->write_busy_ms;
        qemu_log_mask(LOG_UNIMP, "sc6530_sdio: injected write busy %u ms\n",
                      s->write_busy_ms);
    }
    if (cmd == 24 && s->fail_write_every &&
        s->writes % s->fail_write_every == 0) {
        SDHCIState *h = &s->parent_obj;
        s->failed++;
        qemu_log_mask(LOG_UNIMP, "sc6530_sdio: injected write fault %u\n",
                      s->failed);
        h->norintsts |= SDHC_NIS_ERR;
        h->errintsts |= SDHC_EIS_DATACRC;
        return;
    }
    s->sdhci_ops->write(opaque, offset, val, size);
}

static void sc6530_sdio_init(Object *obj)
{
    Sc6530SdioState *s = SC6530_SDIO(obj);
    SDHCIState *h = &s->parent_obj;

    /* SDHCI's init picked its MMIO ops; wrap them (realize maps io_ops). */
    s->sdhci_ops = h->io_ops;
    s->ops = *h->io_ops;
    s->ops.read = sc6530_sdio_read;
    s->ops.write = sc6530_sdio_write;
    h->io_ops = &s->ops;
}

static const Property sc6530_sdio_properties[] = {
    DEFINE_PROP_UINT32("fail-write-every", Sc6530SdioState,
                       fail_write_every, 0),
    DEFINE_PROP_UINT32("write-busy-every", Sc6530SdioState,
                       write_busy_every, 0),
    DEFINE_PROP_UINT32("write-busy-ms", Sc6530SdioState,
                       write_busy_ms, 1000),
};

static void (*sdhci_realize)(DeviceState *, Error **);

static void sc6530_sdio_realize(DeviceState *dev, Error **errp)
{
    Sc6530SdioState *s = SC6530_SDIO(dev);
    Error *local_error = NULL;
    sdhci_realize(dev, &local_error);
    if (local_error) {
        error_propagate(errp, local_error);
        return;
    }
    /* NOR 0xb63bc reads slot interrupt status at +0x1fc to select the
     * controller ISR. SDHCI's default bank places it at +0xfc. Without
     * this alias the first CMD0 IRQ selects no slot and stock asserts. */
    memory_region_init_alias(&s->slot_status, OBJECT(s), "sc6530-slot-status",
                             &s->parent_obj.iomem, 0xfc, 4);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->slot_status);
}

static void sc6530_sdio_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->desc = "SC6530C SDIO0 SDHCI controller";
    sdhci_realize = dc->realize;
    dc->realize = sc6530_sdio_realize;
    device_class_set_props(dc, sc6530_sdio_properties);
}

static const TypeInfo sc6530_sdio_info = {
    .name = TYPE_SC6530_SDIO,
    .parent = TYPE_SYSBUS_SDHCI,
    .instance_size = sizeof(Sc6530SdioState),
    .instance_init = sc6530_sdio_init,
    .class_init = sc6530_sdio_class_init,
};

static void sc6530_sdio_register_types(void)
{
    type_register_static(&sc6530_sdio_info);
}
type_init(sc6530_sdio_register_types)
