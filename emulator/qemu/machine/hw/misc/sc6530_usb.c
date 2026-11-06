/*
 * Spreadtrum SC6530C USB device controller ("sx_udc") model.
 *
 * Register semantics come from the stock B310E firmware's own drivers
 * (decompiled from NOR): sx_udc.c @ 0x1efa8-0x20b44 (interrupt driven, DMA),
 * vcom_usb.c @ 0x3862c-0x38f14 (polled bring-up + CDC) and the boot loader's
 * download stack @ 0x9b3c-0xa2cc. See docs/usb.md for the full map.
 *
 * Endpoints are fixed: EP0 control, EP1 IN, EP2 OUT, EP3 IN, EP4 OUT, all
 * full speed with 64-byte FIFOs. Register bank 0x20300000:
 *
 *   0x00 CTRL        bit0 controller enable, bit1 connect (D+ pull-up)
 *   0x0c FIFO_RST    write 1: reset FIFO (b0 EP0-IN b1 EP0-OUT b2 EP1 b3 EP2
 *                    b4 EP3 b5 EP4); self clearing
 *   0x18 INT_STS     b8+n: endpoint n has an enabled pending status
 *   0x1c INT_CLR     (device-level events; nothing modeled)
 *   0x40 EP0 TR_SIZE 0x5c/0x60 SETUP bytes 0-3 / 4-7
 *   0x64 EP0 CTRL    0x68 INT_CTRL  0x6c INT_STS  0x70 INT_CLR (write 1)
 *                    (EP0 has no RX count or TRANS_SIZE in this block)
 *   0x80+n*0x40      EPn: +0 CTRL +4 RX count +8 TRANS_SIZE
 *                    +0xc INT_CTRL +0x10 INT_STS +0x14 INT_CLR
 *
 * Endpoint CTRL: [10:0] received byte count (read only), [22:12] packet
 * size, b25 enable, b27 IN armed (TX start), b28 OUT armed (buffer ready),
 * b29 stall. Endpoint INT_STS: b0 transaction end, b8 SETUP (EP0), b9
 * transfer end. The controller answers SET_ADDRESS and every status stage by
 * itself; the stock drivers never write a device address or a status ZLP.
 * SET_ADDRESS and SET_CONFIGURATION raise no SETUP interrupt at all
 * (verified on the phone). An EP0 IN data stage ends once TR_SIZE bytes
 * have been sent: bit 0 per packet, bit 9 at TR_SIZE (boot loader 0xa1a4).
 *
 * FIFO window 0x20380000: +0 EP0-IN +4 EP1-IN +8 EP3-IN +0xc EP0-OUT
 * +0x10 EP2-OUT +0x14 EP4-OUT. Words carry wire bytes most significant
 * first (the stock code byte-swaps every word).
 *
 * The host side is a chardev ("chardev" property). Each request is
 *   u8 op, u8 ep, u16 len (LE), payload
 * and gets one reply  u8 code, u16 len (LE), payload.
 *   'Q' query            -> 'A' {attached, address, ctrl[4]}
 *   'R' bus reset        -> 'A'
 *   'S' SETUP (8 bytes)  -> 'A' | 'N' (detached)
 *   'O' OUT data to ep   -> 'A' | 'N' (not armed) | 'T' (stalled)
 *   'I' IN token         -> 'D' data | 'N' | 'T'
 *                           (optional u16 payload: most bytes accepted)
 * Status stages are part of the 'I'/'O' traffic on EP0 and complete in the
 * model, as they do in the controller.
 *
 * irq-stuck=on holds line 25 high whenever the controller is enabled, with
 * nothing in INT_STS: a test for the driver's interrupt-storm fallback.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/log.h"
#include "qemu/bitops.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "chardev/char-fe.h"
#include "trace.h"

size_t sc6530_dma_hw_request(Object *obj, unsigned request, size_t bytes);

#define TYPE_SC6530_USB "sc6530_usb"
OBJECT_DECLARE_SIMPLE_TYPE(Sc6530UsbState, SC6530_USB)

#define SC6530_USB_SIZE       0x1000
#define SC6530_USB_FIFO_SIZE  0x100

#define USB_NUM_EP      5
#define USB_FIFO_BYTES  64

/* global registers */
#define REG_CTRL        0x00
#define REG_FIFO_RST    0x0c
#define REG_INT_STS     0x18
#define REG_INT_CLR     0x1c
#define REG_EP0_TRSIZE  0x40
#define REG_SETUP_LO    0x5c
#define REG_SETUP_HI    0x60

