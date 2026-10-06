/* SC6530C SDIO0 has the SDHCI register/command layout used by the stock
 * driver at 0xb5200 and the hardware-tested polled Rockbox driver.
 * Reuse QEMU's SDHCI command, PIO/SDMA/ADMA, reset, IRQ and card protocol
 * implementation rather than returning successful commands without media.
 * B310E attaches an optional sd-card block backend to the inherited sd-bus.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "hw/sd/sdhci.h"
#include "qapi/error.h"

#define TYPE_SC6530_SDIO "sc6530_sdio"
OBJECT_DECLARE_SIMPLE_TYPE(Sc6530SdioState, SC6530_SDIO)

struct Sc6530SdioState {
    SDHCIState parent_obj;
    MemoryRegion slot_status;
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
}

static const TypeInfo sc6530_sdio_info = {
    .name = TYPE_SC6530_SDIO,
    .parent = TYPE_SYSBUS_SDHCI,
    .instance_size = sizeof(Sc6530SdioState),
    .class_init = sc6530_sdio_class_init,
};

static void sc6530_sdio_register_types(void)
{
    type_register_static(&sc6530_sdio_info);
}
type_init(sc6530_sdio_register_types)