#define CTRL_ENABLE     (1u << 0)
#define CTRL_CONNECT    (1u << 1)

/* per-endpoint register offsets from the endpoint's CTRL register */
#define EP_CTRL         0x00
#define EP_RXCNT        0x04
#define EP_TRSIZE       0x08
#define EP_INT_CTRL     0x0c
#define EP_INT_STS      0x10
#define EP_INT_CLR      0x14
#define EP_DMA          0x1c    /* set to 400 by every stock DMA start */

#define EPCTRL_COUNT    0x7ffu
#define EPCTRL_SIZE_SH  12
#define EPCTRL_SIZE     (0x7ffu << EPCTRL_SIZE_SH)
#define EPCTRL_ENABLE   (1u << 25)
#define EPCTRL_TX       (1u << 27)
#define EPCTRL_RX       (1u << 28)
#define EPCTRL_STALL    (1u << 29)

#define EPINT_XACT      (1u << 0)
#define EPINT_SETUP     (1u << 8)
#define EPINT_DONE      (1u << 9)
/* OUT refused because buffer ready was clear: the host tried to send
 * (stock EP2/EP4 handlers 0x1fc4a/0x1fe80 start their transfer on it) */
#define EPINT_OUT_TOKEN (1u << 12)

/* FIFO indices (FIFO window word slot and FIFO_RST bit) */
enum { FIFO_EP0_IN, FIFO_EP0_OUT, FIFO_EP1, FIFO_EP2, FIFO_EP3, FIFO_EP4,
       FIFO_NUM };

/* FIFO window slot -> FIFO index */
static const int fifo_slot[6] = {
    FIFO_EP0_IN, FIFO_EP1, FIFO_EP3, FIFO_EP0_OUT, FIFO_EP2, FIFO_EP4
};

/* EP0 control-transfer stage, as the controller tracks it */
enum { EP0_IDLE, EP0_DATA_IN, EP0_DATA_OUT, EP0_STATUS_IN, EP0_STATUS_OUT };

typedef struct {
    uint8_t data[USB_FIFO_BYTES + 4];
    uint32_t count;     /* bytes held */
    uint32_t rd;        /* read position */
} UsbFifo;

typedef struct {
    uint32_t ctrl, rxcnt, trsize, int_ctrl, int_sts;
    uint32_t sent;      /* bytes sent (IN) or received (OUT) of TRANS_SIZE */
    uint32_t dma;       /* +0x1c, stored only: meaning unknown */
} UsbEp;

struct Sc6530UsbState {
    SysBusDevice parent_obj;

    MemoryRegion usb_iomem;
    MemoryRegion fifo_iomem;
    qemu_irq irq;
    CharFrontend chr;
    Object *dma;        /* sc6530_dma: endpoint FIFO requests */

    uint32_t ctrl;
    uint32_t timeout_lmt;
    uint32_t ep0_trsize;
    uint32_t ep0_sent;  /* bytes sent in the current EP0 IN data stage */
    uint32_t setup[2];
    uint8_t address;
    int ep0_stage;
    UsbEp ep[USB_NUM_EP];
    UsbFifo fifo[FIFO_NUM];

    bool irq_stuck;     /* test: line 25 held high with no source set */
    bool dma_requests;  /* FIFO DMA requests raised (off: test PIO fallback) */
    bool dma_out_requests; /* the same for OUT FIFOs only */

    /* host protocol reassembly */
    uint8_t rx[4 + 4096];
    uint32_t rx_len;
};

static bool usb_attached(Sc6530UsbState *s)
{
    return (s->ctrl & (CTRL_ENABLE | CTRL_CONNECT)) ==
           (CTRL_ENABLE | CTRL_CONNECT);
}

static void usb_update_irq(Sc6530UsbState *s)
{
    bool level = s->irq_stuck && (s->ctrl & CTRL_ENABLE);

    for (int n = 0; n < USB_NUM_EP; n++) {
        if (s->ep[n].int_sts & s->ep[n].int_ctrl) {
            level = true;
        }
    }
    qemu_set_irq(s->irq, level);
}

static uint32_t usb_int_sts(Sc6530UsbState *s)
{
    uint32_t v = 0;

    for (int n = 0; n < USB_NUM_EP; n++) {
        if (s->ep[n].int_sts & s->ep[n].int_ctrl) {
            v |= 1u << (8 + n);
        }
    }
    return v;
}

static void fifo_reset(UsbFifo *f)
{
    f->count = 0;
    f->rd = 0;
}

static int ep_in_fifo(int ep)
{
    return ep == 0 ? FIFO_EP0_IN : ep == 1 ? FIFO_EP1 : FIFO_EP3;
}

static int ep_out_fifo(int ep)
{
    return ep == 0 ? FIFO_EP0_OUT : ep == 2 ? FIFO_EP2 : FIFO_EP4;
}

/* ---- register bank ---------------------------------------------------- */

/* decode an endpoint register; returns the endpoint or -1. EP0 packs its
 * interrupt registers right after CTRL (0x64 CTRL, 0x68 INT_CTRL, 0x6c
 * INT_STS, 0x70 INT_CLR); EP1-4 have RX count and transfer size between. */
static int ep_decode(hwaddr offset, hwaddr *reg)
{
    static const hwaddr ep0_regs[] = {
        EP_CTRL, EP_INT_CTRL, EP_INT_STS, EP_INT_CLR,
    };

    if (offset >= 0x64 && offset <= 0x70) {
        *reg = ep0_regs[(offset - 0x64) / 4];
        return 0;
    }
    if (offset >= 0xc0 && offset < 0x1c0) {
        *reg = (offset - 0x80) % 0x40;
        return (offset - 0x80) / 0x40;
    }
    return -1;
}

static uint64_t sc6530_usb_read(void *opaque, hwaddr offset, unsigned size)
{
    Sc6530UsbState *s = opaque;
    hwaddr reg;
    int n;
    uint32_t v = 0;

    switch (offset & ~3) {
    case REG_CTRL:       v = s->ctrl; break;
    case REG_INT_STS:    v = usb_int_sts(s); break;
    case 0x28:           v = s->timeout_lmt; break;
    case REG_EP0_TRSIZE: v = s->ep0_trsize; break;
    case REG_SETUP_LO:   v = s->setup[0]; break;
    case REG_SETUP_HI:   v = s->setup[1]; break;
    default:
        n = ep_decode(offset & ~3, &reg);
        if (n < 0) {
            break;
        }
        switch (reg) {
        case EP_CTRL:     v = s->ep[n].ctrl; break;
        case EP_RXCNT:    v = s->ep[n].rxcnt; break;
        case EP_TRSIZE:   v = s->ep[n].trsize; break;
        case EP_INT_CTRL: v = s->ep[n].int_ctrl; break;
        case EP_INT_STS:  v = s->ep[n].int_sts; break;
        case EP_DMA:      v = s->ep[n].dma; break;
        }
    }
    return extract32(v, (offset & 3) * 8, size * 8);
}

static void sc6530_usb_write(void *opaque, hwaddr offset, uint64_t value,
                             unsigned size)
{
    Sc6530UsbState *s = opaque;
    hwaddr reg;
    int n;
    uint32_t v = value;

    if (size != 4) {
        qemu_log_mask(LOG_GUEST_ERROR, "sc6530_usb: %u-byte write 0x%"
                      HWADDR_PRIx "\n", size, offset);
        return;
    }
    trace_sc6530_usb_write(offset, v);

    switch (offset) {
    case REG_CTRL:
        if (!(v & CTRL_ENABLE) && (s->ctrl & CTRL_ENABLE)) {
            s->address = 0;
            s->ep0_stage = EP0_IDLE;
        }
        s->ctrl = v & 3;
        usb_update_irq(s);
        return;
    case REG_FIFO_RST:
        for (int f = 0; f < FIFO_NUM; f++) {
            if (v & (1u << f)) {
                fifo_reset(&s->fifo[f]);
            }
        }
        return;
    case REG_INT_CLR:
        return;
    case 0x28:
        s->timeout_lmt = v;
        return;
    case REG_EP0_TRSIZE:
        s->ep0_trsize = v & 0x1ffff;
        s->ep0_sent = 0;
        return;
    }

    n = ep_decode(offset, &reg);
    if (n < 0) {
        return;
    }
    switch (reg) {
    case EP_CTRL:
        s->ep[n].ctrl = (s->ep[n].ctrl & EPCTRL_COUNT) | (v & ~EPCTRL_COUNT);
        break;
    case EP_TRSIZE:
        s->ep[n].trsize = v & 0x1ffff;
        s->ep[n].sent = 0;
        break;
    case EP_INT_CTRL:
        s->ep[n].int_ctrl = v;
        break;
    case EP_INT_CLR:
        s->ep[n].int_sts &= ~v;
        break;
    case EP_DMA:
        s->ep[n].dma = v;
        break;
    }
    usb_update_irq(s);
}

static const MemoryRegionOps sc6530_usb_ops = {
    .read  = sc6530_usb_read,
    .write = sc6530_usb_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 1, .max_access_size = 4 },
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

/* ---- FIFO window ------------------------------------------------------ */

static uint64_t sc6530_usb_fifo_read(void *opaque, hwaddr offset,
                                     unsigned size)
{
    Sc6530UsbState *s = opaque;
    unsigned slot = offset / 4;
    UsbFifo *f;
    uint32_t v = 0;

    if (slot >= ARRAY_SIZE(fifo_slot) || size != 4) {
        return 0;
    }
    f = &s->fifo[fifo_slot[slot]];
    for (int i = 0; i < 4; i++) {
        v <<= 8;
        if (f->rd < f->count) {
            v |= f->data[f->rd];
        }
        f->rd++;
    }
    return v;
}

static void sc6530_usb_fifo_write(void *opaque, hwaddr offset,
                                  uint64_t value, unsigned size)
{
    Sc6530UsbState *s = opaque;
    unsigned slot = offset / 4;
    UsbFifo *f;

    if (slot >= ARRAY_SIZE(fifo_slot) || size != 4) {
        return;
    }
    f = &s->fifo[fifo_slot[slot]];
    for (int i = 3; i >= 0; i--) {
        if (f->count < sizeof(f->data)) {
            f->data[f->count++] = value >> (i * 8);
        }
    }
}

static const MemoryRegionOps sc6530_usb_fifo_ops = {
    .read  = sc6530_usb_fifo_read,
    .write = sc6530_usb_fifo_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 1, .max_access_size = 4 },
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

/* ---- host side -------------------------------------------------------- */

static void host_reply(Sc6530UsbState *s, uint8_t code, const uint8_t *data,
                       uint16_t len)
{
    uint8_t hdr[3] = { code, len & 0xff, len >> 8 };

    qemu_chr_fe_write_all(&s->chr, hdr, sizeof(hdr));
    if (len) {
        qemu_chr_fe_write_all(&s->chr, data, len);
    }
}

/* A bus reset wipes the endpoint setup (B310E-OS usb_debug, verified on
 * the phone's replug: EP0's packet size and the bulk endpoints had to be
 * programmed again). EP0's SETUP interrupt survives: the phone kept
 * receiving SETUPs after Windows reset it. */
static void host_bus_reset(Sc6530UsbState *s)
{
    s->address = 0;
    s->ep0_stage = EP0_IDLE;
    s->ep[0].ctrl &= ~(EPCTRL_TX | EPCTRL_RX | EPCTRL_STALL | EPCTRL_SIZE);
    s->ep[0].int_sts = 0;
    for (int n = 1; n < USB_NUM_EP; n++) {
        s->ep[n].ctrl = 0;
        s->ep[n].trsize = 0;
        s->ep[n].int_ctrl = 0;
        s->ep[n].int_sts = 0;
    }
    usb_update_irq(s);
    for (int f = 0; f < FIFO_NUM; f++) {
        fifo_reset(&s->fifo[f]);
    }
}

static void host_setup(Sc6530UsbState *s, const uint8_t *req)
{
    uint16_t wlength = req[6] | req[7] << 8;
    UsbEp *ep0 = &s->ep[0];

    s->setup[0] = ldl_le_p(req);
    s->setup[1] = ldl_le_p(req + 4);
    /* a SETUP ends whatever was in flight, stall included */
    ep0->ctrl &= ~(EPCTRL_TX | EPCTRL_RX | EPCTRL_STALL);
    fifo_reset(&s->fifo[FIFO_EP0_IN]);
    fifo_reset(&s->fifo[FIFO_EP0_OUT]);
    s->ep0_sent = 0;
    if (wlength == 0) {
        s->ep0_stage = EP0_STATUS_IN;
    } else {
        s->ep0_stage = (req[0] & 0x80) ? EP0_DATA_IN : EP0_DATA_OUT;
    }
    /* Hardware-verified (2026-11-02 phone log): SET_ADDRESS and
     * SET_CONFIGURATION are completed by the controller and never raise a
     * SETUP interrupt. */
    if (req[0] == 0x00 && req[1] == 0x05) {
        s->address = req[2] & 0x7f;
        trace_sc6530_usb_setup(s->setup[0], s->setup[1]);
        return;
    }
    if (req[0] == 0x00 && req[1] == 0x09) {
        trace_sc6530_usb_setup(s->setup[0], s->setup[1]);
        return;
    }
    ep0->int_sts |= EPINT_SETUP;
    trace_sc6530_usb_setup(s->setup[0], s->setup[1]);
    usb_update_irq(s);
}

/* IN token: returns the reply code, fills data/len */
static uint8_t host_in(Sc6530UsbState *s, int n, uint8_t *data, uint16_t *len,
                       uint16_t max)
{
    UsbEp *ep = &s->ep[n];
    UsbFifo *f;
    uint32_t size;

    *len = 0;
    if (n != 0 && n != 1 && n != 3) {
        return 'T';
    }
    if (ep->ctrl & EPCTRL_STALL) {
        return 'T';
    }
    if (n == 0 && (s->ep0_stage == EP0_STATUS_IN ||
                   s->ep0_stage == EP0_DATA_OUT)) {
        s->ep0_stage = EP0_IDLE;        /* status ZLP from the controller */
        return 'D';
    }
    /* Once TR_SIZE bytes have gone out the controller considers the data
     * stage over and ends it with a zero length packet of its own; data
     * queued per packet with TR_SIZE set to the packet length is cut short
     * here (the phone's HID report descriptor failure, 2026-11-02). */
    if (n == 0 && s->ep0_stage == EP0_DATA_IN && s->ep0_trsize &&
        s->ep0_sent >= s->ep0_trsize) {
        return 'D';
    }
    if (n != 0 && !(ep->ctrl & EPCTRL_ENABLE)) {
        return 'N';
    }
    if (!(ep->ctrl & EPCTRL_TX)) {
        return 'N';
    }
    f = &s->fifo[ep_in_fifo(n)];
    size = (ep->ctrl & EPCTRL_SIZE) >> EPCTRL_SIZE_SH;
    if (n != 0 && ep->trsize > ep->sent) {
        size = MIN(size, ep->trsize - ep->sent);
    }
    /* The FIFO's DMA request (stock 0x1f092: 0x20102040 + 4 * FIFO index,
     * one-based request 17 + index) refills it for the packet. */
    if (n != 0 && s->dma && s->dma_requests && f->count < size) {
        sc6530_dma_hw_request(s->dma, 17 + ep_in_fifo(n), size - f->count);
    }
    if (n != 0 && size && f->count < size) {
        return 'N';                     /* packet not loaded (yet) */
    }
    if (n == 0 && size && !f->count) {
        return 'N';                     /* armed, next packet not loaded */
    }
    size = MIN(size, MIN(f->count, (uint32_t)max));
    memcpy(data, f->data, size);
    *len = size;
    fifo_reset(f);
    if (n == 0) {
        /* EP0: transaction end per packet, transfer end at TR_SIZE. TX
         * start stays set until TR_SIZE bytes are out (the phone, 2026-11-02:
         * a driver waiting for it to clear after the first packet of a
         * 118-byte reply never sent the second). */
        s->ep0_sent += size;
        ep->int_sts |= EPINT_XACT;
        if (s->ep0_sent >= s->ep0_trsize) {
            ep->int_sts |= EPINT_DONE;
            ep->ctrl &= ~EPCTRL_TX;
        }
        s->ep0_stage = EP0_DATA_IN;
    } else {
        /* bulk: a transfer of TRANS_SIZE bytes, packet by packet; one
         * packet when the driver sets TRANS_SIZE per packet (PIO) */
        ep->sent += size;
        ep->int_sts |= EPINT_XACT;
        if (ep->sent >= ep->trsize) {
            ep->ctrl &= ~EPCTRL_TX;
            ep->int_sts |= EPINT_DONE;
        }
    }
    trace_sc6530_usb_in(n, size);
    usb_update_irq(s);
    return 'D';
}

static uint8_t host_out(Sc6530UsbState *s, int n, const uint8_t *data,
                        uint16_t len)
{
    UsbEp *ep = &s->ep[n];
    UsbFifo *f;

    if (n != 0 && n != 2 && n != 4) {
        return 'T';
    }
    if (ep->ctrl & EPCTRL_STALL) {
        return 'T';
    }
    if (n == 0 && (s->ep0_stage == EP0_DATA_IN ||
                   s->ep0_stage == EP0_STATUS_OUT) && len == 0) {
        s->ep0_stage = EP0_IDLE;        /* status stage of a control read */
        return 'A';
    }
    if (n != 0 && !(ep->ctrl & EPCTRL_ENABLE)) {
        return 'N';
    }
    if (!(ep->ctrl & EPCTRL_RX)) {
        if (n != 0) {
            ep->int_sts |= EPINT_OUT_TOKEN;
            usb_update_irq(s);
        }
        return 'N';
    }
    if (len > USB_FIFO_BYTES) {
        return 'T';
    }
    f = &s->fifo[ep_out_fifo(n)];
    if (n != 0 && f->rd < f->count) {
        return 'N';                     /* the last packet is still there */
    }
    memcpy(f->data, data, len);
    f->count = len;
    f->rd = 0;
    if (n == 0) {
        ep->ctrl = (ep->ctrl & ~(EPCTRL_RX | EPCTRL_COUNT)) | len;
        ep->rxcnt = len;
        ep->int_sts |= EPINT_XACT | EPINT_DONE;
    } else {
        /* Bulk: a transfer of TRANS_SIZE bytes (stock 0x1f454: DMA with
         * TRANS_SIZE the whole length); one packet when the driver sets
         * TRANS_SIZE per packet (PIO). RX count (+0x04) totals the
         * transfer. The FIFO's DMA request drains each packet. */
        uint32_t size = (ep->ctrl & EPCTRL_SIZE) >> EPCTRL_SIZE_SH;

        ep->sent += len;
        ep->rxcnt = ep->sent;
        ep->ctrl = (ep->ctrl & ~EPCTRL_COUNT) | len;
        if (s->dma && s->dma_requests && s->dma_out_requests) {
            sc6530_dma_hw_request(s->dma, 17 + ep_out_fifo(n), len);
        }
        ep->int_sts |= EPINT_XACT;
        if (ep->sent >= ep->trsize || len < size) {
            ep->ctrl &= ~EPCTRL_RX;
            ep->int_sts |= EPINT_DONE;
        }
    }
    trace_sc6530_usb_out(n, len);
    usb_update_irq(s);
    return 'A';
}

static void host_request(Sc6530UsbState *s, uint8_t op, uint8_t n,
                         const uint8_t *payload, uint16_t len)
{
    uint8_t buf[USB_FIFO_BYTES];
    uint16_t out_len;
    uint8_t code;

    if (n >= USB_NUM_EP) {
        host_reply(s, 'T', NULL, 0);
        return;
    }
    switch (op) {
    case 'Q':
        buf[0] = usb_attached(s);
        buf[1] = s->address;
        stl_le_p(buf + 2, s->ctrl);
        host_reply(s, 'A', buf, 6);
        return;
    case 'R':
        host_bus_reset(s);
        host_reply(s, 'A', NULL, 0);
        return;
    }
    if (!usb_attached(s)) {
        host_reply(s, 'N', NULL, 0);
        return;
    }
    switch (op) {
    case 'S':
        if (len != 8) {
            host_reply(s, 'T', NULL, 0);
            return;
        }
        host_setup(s, payload);
        host_reply(s, 'A', NULL, 0);
        return;
    case 'I':
        code = host_in(s, n, buf, &out_len,
                       len == 2 ? MIN(lduw_le_p(payload), USB_FIFO_BYTES)
                                : USB_FIFO_BYTES);
        host_reply(s, code, buf, code == 'D' ? out_len : 0);
        return;
    case 'O':
        host_reply(s, host_out(s, n, payload, len), NULL, 0);
        return;
    }
    host_reply(s, 'T', NULL, 0);
}

static int sc6530_usb_can_receive(void *opaque)
{
    Sc6530UsbState *s = opaque;

    return sizeof(s->rx) - s->rx_len;
}

static void sc6530_usb_receive(void *opaque, const uint8_t *buf, int size)
{
    Sc6530UsbState *s = opaque;

    memcpy(s->rx + s->rx_len, buf, size);
    s->rx_len += size;
    while (s->rx_len >= 4) {
        uint16_t len = s->rx[2] | s->rx[3] << 8;
        uint32_t total = 4 + len;

        if (total > sizeof(s->rx)) {
            s->rx_len = 0;              /* garbage: resynchronise */
            return;
        }
        if (s->rx_len < total) {
            return;
        }
        host_request(s, s->rx[0], s->rx[1], s->rx + 4, len);
        memmove(s->rx, s->rx + total, s->rx_len - total);
        s->rx_len -= total;
    }
}

/* ---- device ----------------------------------------------------------- */

static void sc6530_usb_reset(DeviceState *dev)
{
    Sc6530UsbState *s = SC6530_USB(dev);

    s->ctrl = 0;
    s->timeout_lmt = 0;
    s->ep0_trsize = 0;
    s->ep0_sent = 0;
    s->setup[0] = s->setup[1] = 0;
    s->address = 0;
    s->ep0_stage = EP0_IDLE;
    memset(s->ep, 0, sizeof(s->ep));
    for (int f = 0; f < FIFO_NUM; f++) {
        fifo_reset(&s->fifo[f]);
    }
    s->rx_len = 0;
    qemu_set_irq(s->irq, 0);
}

static void sc6530_usb_realize(DeviceState *dev, Error **errp)
{
    Sc6530UsbState *s = SC6530_USB(dev);

    qemu_chr_fe_set_handlers(&s->chr, sc6530_usb_can_receive,
                             sc6530_usb_receive, NULL, NULL, s, NULL, true);
}

static void sc6530_usb_init(Object *obj)
{
    Sc6530UsbState *s = SC6530_USB(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->usb_iomem, obj, &sc6530_usb_ops, s,
                          "sc6530-usb", SC6530_USB_SIZE);
    sysbus_init_mmio(sbd, &s->usb_iomem);

    memory_region_init_io(&s->fifo_iomem, obj, &sc6530_usb_fifo_ops, s,
                          "sc6530-usb-fifo", SC6530_USB_FIFO_SIZE);
    sysbus_init_mmio(sbd, &s->fifo_iomem);
    sysbus_init_irq(sbd, &s->irq);
    object_property_add_link(obj, "dma", "sc6530_dma", &s->dma,
                             object_property_allow_set_link,
                             OBJ_PROP_LINK_STRONG);
}

static const Property sc6530_usb_properties[] = {
    DEFINE_PROP_CHR("chardev", Sc6530UsbState, chr),
    DEFINE_PROP_BOOL("irq-stuck", Sc6530UsbState, irq_stuck, false),
    DEFINE_PROP_BOOL("dma-requests", Sc6530UsbState, dma_requests, true),
    DEFINE_PROP_BOOL("dma-out-requests", Sc6530UsbState, dma_out_requests, true),
};

static void sc6530_usb_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = sc6530_usb_realize;
    device_class_set_legacy_reset(dc, sc6530_usb_reset);
    device_class_set_props(dc, sc6530_usb_properties);
}

static const TypeInfo sc6530_usb_info = {
    .name          = TYPE_SC6530_USB,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Sc6530UsbState),
    .instance_init = sc6530_usb_init,
    .class_init    = sc6530_usb_class_init,
};

static void sc6530_usb_register_types(void)
{
    type_register_static(&sc6530_usb_info);
}

type_init(sc6530_usb_register_types)
