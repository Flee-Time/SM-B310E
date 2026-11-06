/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2026 by B310E-OS project
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/

/*
 * SC6530C USB device controller ("sx_udc"), full speed, programmed I/O.
 *
 * Register map and sequences from the stock firmware's own drivers (NOR
 * sx_udc.c 0x1efa8-0x20b44, vcom_usb.c 0x3862c-0x38f14, boot loader
 * 0x9b3c-0xa2cc); docs/usb.md has the details.
 *
 * Fixed endpoints, 64-byte FIFOs: EP0 control, EP1 IN, EP2 OUT, EP3 IN,
 * EP4 OUT. On the bulk endpoints every packet is its own transfer: packet
 * size and transfer size set to the packet length, FIFO filled (words, wire
 * bytes most significant first), then TX start; the controller raises
 * "transfer end" (bit 9) when the host has taken it. OUT works the same
 * way: buffer ready, one packet, transfer end, length in CTRL[10:0]. EP0
 * sets TR_SIZE to the whole data stage instead and moves on at each
 * packet's "transaction end" (bit 0), as the boot loader does.
 *
 * The controller answers SET_ADDRESS, SET_CONFIGURATION and every control
 * status stage itself; on the phone neither request raises a SETUP. The
 * status stages the core arms complete in software (a control-write status
 * at once, a control-read status when its data stage has gone out), and
 * the core learns of the configuration and of bus resets from the requests
 * around them (ep0_setup()).
 */

#include "config.h"
#include "system.h"
#include "kernel.h"
#include "string.h"
#include "usb.h"
#include "usb_ch9.h"
#include "usb_core.h"
#include "usb_drv.h"
#include "logf.h"

/* Bring-up stages, interrupt counters, SETUPs and throughput logged to
 * /usb-log.txt on the card (see diag_log()). Off by default: it writes the
 * card on every connection. Define it to investigate USB on hardware. */
/* #define USB_SC6530_DIAG */

#ifdef USB_SC6530_DIAG
#include <stdarg.h>
#include <stdio.h>
#include "file.h"
#endif

/* bring-up log events (see diag_event()) */
static void diag_event(char type, uint32_t a, uint32_t b, uint32_t c);
#define diag_setup(lo, hi) diag_event('S', (lo), (hi), 0)
static void diag_bulk(char type, uint32_t a, uint32_t b);

#define REG32(a)         (*(volatile uint32_t *)(a))
#define USB_REG(o)       (*(volatile uint32_t *)(0x20300000 + (o)))
#define USB_FIFO(o)      (*(volatile uint32_t *)(0x20380000 + (o)))

#define USB_CTRL         0x00
#define USB_FIFO_RST     0x0c
#define USB_INT_STS      0x18
#define USB_INT_CLR      0x1c
#define USB_TIMEOUT_LMT  0x28
#define USB_SETUP_LO     0x5c
#define USB_SETUP_HI     0x60

#define CTRL_ENABLE      (1 << 0)
#define CTRL_CONNECT     (1 << 1)    /* D+ pull-up (stock 0x20b44) */

/* endpoint CTRL bits */
#define EPCTRL_COUNT     0x7ff
#define EPCTRL_SIZE(n)   (((n) & 0x7ff) << 12)
#define EPCTRL_SIZE_MASK (0x7ff << 12)
#define EPCTRL_TYPE_MASK (3 << 23)
#define EPCTRL_ENABLE    (1 << 25)
#define EPCTRL_TX        (1 << 27)
#define EPCTRL_RX        (1 << 28)
#define EPCTRL_STALL     (1 << 29)

/* endpoint interrupt bits */
#define EPINT_XACT       (1 << 0)    /* one packet (transaction) done */
#define EPINT_SETUP      (1 << 8)
#define EPINT_DONE       (1 << 9)
/* OUT: the host tried to send while buffer ready was clear (stock EP2/EP4
 * handlers 0x1fc4a/0x1fe80 start their transfer on it) */
#define EPINT_OUT_TOKEN  (1 << 12)
#define EPINT_ALL        0x3fff

#define EP_PKT           64
#define USB_IRQ_LINE     25

/* Analog-die USB LDO: bit 4 of the LDO power-down set/clear pair
 * (stock 0x3862c); bit 0 of the same pair is the SD card LDO. */
#define ANA_LDO_PD_SET   0x82001184
#define ANA_LDO_PD_CLR   0x820011a4
#define ANA_LDO_USB      (1 << 4)

#define AHB_EB_SET       0x20500060
#define AHB_EB_CLR       0x20500070
#define AHB_USB_EB       (1 << 8)    /* clock device "USB" enable (0x67c6c) */
#define AHB_USB_EB2      (1 << 4)    /* set by vcom_usb init (0x38b62) */
#define APB_MISC_CTL0    0x8b000220
#define APB_USB_PD       (1 << 6)    /* cleared by vcom_usb init */

struct ep_regs
{
    uint16_t ctrl, trsize, int_ctrl, int_sts, int_clr;
    uint8_t fifo;       /* FIFO window offset */
    uint8_t fifo_rst;   /* FIFO_RST bit */
};

/* [ep][dir]: EP0 has both directions on one register set */
static const struct ep_regs ep_regs[USB_NUM_ENDPOINTS][2] =
{
    [0] = {
        [DIR_OUT] = { 0x64, 0x40, 0x68, 0x6c, 0x70, 0x0c, 1 << 1 },
        [DIR_IN]  = { 0x64, 0x40, 0x68, 0x6c, 0x70, 0x00, 1 << 0 },
    },
    [1] = { [DIR_IN]  = { 0xc0,  0xc8,  0xcc,  0xd0,  0xd4,  0x04, 1 << 2 } },
    [2] = { [DIR_OUT] = { 0x100, 0x108, 0x10c, 0x110, 0x114, 0x10, 1 << 3 } },
    [3] = { [DIR_IN]  = { 0x140, 0x148, 0x14c, 0x150, 0x154, 0x08, 1 << 4 } },
    [4] = { [DIR_OUT] = { 0x180, 0x188, 0x18c, 0x190, 0x194, 0x14, 1 << 5 } },
};

struct usb_drv_ep_spec usb_drv_ep_specs[USB_NUM_ENDPOINTS] =
{
    {.type = {USB_ENDPOINT_XFER_CONTROL, USB_ENDPOINT_XFER_CONTROL}},
    {.type = {USB_ENDPOINT_TYPE_NONE, USB_ENDPOINT_TYPE_ANY}},
    {.type = {USB_ENDPOINT_TYPE_ANY, USB_ENDPOINT_TYPE_NONE}},
    {.type = {USB_ENDPOINT_TYPE_NONE, USB_ENDPOINT_TYPE_ANY}},
    {.type = {USB_ENDPOINT_TYPE_ANY, USB_ENDPOINT_TYPE_NONE}},
};
uint8_t usb_drv_ep_specs_flags = 0;

struct endpoint
{
    uint8_t *buf;
    int len;                /* requested length */
    int done;               /* bytes moved so far */
    int last;               /* length of the packet in flight */
    volatile bool busy;
    bool armed;             /* OUT: buffer ready set, packet expected */
    bool block;
    bool zlp;               /* IN: end with a zero length packet */
    bool dma;               /* the (rest of the) transfer runs by DMA */
    bool dma_next;          /* OUT: DMA may take over after this packet */
    bool dma_wait;          /* OUT: waiting for the host's next send */
    int dma_base;           /* OUT: bytes moved by PIO before the DMA */
    int result;
    struct semaphore complete;
};

static struct endpoint endpoints[USB_NUM_ENDPOINTS][2];

/* EP0: wLength of the current request, and a control-read status stage
 * armed by the core that completes when the data stage has gone out */
static uint16_t ep0_wlength;
static bool ep0_status_pending;

/* Configured since the last bus reset. The controller completes
 * SET_ADDRESS and SET_CONFIGURATION itself without a SETUP interrupt
 * (phone log, 2026-11-02), so the core is told about the configuration
 * when the first request that needs one arrives. */
static bool set_config_seen;

#define REG(ep, dir, r) USB_REG(ep_regs[ep][dir].r)

/* ---- analog-die access (bounded ADI mailbox) -------------------------- */

#define ADI_FIFO_STS     0x82000020
#define ADI_FIFO_FULL    (1 << 9)
#define ADI_FIFO_EMPTY   (1 << 8)
#define ADI_BUDGET       1000000u

static bool adi_read(uint32_t addr, uint32_t *value)
{
    int old = disable_irq_save();
    uint32_t n = ADI_BUDGET, data;
    bool ok = false;
    while (!(REG32(ADI_FIFO_STS) & ADI_FIFO_EMPTY))
        if (--n == 0) goto done;
    REG32(0x82000018) = addr & 0xfff;
    n = ADI_BUDGET;
    do {
        data = REG32(0x8200001c);
        if (!(data & (1u << 31))) {
            ok = ((data >> 16) & 0xfff) == (addr & 0xfff);
            if (ok) *value = data & 0xffff;
            break;
        }
    } while (--n);
done:
    restore_irq(old);
    return ok;
}

static bool adi_write(uint32_t addr, uint32_t val)
{
    uint32_t n = ADI_BUDGET;

    while (REG32(ADI_FIFO_STS) & ADI_FIFO_FULL)
        if (--n == 0) return false;
    REG32(addr) = val;
    n = ADI_BUDGET;
    while (!(REG32(ADI_FIFO_STS) & ADI_FIFO_EMPTY))
        if (--n == 0) return false;
    return true;
}

/* stock 0x3862c: clear one register of the pair, then set the other */
static void usb_ldo(bool on)
{
    uint32_t v;
    uint32_t clear = on ? ANA_LDO_PD_SET : ANA_LDO_PD_CLR;
    uint32_t set = on ? ANA_LDO_PD_CLR : ANA_LDO_PD_SET;

    if (adi_read(clear, &v))
        adi_write(clear, v & ~ANA_LDO_USB);
    if (adi_read(set, &v))
        adi_write(set, v | ANA_LDO_USB);
}

/* ---- FIFO access ------------------------------------------------------ */

/* The FIFO takes words most significant byte first (wire order). Aligned
 * buffers (all bulk data) go a word at a time; anything else by bytes. */
static void fifo_write(int ep, const uint8_t *src, int len)
{
    volatile uint32_t *fifo = &USB_FIFO(ep_regs[ep][DIR_IN].fifo);

    if (((uintptr_t)src & 3) == 0) {
        const uint32_t *s = (const uint32_t *)src;
        for (int i = 0; i < len; i += 4)
            *fifo = __builtin_bswap32(*s++);
        return;
    }
    for (int i = 0; i < len; i += 4, src += 4)
        *fifo = (uint32_t)src[0] << 24 | (uint32_t)src[1] << 16 |
                (uint32_t)src[2] << 8 | src[3];
}

static void fifo_read(int ep, uint8_t *dst, int len)
{
    volatile uint32_t *fifo = &USB_FIFO(ep_regs[ep][DIR_OUT].fifo);

    if (((uintptr_t)dst & 3) == 0) {
        uint32_t *d = (uint32_t *)dst;
        for (; len >= 4; len -= 4)
            *d++ = __builtin_bswap32(*fifo);
        dst = (uint8_t *)d;
    }
    while (len > 0) {
        uint32_t w = *fifo;
        for (int i = 0; i < 4 && len > 0; i++, len--)
            *dst++ = w >> (24 - 8 * i);
    }
}

/* ---- transfers -------------------------------------------------------- */

static void complete(int ep, int dir, int result)
{
    struct endpoint *e = &endpoints[ep][dir];

    e->busy = false;
    e->result = result;
    usb_core_transfer_complete(ep, dir == DIR_IN ? USB_DIR_IN : USB_DIR_OUT,
                               result, result ? 0 : e->done);
    if (e->block)
        semaphore_release(&e->complete);
}

/* queue the next IN packet; the caller has checked there is one */
static void in_packet(int ep)
{
    struct endpoint *e = &endpoints[ep][DIR_IN];
    int n = MIN(e->len - e->done, EP_PKT);
    uint32_t ctrl;

    e->last = n;
    if (e->len - e->done == 0)
        e->zlp = false;                 /* this is the zero length packet */
    REG(ep, DIR_IN, int_clr) = EPINT_ALL & ~EPINT_SETUP;
    ctrl = REG(ep, DIR_IN, ctrl);
    REG(ep, DIR_IN, ctrl) = (ctrl & ~EPCTRL_SIZE_MASK) | EPCTRL_SIZE(n);
    /* EP0's TR_SIZE holds the whole data stage (start_send()): the
     * controller ends the stage once that many bytes are out, so a
     * per-packet size cut every answer at 64 bytes on the phone. */
    if (ep != 0)
        REG(ep, DIR_IN, trsize) = n;
    USB_REG(USB_FIFO_RST) |= ep_regs[ep][DIR_IN].fifo_rst;
    fifo_write(ep, e->buf + e->done, n);
    REG(ep, DIR_IN, ctrl) |= EPCTRL_TX;
}

static void out_arm(int ep)
{
    uint32_t ctrl = REG(ep, DIR_OUT, ctrl);

    if (ep != 0)
        REG(ep, DIR_OUT, trsize) = EP_PKT;
    endpoints[ep][DIR_OUT].armed = true;
    REG(ep, DIR_OUT, ctrl) = (ctrl & ~EPCTRL_SIZE_MASK) |
                             EPCTRL_SIZE(EP_PKT) | EPCTRL_RX;
}

static void diag_packet(bool in, int bytes);   /* bring-up log */

static void dma_stop(int ep);

/* ---- USB write DMA vs the SD card's DMA --------------------------------
 * The phone froze (2026-11-06) the first time the bulk OUT DMA streamed
 * while Rockbox's mass storage wrote the previous chunk to the card, i.e.
 * USB DMA into PSRAM alongside the SDIO DMA; this board already hangs on
 * other SDIO DMA contention. The two are serialized: the SD driver waits
 * while a USB OUT DMA runs (sc6530_usb_out_dma), and a USB OUT DMA that
 * becomes ready while the SD driver is busy (sd_busy_now) waits until it
 * ends (sc6530_usb_sd_idle). */
extern volatile bool sd_busy_now;
volatile bool sc6530_usb_out_dma;
static int out_kick_ep;             /* OUT DMA held back for the SD driver */
static unsigned long out_held;      /* how often (log) */

/* ---- freeze recorder (test builds with the card log) -------------------
 * Breadcrumbs in the last 4 KB of IRAM (nothing else uses it), kept from
 * the start of a USB session to its clean exit. A hardware watchdog,
 * fed from the tick, resets a frozen phone after ~4 s; the next session's
 * log then lists the breadcrumbs of the one that never ended. */
#ifdef USB_SC6530_DIAG
#define CRUMB_MAGIC   0x55534243u          /* "USBC" */
#define CRUMB_COUNT   64
struct usb_crumbs {
    uint32_t magic, n;
    uint32_t e[CRUMB_COUNT][3];            /* tick, code, value */
};
#define CRUMBS ((volatile struct usb_crumbs *)0x4003f000u)

static void crumb(char code, uint32_t value)
{
    int old = disable_irq_save();
    if (CRUMBS->magic == CRUMB_MAGIC) {
        volatile uint32_t *e = CRUMBS->e[CRUMBS->n % CRUMB_COUNT];
        e[0] = current_tick;
        e[1] = code;
        e[2] = value;
        CRUMBS->n++;
    }
    restore_irq(old);
}
#else
static inline void crumb(char code, uint32_t value) { (void)code; (void)value; }
#endif

static void in_done(int ep)
{
    struct endpoint *e = &endpoints[ep][DIR_IN];

    if (!e->busy)
        return;
    diag_packet(true, e->last);
    e->done += e->last;
    if (e->dma)
        dma_stop(ep);
    if (e->done < e->len || e->zlp) {
        in_packet(ep);
        return;
    }
    if (ep == 0)
        diag_event('C', e->done, 0, 0);
    else
        diag_bulk('b', ep | 0x100, e->done);
    complete(ep, DIR_IN, 0);
    if (ep == 0 && ep0_status_pending) {
        ep0_status_pending = false;
        endpoints[0][DIR_OUT].done = 0;
        complete(0, DIR_OUT, 0);
    }
}

static void dma_out_done(int ep);
static bool dma_usable_out(int ep, const void *ptr, int length);
static void dma_wait_out(int ep);

static void out_done(int ep)
{
    struct endpoint *e = &endpoints[ep][DIR_OUT];
    int n;

    if (!e->busy || !e->armed)
        return;
    if (e->dma) {
        dma_out_done(ep);
        return;
    }
    e->armed = false;
    diag_packet(false, REG(ep, DIR_OUT, ctrl) & EPCTRL_COUNT);
    n = REG(ep, DIR_OUT, ctrl) & EPCTRL_COUNT;
    if (ep == 0)
        n = MIN(e->len - e->done, EP_PKT);
    n = MIN(n, e->len - e->done);
    fifo_read(ep, e->buf + e->done, n);
    e->done += n;
    if (e->done < e->len && n == EP_PKT) {
        /* The first packet was full: a data phase, not a short command
         * block. Hand the rest to the DMA, which then only ever sees full
         * packets. */
        if (e->dma_next && dma_usable_out(ep, e->buf + e->done,
                                          e->len - e->done)) {
            e->dma_next = false;
            dma_wait_out(ep);
            return;
        }
        e->dma_next = false;
        out_arm(ep);
        return;
    }
    e->dma_next = false;
    if (ep != 0)
        diag_bulk('b', ep, e->done);
    complete(ep, DIR_OUT, 0);
}

/* Bulk IN by DMA, as the stock driver does (EP3 0x1f1c6). With one 64-byte
 * FIFO the controller can only answer "not ready" while software reloads
 * it; the phone stayed at 225 KB/s with or without bursts (2026-11-02).
 * The DMA refills the FIFO as soon as it drains.
 *
 * Channel registers at 0x20100fc0 + id * 0x40, ids 1..32 (stock
 * dma_phy_v5.c; Rockbox's PCM uses ids 3 and 4). The SC6530C encoding of
 * the fragment register (stock 0x32a68, the branch for chip ids other
 * than 0x65300000; Rockbox's PCM 0x50300140 decodes the same way):
 *   [31:30] source width, [29:28] destination width (2 = 32-bit),
 *   [27:26] 1: set only by the stock USB driver, taken to be the byte
 *   swap into the FIFO (wire order is most significant byte first),
 *   bits 20+21: destination address fixed, [15:0] bytes per request.
 * The endpoint's FIFO raises request 17 + FIFO index, routed by writing
 * the channel id to 0x20102040 + 4 * index (stock 0x1f092). Completion is
 * the endpoint's transfer end at TRANS_SIZE; the channel interrupts stay
 * off (stock 0x1a9fc(ch, 0, 0)). */
#define DMA_CH(id, off)   REG32(0x20100fc0 + (id) * 0x40 + (off))
#define DMA_CFG           0x08
#define DMA_INT           0x0c
#define DMA_SRC           0x10
#define DMA_DST           0x14
#define DMA_FRAG          0x18
#define DMA_BLOCK         0x1c
#define DMA_REQ_MAP(k)    REG32(0x20102040 + 4 * (k))
#define DMA_INT_CLEAR     0x1f000000
/* 32/32-bit, swap, dst fixed, 64 B per request: EP1's own routine
 * (0x1f288) and both OUT routines use a whole packet per request; the
 * first DMA build copied EP3's 32 B (0x1f1c6) and measured 70 KB/s. */
#define DMA_FRAG_USB_IN   0xa4300040u
/* Bulk OUT (stock EP2 0x1f454, EP4 0x1f524): the same 32-bit widths, swap
 * and 64 B requests, but the SOURCE (the FIFO) fixed: the stock encoder
 * 0x32a68 sets bit 20 alone for "source fixed", bits 20+21 for
 * "destination fixed". */
#define DMA_FRAG_USB_OUT  0xa4100040u
/* Every stock bulk DMA start sets the low half of the endpoint's register
 * +0x1c to 400 (0x1f288, 0x1f454, 0x1f524); PIO never touches it. Its
 * meaning is unknown. */
#define EP_DMA_REG        0x1c
#define EP_DMA_VALUE      400
#define AHB_DMA_EB        (1 << 0)    /* also set by the PCM driver */
#define USB_DMA_MIN       512         /* sector data; the rest stays PIO */

/* Bulk IN by DMA: off. Measured on the phone (docs/usb.md "Hardware
 * report 13") it ran at ~138 KB/s, slower than PIO (225-350 KB/s), as the
 * tenth build found. Stock reads no faster with it.
 *
 * Bulk OUT by DMA: OFF, and NOT SAFE on this phone. Two attempts on
 * 2026-11-06 (docs/usb.md "Hardware report 15") both trickled at ~0 KB/s,
 * and both wrote garbage into the card's root directory: 128 bytes (two
 * packets) of one word repeated, the last word of the packet before.
 * That is what reading the FIFO while it is empty returns: the channel
 * reads it whether or not a packet has arrived, so its requests are not
 * paced by the controller as configured here. The controller's count
 * still said "complete", so the full-count check cannot catch it.
 * Stock's waiting for the host's send attempt (EPINT_OUT_TOKEN) did not
 * change this. Do not turn this on again until a hardware test (on a
 * scratch card) shows the channel waits for each packet. */
#ifndef USB_SC6530_DMA_IN
#define USB_SC6530_DMA_IN 0
#endif
#ifndef USB_SC6530_DMA_OUT
#define USB_SC6530_DMA_OUT 0
#endif

/* DMA channel id per bulk IN / OUT endpoint, and FIFO index per endpoint
 * (Rockbox's PCM uses ids 3..6) */
static const uint8_t dma_id[USB_NUM_ENDPOINTS]   = { 0, 7, 0, 8, 0 };
static const uint8_t dma_id_out[USB_NUM_ENDPOINTS] = { 0, 0, 9, 0, 10 };
static const uint8_t fifo_index[USB_NUM_ENDPOINTS] = { 0, 2, 3, 4, 5 };

/* Should a DMA transfer never finish (the hardware does not do what the
 * stock code implies), DMA is given up for the rest of the connection. */
#define DMA_TIMEOUT       (HZ / 2)
static long dma_deadline, dma_deadline_out;
static int dma_out_seen;        /* OUT: received count at the last check */
static bool dma_broken;
static unsigned long dma_transfers, dma_out_transfers;

static bool dma_usable(int ep, const void *ptr, int length)
{
    return USB_SC6530_DMA_IN && dma_id[ep] && !dma_broken &&
           length >= USB_DMA_MIN &&
           length % EP_PKT == 0 && ((uintptr_t)ptr & 3) == 0;
}

/* OUT: the buffer must start on a cache line, since it is invalidated */
static bool dma_usable_out(int ep, const void *ptr, int length)
{
    return USB_SC6530_DMA_OUT && dma_id_out[ep] && !dma_broken &&
           length >= USB_DMA_MIN && length % EP_PKT == 0 &&
           ((uintptr_t)ptr & (CACHEALIGN_SIZE - 1)) == 0;
}

/* the endpoint's channel, whichever direction it runs */
static void dma_stop(int ep)
{
    int id = dma_id[ep] ? dma_id[ep] : dma_id_out[ep];

    DMA_CH(id, DMA_CFG) = 0;
    DMA_REQ_MAP(fifo_index[ep]) = 0;
    if (endpoints[ep][DIR_OUT].dma)
        sc6530_usb_out_dma = false;
    endpoints[ep][DIR_IN].dma = false;
    endpoints[ep][DIR_OUT].dma = false;
}

static void dma_start_in(int ep)
{
    struct endpoint *e = &endpoints[ep][DIR_IN];
    int id = dma_id[ep];
    uint32_t ctrl;

    e->dma = true;
    e->last = e->len;               /* in_done() counts the transfer once */
    dma_transfers++;
    dma_deadline = current_tick + DMA_TIMEOUT;
    commit_dcache_range(e->buf, e->len);

    DMA_CH(id, DMA_CFG) = 0;
    DMA_CH(id, DMA_INT) = DMA_INT_CLEAR;
    DMA_CH(id, DMA_SRC) = (uint32_t)e->buf;
    DMA_CH(id, DMA_DST) = 0x20380000 + ep_regs[ep][DIR_IN].fifo;
    DMA_CH(id, DMA_FRAG) = DMA_FRAG_USB_IN;
    DMA_CH(id, DMA_BLOCK) = e->len;
    DMA_REQ_MAP(fifo_index[ep]) = id;
    DMA_CH(id, DMA_CFG) = 1;

    /* stock 0x1f288: packets of 64, TRANS_SIZE the whole length, one TX */
    REG(ep, DIR_IN, int_clr) = EPINT_ALL;
    USB_REG(USB_FIFO_RST) |= ep_regs[ep][DIR_IN].fifo_rst;
    USB_REG(ep_regs[ep][DIR_IN].ctrl + EP_DMA_REG) =
        (USB_REG(ep_regs[ep][DIR_IN].ctrl + EP_DMA_REG) & 0xffff0000) |
        EP_DMA_VALUE;
    ctrl = REG(ep, DIR_IN, ctrl);
    REG(ep, DIR_IN, ctrl) = (ctrl & ~EPCTRL_SIZE_MASK) | EPCTRL_SIZE(EP_PKT);
    REG(ep, DIR_IN, trsize) = e->len;
    REG(ep, DIR_IN, ctrl) |= EPCTRL_TX;
}

/* OUT waits on the host, so its watchdog only fires after a whole second
 * without any packet arriving */
#define DMA_OUT_TIMEOUT   HZ

/* After a full first packet by PIO: buffer ready stays clear, so the
 * host's next packet is refused and raises EPINT_OUT_TOKEN, which starts
 * the DMA (stock 0x1fc4a: bit 12 -> 0x1f454, then bit 9 replaces bit 12
 * in the interrupt enables). */
static void dma_wait_out(int ep)
{
    struct endpoint *e = &endpoints[ep][DIR_OUT];

    e->dma_wait = true;
    e->armed = false;
    crumb('W', e->len - e->done);
    dma_out_seen = 0;
    dma_deadline_out = current_tick + DMA_OUT_TIMEOUT;
    REG(ep, DIR_OUT, int_clr) = EPINT_OUT_TOKEN;
    REG(ep, DIR_OUT, int_ctrl) = EPINT_DONE | EPINT_OUT_TOKEN;
}

/* Bulk OUT by DMA (stock 0x1f454): channel from the FIFO into the buffer
 * in 64 B requests, +0x1c = 400, packets of 64, TRANS_SIZE the whole
 * length, buffer ready once. Each packet is drained by the DMA as it
 * lands, so the controller takes the next without the CPU; transfer end
 * (bit 9) at TRANS_SIZE is the completion. Here it moves the rest of a
 * transfer whose first packet came by PIO (e->done bytes so far). */
static void dma_start_out(int ep)
{
    struct endpoint *e = &endpoints[ep][DIR_OUT];
    int id = dma_id_out[ep];
    int len = e->len - e->done;
    uint32_t ctrl;

    e->dma_wait = false;
    e->dma = true;
    sc6530_usb_out_dma = true;
    crumb('D', len);
    e->armed = true;
    e->dma_base = e->done;
    dma_out_transfers++;
    dma_out_seen = 0;
    dma_deadline_out = current_tick + DMA_OUT_TIMEOUT;
    REG(ep, DIR_OUT, int_ctrl) = EPINT_DONE;
    /* no dirty line may be written back over the DMA's data later */
    commit_discard_dcache_range(e->buf + e->done, len);

    DMA_CH(id, DMA_CFG) = 0;
    DMA_CH(id, DMA_INT) = DMA_INT_CLEAR;
    DMA_CH(id, DMA_SRC) = 0x20380000 + ep_regs[ep][DIR_OUT].fifo;
    DMA_CH(id, DMA_DST) = (uint32_t)(e->buf + e->done);
    DMA_CH(id, DMA_FRAG) = DMA_FRAG_USB_OUT;
    DMA_CH(id, DMA_BLOCK) = len;
    DMA_REQ_MAP(fifo_index[ep]) = id;
    DMA_CH(id, DMA_CFG) = 1;

    REG(ep, DIR_OUT, int_clr) = EPINT_ALL;
    USB_REG(ep_regs[ep][DIR_OUT].ctrl + EP_DMA_REG) =
        (USB_REG(ep_regs[ep][DIR_OUT].ctrl + EP_DMA_REG) & 0xffff0000) |
        EP_DMA_VALUE;
    ctrl = REG(ep, DIR_OUT, ctrl);
    REG(ep, DIR_OUT, ctrl) = (ctrl & ~EPCTRL_SIZE_MASK) | EPCTRL_SIZE(EP_PKT);
    REG(ep, DIR_OUT, trsize) = len;
    REG(ep, DIR_OUT, ctrl) |= EPCTRL_RX;
}

/* transfer end: let the channel finish the last packet, then hand over */
static void dma_out_done(int ep)
{
    struct endpoint *e = &endpoints[ep][DIR_OUT];
    int id = dma_id_out[ep];
    int len = e->len - e->dma_base;
    int n = USB_REG(ep_regs[ep][DIR_OUT].ctrl + 0x04) & 0x1ffff;

    (void)id;
    /* Stock takes transfer end as the data being in memory and stops the
     * channel (0x1fc4a). Only the full count is a success; anything else
     * fails the receive, so nothing of it reaches the card. */
    crumb('E', n);
    dma_stop(ep);
    e->armed = false;
    discard_dcache_range(e->buf + e->dma_base, len);
    if (n != len) {
        dma_broken = true;
        diag_event('Z', ep | 0x80, len, n);
        complete(ep, DIR_OUT, -1);
        return;
    }
    diag_packet(false, n);
    e->done = e->len;
    diag_bulk('b', ep, e->done);
    complete(ep, DIR_OUT, 0);
}

/* tick: a DMA transfer that never ends switches the connection to PIO */
static void dma_watchdog(void)
{
    for (int ep = 1; ep < USB_NUM_ENDPOINTS; ep++) {
        struct endpoint *e = &endpoints[ep][DIR_IN];
        if (e->busy && e->dma && TIME_AFTER(current_tick, dma_deadline)) {
            dma_broken = true;
            diag_event('Z', ep, e->len, REG(ep, DIR_IN, ctrl));
            dma_stop(ep);
            /* start the transfer over by PIO: a DMA that never ends has
             * most likely never fed the FIFO at all */
            REG(ep, DIR_IN, ctrl) &= ~EPCTRL_TX;
            e->done = 0;
            in_packet(ep);
        }
    }
    /* OUT: no progress for DMA_OUT_TIMEOUT, waiting for the host's send
     * attempt or receiving by DMA. Fail the receive: Rockbox's mass storage
     * then writes nothing to the card, the host resets the device and
     * retries, and DMA stays off for the rest of the connection. */
    for (int ep = 1; ep < USB_NUM_ENDPOINTS; ep++) {
        struct endpoint *e = &endpoints[ep][DIR_OUT];
        int rx;

        if (!e->busy || !(e->dma || e->dma_wait))
            continue;
        if (out_kick_ep == ep) {        /* held back for the SD driver */
            dma_deadline_out = current_tick + DMA_OUT_TIMEOUT;
            continue;
        }
        rx = e->dma ? USB_REG(ep_regs[ep][DIR_OUT].ctrl + 0x04) & 0x1ffff : 0;
        if (rx != dma_out_seen) {
            dma_out_seen = rx;
            dma_deadline_out = current_tick + DMA_OUT_TIMEOUT;
            continue;
        }
        if (!TIME_AFTER(current_tick, dma_deadline_out))
            continue;
        dma_broken = true;
        crumb('F', rx);
        diag_event('Z', ep | 0x80, e->len, rx | (e->dma_wait ? 0x80000000 : 0));
        REG(ep, DIR_OUT, ctrl) &= ~EPCTRL_RX;
        REG(ep, DIR_OUT, int_ctrl) = EPINT_DONE;
        if (e->dma)
            dma_stop(ep);
        e->dma_wait = false;
        e->armed = false;
        complete(ep, DIR_OUT, -1);
    }
}

/* One interrupt per bulk packet, no polling. Earlier builds fed further
 * packets from the interrupt that ended one ("bursts": up to 32 packets,
 * 3000 polls each). On the phone (2026-11-06, docs/usb.md "Hardware report
 * 16") reads ran at 230 KB/s with or without them, as the PC sets that
 * pace, while the polling took 60-67% of the CPU with interrupts off; a
 * build that also tried no polling wrote at up to 710 KB/s, against 347
 * KB/s with bursts alone. */

#ifdef USB_SC6530_DIAG
/* Timer 2 (the tick) counts down from its load at 26 MHz: the card log's
 * share of time spent in the USB interrupt. */
#define T26_VALUE   REG32(0x81000044)
#define T26_LOAD    (26000u * (1000 / HZ))
#define T26_EXPIRED (REG32(0x8100004c) & 4)   /* tick pending (raw) */
static unsigned long irq_cycles;
#endif

static int start_send(int ep, void *ptr, int length, bool block)
{
    struct endpoint *e = &endpoints[ep][DIR_IN];
    int old;

    if (ep == 0) {
        if (length > ep0_wlength)
            length = ep0_wlength;
        if (length == 0) {
            /* status stage of a control write: done by the controller */
            old = disable_irq_save();
            e->buf = ptr;
            e->len = e->done = 0;
            e->block = false;
            complete(0, DIR_IN, 0);
            restore_irq(old);
            return 0;
        }
    }

    if (block)
        semaphore_wait(&e->complete, TIMEOUT_NOBLOCK);

    old = disable_irq_save();
    e->buf = ptr;
    e->len = length;
    e->done = 0;
    e->block = block;
    /* EP0: the controller ends a data stage shorter than wLength itself
     * once TR_SIZE bytes are out (boot loader 0xa1a4 never sends a ZLP) */
    e->zlp = ep != 0 && length == 0;
    e->busy = true;
    e->result = 0;
    if (ep == 0) {
        USB_REG(0x40) = length;         /* EP0 TR_SIZE: the whole stage */
        diag_event('I', length, ep0_wlength, 0);
    } else {
        diag_bulk('B', ep | 0x100, length);
    }
    e->dma = false;
    if (ep != 0 && dma_usable(ep, ptr, length))
        dma_start_in(ep);
    else
        in_packet(ep);
    restore_irq(old);

    if (!block)
        return 0;
    if (semaphore_wait(&e->complete, HZ) == OBJ_WAIT_TIMEDOUT) {
        old = disable_irq_save();
        bool pending = e->busy;
        e->busy = false;
        restore_irq(old);
        if (pending)
            return -1;
    }
    return e->result;
}

int usb_drv_send(int endpoint, void *ptr, int length)
{
    return start_send(EP_NUM(endpoint), ptr, length, true);
}

int usb_drv_send_nonblocking(int endpoint, void *ptr, int length)
{
    return start_send(EP_NUM(endpoint), ptr, length, false);
}

int usb_drv_recv_nonblocking(int endpoint, void *ptr, int length)
{
    int ep = EP_NUM(endpoint);
    struct endpoint *e = &endpoints[ep][DIR_OUT];
    int old = disable_irq_save();

    e->buf = ptr;
    e->len = length;
    e->done = 0;
    e->block = false;
    e->result = 0;
    e->armed = false;
    if (ep == 0 && length == 0) {
        /* status stage of a control read: the controller ACKs it after
         * the data stage, which in_done() reports */
        if (endpoints[0][DIR_IN].busy) {
            ep0_status_pending = true;
            e->busy = true;
        } else {
            complete(0, DIR_OUT, 0);
        }
    } else {
        e->busy = true;
        if (ep != 0)
            diag_bulk('B', ep, length);
        /* Every OUT transfer starts by PIO. Rockbox posts the same 512-byte
         * receive for a 31-byte CBW (a short packet) and for a one-block
         * WRITE; only a full first packet shows that a data phase follows,
         * and only then does the DMA take the rest (out_done). */
        e->dma = false;
        e->dma_next = ep != 0 && dma_usable_out(ep, ptr, length);
        out_arm(ep);
    }
    restore_irq(old);
    return 0;
}

/* ---- endpoints -------------------------------------------------------- */

/* stock 0x38726/0x3876e/0x387b8: 64-byte packets, type bits clear, enable */
/* endpoints in use, bit ep * 2 + dir: a bus reset wipes their setup */
static uint32_t ep_enabled;
/* the core has activated a configuration since usb_drv_init() */
static bool core_configured;

static void ep_program(int ep, int dir)
{
    uint32_t ctrl;

    REG(ep, dir, trsize) = EP_PKT;
    ctrl = REG(ep, dir, ctrl) & ~(EPCTRL_SIZE_MASK | EPCTRL_TYPE_MASK |
                                  EPCTRL_TX | EPCTRL_RX | EPCTRL_STALL);
    REG(ep, dir, ctrl) = ctrl | EPCTRL_SIZE(EP_PKT) | EPCTRL_ENABLE;
    REG(ep, dir, int_clr) = EPINT_ALL;
    REG(ep, dir, int_ctrl) = EPINT_DONE;
    USB_REG(USB_FIFO_RST) |= ep_regs[ep][dir].fifo_rst;
}

/* EP0 as stock vcom init programs it; also after every bus reset */
static void ep0_program(void)
{
    USB_REG(0x64) = (USB_REG(0x64) & ~(EPCTRL_SIZE_MASK | EPCTRL_STALL |
                                       EPCTRL_TX | EPCTRL_RX)) |
                    EPCTRL_SIZE(8);
    USB_REG(0x70) = EPINT_ALL & ~EPINT_SETUP;
    /* bit 0 ends each EP0 packet, bit 9 the whole TR_SIZE (0xa1a4) */
    USB_REG(0x68) |= EPINT_SETUP | EPINT_DONE | EPINT_XACT;
}

static void deliver_stashed_setup(void);

void usb_drv_ep_init(const struct usb_drv_ep_alloc_ctx *ctx, int endpoint)
{
    int ep = EP_NUM(endpoint);
    int dir = EP_DIR(endpoint);
    (void)ctx;

    if (ep == 0)
        return;
    ep_enabled |= 1u << (ep * 2 + dir);
    ep_program(ep, dir);
    core_configured = true;
    deliver_stashed_setup();
}

void usb_drv_ep_deinit(const struct usb_drv_ep_alloc_ctx *ctx, int endpoint)
{
    int ep = EP_NUM(endpoint);
    int dir = EP_DIR(endpoint);
    (void)ctx;

    if (ep == 0)
        return;
    ep_enabled &= ~(1u << (ep * 2 + dir));
    REG(ep, dir, int_ctrl) = 0;
    REG(ep, dir, ctrl) &= ~(EPCTRL_ENABLE | EPCTRL_TX | EPCTRL_RX);
    REG(ep, dir, int_clr) = EPINT_ALL;
}

bool usb_drv_stalled(int endpoint, bool in)
{
    int ep = EP_NUM(endpoint);

    return (REG(ep, in ? DIR_IN : DIR_OUT, ctrl) & EPCTRL_STALL) != 0;
}

/* EP1 bit 29 is the stall the stock driver clears on CLEAR_FEATURE
 * (0x1fa62); EP0 uses the same bit of its CTRL register. */
void usb_drv_stall(int endpoint, bool stall, bool in)
{
    int ep = EP_NUM(endpoint);
    int dir = in ? DIR_IN : DIR_OUT;

    if (ep == 0) {
        /* a stalled request is over: the core gets no completion */
        endpoints[0][DIR_IN].busy = false;
        endpoints[0][DIR_OUT].busy = false;
        ep0_status_pending = false;
    }
    diag_event('K', ep | (in ? 0x100 : 0), stall, 0);
    if (stall)
        REG(ep, dir, ctrl) |= EPCTRL_STALL;
    else
        REG(ep, dir, ctrl) &= ~EPCTRL_STALL;
}

static void cancel(int ep, int dir)
{
    struct endpoint *e = &endpoints[ep][dir];

    if (e->dma)
        dma_stop(ep);
    if (e->dma_wait) {
        e->dma_wait = false;
        REG(ep, dir, int_ctrl) = EPINT_DONE;
    }
    if (out_kick_ep == ep)
        out_kick_ep = 0;
    if (!e->busy)
        return;
    e->busy = false;
    e->result = -1;
    if (e->block)
        semaphore_release(&e->complete);
}

void usb_drv_cancel_all_transfers(void)
{
    int old = disable_irq_save();

    for (int ep = 1; ep < USB_NUM_ENDPOINTS; ep++) {
        for (int dir = 0; dir < 2; dir++) {
            if (!ep_regs[ep][dir].ctrl)
                continue;
            cancel(ep, dir);
            REG(ep, dir, ctrl) &= ~(EPCTRL_TX | EPCTRL_RX);
            USB_REG(USB_FIFO_RST) |= ep_regs[ep][dir].fifo_rst;
        }
    }
    restore_irq(old);
}

void usb_drv_set_test_mode(int mode)
{
    (void)mode;
}

int usb_drv_port_speed(void)
{
    return 0;   /* full speed only */
}

/* ---- control endpoint ------------------------------------------------- */

static unsigned long bus_resets;

/* a SETUP held back until the core has activated a configuration */
static union {
    struct usb_ctrlrequest req;
    uint32_t words[2];
} stashed;
#define stashed_setup (stashed.req)
#define stashed_words (stashed.words)
static volatile bool setup_stashed;
static long stash_deadline;
static bool stash_tick_on;

static void deliver_stashed_setup(void)
{
    int old = disable_irq_save();

    if (setup_stashed) {
        setup_stashed = false;
        diag_event('D', stashed_words[0], 0, 0);
        usb_core_setup_received(&stashed_setup);
    }
    restore_irq(old);
}

/* should no endpoint be initialised (a configuration without any), the
 * held request goes out after a short wait anyway */
static void dma_watchdog(void);

static void wdg_feed(void);

static void stash_tick(void)
{
    if (setup_stashed && TIME_AFTER(current_tick, stash_deadline))
        deliver_stashed_setup();
    dma_watchdog();
    wdg_feed();
}

/* SD driver: its transfer is over; start an OUT DMA it held back */
void sc6530_usb_sd_idle(void)
{
    int old = disable_irq_save();
    int ep = out_kick_ep;

    if (ep) {
        struct endpoint *e = &endpoints[ep][DIR_OUT];

        out_kick_ep = 0;
        crumb('k', ep);
        if (e->dma_wait && e->busy)
            dma_start_out(ep);
    }
    restore_irq(old);
}

/* The host has reset the bus. On this controller that wipes the endpoint
 * setup (B310E-OS usb_debug, hardware-verified on replug): program EP0 and
 * every endpoint in use again and drop the transfers in flight. The core
 * is not told: usb_core_bus_reset() defers to the USB thread and drops
 * every SETUP until then, the very requests the host re-enumerates with.
 * Instead the next request that needs a configuration re-runs
 * SET_CONFIGURATION in the core (ep0_setup()), which re-arms the class
 * drivers and keeps the card with the host. */
static void bus_reset_seen(void)
{
    bus_resets++;
    diag_event('R', 0, 0, 0);
    usb_drv_cancel_all_transfers();
    ep0_program();
    for (int ep = 1; ep < USB_NUM_ENDPOINTS; ep++)
        for (int dir = 0; dir < 2; dir++)
            if (ep_enabled & (1u << (ep * 2 + dir)))
                ep_program(ep, dir);
    set_config_seen = false;
}

static void ep0_setup(void)
{
    union {
        uint32_t words[2];
        struct usb_ctrlrequest req;
    } setup;

    setup.words[0] = USB_REG(USB_SETUP_LO);
    setup.words[1] = USB_REG(USB_SETUP_HI);
    logf("udc: setup %08lx %08lx", setup.words[0], setup.words[1]);
    diag_setup(setup.words[0], setup.words[1]);

    /* a SETUP ends the previous request, stall included */
    USB_REG(0x64) &= ~(EPCTRL_STALL | EPCTRL_TX | EPCTRL_RX);
    if (endpoints[0][DIR_OUT].busy) {
        struct endpoint *e = &endpoints[0][DIR_OUT];
        if (e->len)
            diag_event('O', e->done << 16 | e->len, USB_REG(0x64),
                       USB_REG(0x6c));
        e->busy = false;
        usb_core_transfer_complete(EP_CONTROL, USB_DIR_OUT, -1, 0);
    }
    if (endpoints[0][DIR_IN].busy) {
        struct endpoint *e = &endpoints[0][DIR_IN];
        diag_event('A', e->done << 16 | e->len, USB_REG(0x64), USB_REG(0x6c));
        e->busy = false;
        usb_core_transfer_complete(EP_CONTROL, USB_DIR_IN, -1, 0);
    }
    ep0_status_pending = false;
    if (setup_stashed) {
        diag_event('X', stashed_words[0], 0, 0);
        setup_stashed = false;          /* superseded */
    }

    if (usb_drv_is_set_address(&setup.req)) {
        /* not seen on the phone (the controller answers it), but a host
         * only addresses a device after resetting it */
        bus_reset_seen();
        usb_core_notify_set_address(setup.req.wValue & 0x7f);
        return;
    }
    /* A host opens enumeration after a bus reset with a 64-byte device
     * descriptor read. With no reset interrupt and no SET_ADDRESS to see
     * (both handled by the controller), that read is the reset. */
    if (setup.req.bRequestType == (USB_DIR_IN | USB_TYPE_STANDARD |
                                   USB_RECIP_DEVICE) &&
        setup.req.bRequest == USB_REQ_GET_DESCRIPTOR &&
        (setup.req.wValue >> 8) == USB_DT_DEVICE &&
        setup.req.wLength == 64 && set_config_seen)
        bus_reset_seen();
    if ((setup.req.bRequestType & USB_TYPE_MASK) == USB_TYPE_STANDARD &&
        setup.req.bRequest == USB_REQ_SET_CONFIGURATION) {
        set_config_seen = true;
    } else if (!set_config_seen &&
               ((setup.req.bRequestType & USB_TYPE_MASK) != USB_TYPE_STANDARD ||
                (setup.req.bRequestType & USB_RECIP_MASK) != USB_RECIP_DEVICE)) {
        /* the first request for an interface or endpoint, or a class or
         * vendor request: the host has configured the device. The request
         * waits until the core has activated the configuration: re-running
         * SET_CONFIGURATION drops every transfer completion queued before
         * it (usb_clear_pending_transfer_completion_events()), this
         * request's included, which left the phone unanswered after a
         * Windows reset. usb_drv_ep_init() delivers it. */
        set_config_seen = true;
        ep0_wlength = setup.req.wLength;
        usb_core_notify_set_config(1);
        if (core_configured) {
            stashed_setup = setup.req;
            setup_stashed = true;
            stash_deadline = current_tick + HZ / 5;
            return;
        }
        /* first configuration: nothing queued to lose (the fourth build's
         * path, which got HID going on the phone) */
        usb_core_setup_received(&setup.req);
        return;
    }
    ep0_wlength = setup.req.wLength;
    usb_core_setup_received(&setup.req);
}

/* Interrupt bookkeeping. Line 25 is only known to carry the endpoint bits
 * of INT_STS; should it stay asserted with none of them set (a device
 * event not cleared by INT_CLR), the IRQ would re-enter forever. Idle
 * interrupts at a sane rate (a 1 kHz SOF, say) are fine; IRQ_IDLE_LIMIT of
 * them within one tick is a storm: the line is masked and the controller
 * is serviced from the tick instead. The tick (line 23) is dispatched
 * first, so it keeps running through a storm. */
#define IRQ_IDLE_LIMIT 500
static unsigned long irq_total, irq_idle, setups;
static uint32_t last_sts;
static long idle_tick;
static unsigned idle_in_tick;
static bool irq_polled;

static void udc_service(void);

static void udc_poll_tick(void)
{
    udc_service();
}

void USB_UDC(void)
{
    uint32_t sts = USB_REG(USB_INT_STS);
#ifdef USB_SC6530_DIAG
    uint32_t t0 = T26_VALUE;
    bool expired0 = T26_EXPIRED;
#endif

    irq_total++;
    last_sts = sts;
    if (!(sts & 0x1f00)) {
        USB_REG(USB_INT_CLR) = 0x7f;
        irq_idle++;
        if (current_tick != idle_tick) {
            idle_tick = current_tick;
            idle_in_tick = 0;
        }
        if (++idle_in_tick >= IRQ_IDLE_LIMIT && !irq_polled) {
            REG32(0x8000000c) = 1 << USB_IRQ_LINE;
            irq_polled = true;
            tick_add_task(udc_poll_tick);
        }
        return;
    }
    udc_service();
#ifdef USB_SC6530_DIAG
    {
        /* the pending tick says the counter went through zero once
         * (the tick waits for us) */
        uint32_t now = T26_VALUE;

        irq_cycles += (T26_EXPIRED && !expired0) || t0 < now ?
                      t0 + T26_LOAD - now : t0 - now;
    }
#endif
}

static void udc_service(void)
{
    uint32_t sts = USB_REG(USB_INT_STS);

    if (sts & (1 << 8)) {
        uint32_t ep0 = USB_REG(0x6c);
        USB_REG(0x70) = ep0;
        /* completions first: a SETUP ends the request they belong to.
         * Both directions share EP0's status; only one data stage is ever
         * in flight. The TX/RX bits are not consulted: on the phone TX
         * start was still set when a multi-packet reply's first packet
         * ended, so the reply never moved on (2026-11-02 log). The boot
         * loader (0xa1a4) also steps on bit 0 alone. */
        if (endpoints[0][DIR_IN].busy && endpoints[0][DIR_IN].len > EP_PKT)
            diag_event('E', ep0, USB_REG(0x64), endpoints[0][DIR_IN].done);
        if (ep0 & (EPINT_XACT | EPINT_DONE)) {
            if (endpoints[0][DIR_IN].busy)
                in_done(0);
            else if (endpoints[0][DIR_OUT].armed)
                out_done(0);
        }
        if (ep0 & EPINT_SETUP) {
            setups++;
            ep0_setup();
        }
    }
    for (int ep = 1; ep < USB_NUM_ENDPOINTS; ep++) {
        if (!(sts & (1 << (8 + ep))))
            continue;
        int dir = ep_regs[ep][DIR_IN].ctrl ? DIR_IN : DIR_OUT;
        uint32_t s = REG(ep, dir, int_sts);
        REG(ep, dir, int_clr) = s;
        /* the status latches a refused send even while its interrupt is
         * off (e.g. the next CBW at transfer end): only an enabled one
         * counts */
        if (dir == DIR_OUT && (s & EPINT_OUT_TOKEN) &&
            (REG(ep, DIR_OUT, int_ctrl) & EPINT_OUT_TOKEN)) {
            struct endpoint *o = &endpoints[ep][DIR_OUT];

            if (!o->dma_wait || !o->busy || out_kick_ep) {
                /* not expected: never service it twice */
                REG(ep, DIR_OUT, int_ctrl) &= ~EPINT_OUT_TOKEN;
                crumb('U', s);
            } else if (sd_busy_now) {
                /* the SD driver's DMA runs: start when it ends */
                REG(ep, DIR_OUT, int_ctrl) &= ~EPINT_OUT_TOKEN;
                out_kick_ep = ep;
                out_held++;
                crumb('K', ep);
            } else {
                crumb('T', ep);
                dma_start_out(ep);
            }
        }
        if (s & EPINT_DONE) {
            if (dir == DIR_IN)
                in_done(ep);
            else
                out_done(ep);
        }
    }
    USB_REG(USB_INT_CLR) = 0x7f;
}

/* ---- bring-up diagnostics --------------------------------------------- */

#ifdef USB_SC6530_DIAG
/* A log on the card, /usb-log.txt. Every line is appended and the file
 * closed before the next register access, so after a bus stall the log
 * ends with the last stage that completed. While the PC owns the card
 * (exclusive mass storage) nothing can be written: lines wait in RAM and
 * go out once Rockbox has the card back. A thread of its own logs the
 * interrupt counters and every SETUP the host sends; the USB thread
 * answers the host and must not wait. */
#define DIAG_LOG "/usb-log.txt"

static char diag_pending[16384];
static size_t diag_pending_len;
static bool diag_dropped;
static struct mutex diag_mutex;
static bool diag_ready;

/* the last SETUPs and EP0 events, filled by the interrupt handler */
#define DIAG_SETUPS 256
static struct { char type; uint32_t a, b, c; } diag_ring[DIAG_SETUPS];
static volatile unsigned diag_setup_wr;

static volatile bool diag_exported;     /* the PC owns the card */

static void diag_flush(void)
{
    int fd;

    if (diag_exported)
        return;                 /* lines wait in RAM until the card is back */
    fd = open(DIAG_LOG, O_WRONLY | O_CREAT | O_APPEND, 0666);

    if (fd < 0)
        return;                 /* card exported or not mounted: later */
    if (write(fd, diag_pending, diag_pending_len) == (ssize_t)diag_pending_len)
        diag_pending_len = 0;
    close(fd);
}

static void diag_log(const char *fmt, ...)
{
    char line[256];
    va_list ap;
    int n = snprintf(line, sizeof(line), "%7ld ", current_tick);

    va_start(ap, fmt);
    n += vsnprintf(line + n, sizeof(line) - n - 1, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof(line) - 2)
        n = sizeof(line) - 2;
    line[n++] = '\n';

    mutex_lock(&diag_mutex);
    if (diag_dropped && diag_pending_len + 24 < sizeof(diag_pending)) {
        memcpy(diag_pending + diag_pending_len, "-- lines dropped --\n", 20);
        diag_pending_len += 20;
        diag_dropped = false;
    }
    if (diag_pending_len + n <= sizeof(diag_pending)) {
        memcpy(diag_pending + diag_pending_len, line, n);
        diag_pending_len += n;
    } else {
        diag_dropped = true;
    }
    diag_flush();
    mutex_unlock(&diag_mutex);
}

static long diag_stack[DEFAULT_STACK_SIZE * 2 / sizeof(long)];
static struct event_queue diag_queue;
static volatile bool diag_online;
static unsigned long in_packets, out_packets;

static void diag_rate(void);

static void diag_thread(void)
{
    unsigned setup_rd = 0;
    unsigned long last_irq = ~0ul;
    long next_counters = 0;
    struct queue_event ev;

    for (;;) {
        queue_wait_w_tmo(&diag_queue, &ev, HZ / 2);
        if (ev.id == SYS_USB_CONNECTED) {
            /* the card goes to the PC: keep logging, to RAM only, so the
             * lines of the session carry their real time */
            diag_log("card handed to the host");
            diag_exported = true;
            usb_acknowledge(SYS_USB_CONNECTED_ACK, ev.data);
            continue;
        }
        if (ev.id == SYS_USB_DISCONNECTED) {
            diag_exported = false;
            diag_log("card back from the host");
            continue;
        }
        if (diag_setup_wr - setup_rd > DIAG_SETUPS) {
            diag_log("-- %u setups lost --",
                     diag_setup_wr - setup_rd - DIAG_SETUPS);
            setup_rd = diag_setup_wr - DIAG_SETUPS;
        }
        while (setup_rd != diag_setup_wr) {
            unsigned i = setup_rd++ % DIAG_SETUPS;
            unsigned long a = diag_ring[i].a, b = diag_ring[i].b,
                          c = diag_ring[i].c;
            switch (diag_ring[i].type) {
            case 'S': diag_log("setup %08lx %08lx", a, b); break;
            case 'I': diag_log("ep0 in start %lu of wLength %lu", a, b); break;
            case 'C': diag_log("ep0 in done %lu bytes", a); break;
            case 'A': diag_log("ep0 in abandoned %lu/%lu ctrl %08lx sts %08lx",
                               a >> 16, a & 0xffff, b, c); break;
            case 'O': diag_log("ep0 out abandoned %lu/%lu ctrl %08lx sts %08lx",
                               a >> 16, a & 0xffff, b, c); break;
            case 'D': diag_log("held setup delivered %08lx", a); break;
            case 'X': diag_log("held setup superseded %08lx", a); break;
            case 'R': diag_log("bus reset detected"); break;
            case 'K': diag_log("stall ep %lu %s %s", a & 0xff,
                               (a >> 8) ? "in" : "out", b ? "on" : "off"); break;
            case 'E': diag_log("ep0 irq sts %08lx ctrl %08lx at %lu", a, b, c); break;
            case 'B': diag_log("ep%lu %s start %lu", a & 0xff,
                               (a >> 8) ? "in" : "out", b); break;
            case 'b': diag_log("ep%lu %s done %lu", a & 0xff,
                               (a >> 8) ? "in" : "out", b); break;
            case 'L': diag_log("-- bulk log limit reached --"); break;
            case 'Z': diag_log("dma timeout ep%lu len %lu ctrl %08lx: PIO from now",
                               a, b, c); break;
            }
        }
        /* counters every 5 s while connected, when anything moved */
        if (diag_online && TIME_AFTER(current_tick, next_counters) &&
            irq_total != last_irq) {
            next_counters = current_tick + 5 * HZ;
            last_irq = irq_total;
            diag_log("irq %lu idle %lu sts %08lx setups %lu in %lu out %lu "
                     "resets %lu%s", irq_total, irq_idle,
                     (unsigned long)last_sts, setups, in_packets, out_packets,
                     bus_resets, irq_polled ? " POLLED" : "");
            diag_rate();
        }
        if (diag_pending_len) {
            mutex_lock(&diag_mutex);
            diag_flush();
            mutex_unlock(&diag_mutex);
        }
    }
}

static void diag_start(void)
{
    if (diag_ready)
        return;
    diag_ready = true;
    mutex_init(&diag_mutex);
    queue_init(&diag_queue, true);
    create_thread(diag_thread, diag_stack, sizeof(diag_stack), 0,
                  "usb diag" IF_PRIO(, PRIORITY_BACKGROUND) IF_COP(, CPU));
    diag_log("--- B310E USB log, driver " __DATE__ " " __TIME__);
    if (CRUMBS->magic == CRUMB_MAGIC) {
        unsigned n = CRUMBS->n, first = n > CRUMB_COUNT ? n - CRUMB_COUNT : 0;

        diag_log("previous USB session never ended (freeze, watchdog "
                 "reset?): last %u of %u steps", n - first, n);
        for (unsigned i = first; i < n; i++) {
            volatile uint32_t *e = CRUMBS->e[i % CRUMB_COUNT];
            diag_log("  step %u tick %lu %c %lx", i, (unsigned long)e[0],
                     (char)e[1], (unsigned long)e[2]);
        }
        CRUMBS->magic = 0;
    }
}

static void diag_stage(int stage, const char *what)
{
    diag_start();
    diag_log("stage %d %s", stage, what);
}

/* the first bulk transfers of each connection: enough to see the mass
 * storage commands start, without flooding the log during a copy */
#define DIAG_BULK_LIMIT 400
static unsigned diag_bulk_count;

static void diag_bulk(char type, uint32_t a, uint32_t b)
{
    if (diag_bulk_count > DIAG_BULK_LIMIT)
        return;
    if (++diag_bulk_count > DIAG_BULK_LIMIT)
        diag_event('L', 0, 0, 0);
    else
        diag_event(type, a, b, 0);
}

static void diag_event(char type, uint32_t a, uint32_t b, uint32_t c)
{
    int old = disable_irq_save();
    unsigned i = diag_setup_wr % DIAG_SETUPS;

    diag_ring[i].type = type;
    diag_ring[i].a = a;
    diag_ring[i].b = b;
    diag_ring[i].c = c;
    diag_setup_wr++;
    restore_irq(old);
}

/* Where the time goes: sampled every tick while connected. SD = the card
 * driver is in a transfer (sd-sc6530c.c), IN = a bulk IN transfer is being
 * sent, OUT = a bulk OUT transfer is armed (a command or write data
 * expected), other = none of these. */
extern volatile bool sd_busy_now;
static unsigned long smp_total, smp_sd, smp_in, smp_out;
static unsigned long in_bytes, out_bytes;

static void diag_sample_tick(void)
{
    smp_total++;
    if (sd_busy_now)
        smp_sd++;
    else if (endpoints[1][DIR_IN].busy || endpoints[3][DIR_IN].busy)
        smp_in++;
    else if (endpoints[2][DIR_OUT].busy || endpoints[4][DIR_OUT].busy)
        smp_out++;
}

static void diag_rate(void)
{
    static unsigned long l_total, l_sd, l_in, l_out, l_inb, l_outb;
    unsigned long n = smp_total - l_total;

    if (!n)
        return;
    static unsigned long l_pk, l_dma, l_dma_out, l_held, l_irqc;
    unsigned long pk = in_packets + out_packets;
    /* 26 MHz cycles over n ticks, in percent */
    unsigned long span = n * (T26_LOAD / 100);
    unsigned long irq_pc = (irq_cycles - l_irqc) / (span ? span : 1);

    diag_log("%lu KB/s in, %lu KB/s out; sd %lu%% in %lu%% out %lu%% other %lu%%; "
             "irq %lu%%, %lu packets, dma %lu in %lu out transfers "
             "(%lu held for the card)%s",
             (in_bytes - l_inb) * HZ / n / 1024,
             (out_bytes - l_outb) * HZ / n / 1024,
             (smp_sd - l_sd) * 100 / n, (smp_in - l_in) * 100 / n,
             (smp_out - l_out) * 100 / n,
             (n - (smp_sd - l_sd) - (smp_in - l_in) - (smp_out - l_out)) * 100 / n,
             irq_pc, pk - l_pk, dma_transfers - l_dma,
             dma_out_transfers - l_dma_out, out_held - l_held,
             dma_broken ? " (DMA off)" : "");
    l_total = smp_total; l_sd = smp_sd; l_in = smp_in; l_out = smp_out;
    l_inb = in_bytes; l_outb = out_bytes;
    l_pk = pk; l_dma = dma_transfers;
    l_dma_out = dma_out_transfers; l_held = out_held;
    l_irqc = irq_cycles;
}

/* counters while connected */
static void diag_watch(void)
{
    if (!diag_online) {
        diag_bulk_count = 0;
        tick_add_task(diag_sample_tick);
    }
    diag_online = true;
}

static void diag_offline(void)
{
    if (!diag_online)
        return;
    tick_remove_task(diag_sample_tick);
    diag_rate();
    diag_online = false;
    diag_log("irq %lu idle %lu setups %lu in %lu out %lu resets %lu",
             irq_total, irq_idle, setups, in_packets, out_packets, bus_resets);
}

static void diag_packet(bool in, int bytes)
{
    if (in) {
        in_packets++;
        in_bytes += bytes;
    } else {
        out_packets++;
        out_bytes += bytes;
    }
}
#else
static inline void diag_event(char type, uint32_t a, uint32_t b, uint32_t c)
{
    (void)type; (void)a; (void)b; (void)c;
}

static inline void diag_bulk(char type, uint32_t a, uint32_t b)
{
    (void)type; (void)a; (void)b;
}

static inline void diag_stage(int stage, const char *what)
{
    (void)stage; (void)what;
}

static inline void diag_watch(void)
{
}

static inline void diag_offline(void)
{
}

static inline void diag_packet(bool in, int bytes)
{
    (void)in; (void)bytes;
}
#endif

/* ---- controller ------------------------------------------------------- */

void usb_drv_startup(void)
{
    for (int ep = 0; ep < USB_NUM_ENDPOINTS; ep++)
        for (int dir = 0; dir < 2; dir++)
            semaphore_init(&endpoints[ep][dir].complete, 1, 0);
}

/* The stock vcom_usb bring-up (0x38b62), step for step, with the "USB"
 * clock device's enable (0x67c6c) where it calls it. On a card boot the
 * stock firmware never ran and nothing else has touched the block. */
void usb_drv_init(void)
{
    for (int ep = 0; ep < USB_NUM_ENDPOINTS; ep++)
        for (int dir = 0; dir < 2; dir++)
            endpoints[ep][dir].busy = false;
    ep0_status_pending = false;
    set_config_seen = false;
    setup_stashed = false;
    core_configured = false;
    irq_total = irq_idle = setups = bus_resets = 0;
    idle_in_tick = 0;
    dma_broken = false;
    REG32(AHB_EB_SET) = AHB_DMA_EB;
    if (!stash_tick_on) {
        stash_tick_on = true;
        tick_add_task(stash_tick);
    }

    diag_stage(1, "ldo off");
    usb_ldo(false);
    diag_stage(2, "ahb 0x10");
    REG32(AHB_EB_SET) = AHB_USB_EB2;
    diag_stage(3, "apb 220");
    REG32(APB_MISC_CTL0) &= ~APB_USB_PD;
    diag_stage(4, "ahb 0x100");
    REG32(AHB_EB_SET) = AHB_USB_EB;
    sleep(HZ * 3 / 10);                 /* stock: 300 ms */

    /* stock sets CONNECT here too; usb_drv_connect() does it once the core
     * is ready, still before the LDO that puts the device on the bus */
    diag_stage(5, "ctrl");
    USB_REG(USB_CTRL) |= CTRL_ENABLE;
    diag_stage(6, "ep0");
    ep_enabled = 0;
    USB_REG(0x70) = EPINT_ALL;
    ep0_program();
    USB_REG(USB_TIMEOUT_LMT) = 15;
    diag_stage(7, "irq on");
    REG32(0x80000008) |= 1 << USB_IRQ_LINE;
}

/* ---- hardware watchdog for the freeze recorder (test builds) -----------
 * The SC6530 watchdog in the analog die (0x82001480, 32 kHz), set up as
 * system_reboot() does but for ~4 s, fed from the tick task. Boot disarms
 * it (system_init), and a clean session exit stops it. */
#ifdef USB_SC6530_DIAG
#define WDG               0x82001480u
#define WDG_LOAD_LOW      0x0000          /* 0x20000 / 32768 Hz = 4 s */
#define WDG_LOAD_HIGH     0x0002
static bool wdg_on;
static long wdg_next;

static void wdg_load(void)
{
    adi_write(WDG + 0x20, 0xe551);          /* unlock */
    adi_write(WDG + 4, WDG_LOAD_HIGH);
    adi_write(WDG, WDG_LOAD_LOW);
    adi_write(WDG + 0x20, ~0xe551u);
}

static void __attribute__((unused)) wdg_start(void)
{
    uint32_t ctrl = 0;

    adi_write(0x820010e0, 4);               /* watchdog clocks (SET) */
    adi_write(0x820010e4, 2);
    adi_write(WDG + 0x20, 0xe551);
    adi_read(WDG + 8, &ctrl);
    adi_write(WDG + 8, ctrl | 9);           /* reset enable */
    adi_write(WDG + 4, WDG_LOAD_HIGH);
    adi_write(WDG, WDG_LOAD_LOW);
    adi_read(WDG + 8, &ctrl);
    adi_write(WDG + 8, ctrl | 2);           /* run */
    adi_write(WDG + 0x20, ~0xe551u);
    wdg_on = true;
    wdg_next = current_tick + HZ / 2;
}

static void wdg_stop(void)
{
    uint32_t ctrl = 0;

    wdg_on = false;
    adi_write(WDG + 0x20, 0xe551);
    adi_read(WDG + 8, &ctrl);
    adi_write(WDG + 8, ctrl & ~0xfu);
    adi_write(WDG + 0x20, ~0xe551u);
}

static void wdg_feed(void)
{
    if (wdg_on && TIME_AFTER(current_tick, wdg_next)) {
        wdg_next = current_tick + HZ / 2;
        wdg_load();
    }
}

static void crumbs_begin(void)
{
    CRUMBS->n = 0;
    CRUMBS->magic = CRUMB_MAGIC;
    crumb('S', 0);
#ifdef USB_SC6530_WDG       /* not verified on the phone: off */
    wdg_start();
#endif
}

static void crumbs_end(void)
{
    wdg_stop();
    CRUMBS->magic = 0;
}
#else
static void wdg_feed(void) {}
static inline void crumbs_begin(void) {}
static inline void crumbs_end(void) {}
#endif

/* Present ourselves to the host: called by usb_enable() once the core is
 * fully initialised (see usb-sc6530c.c). Stock ends its bring-up by
 * powering the USB LDO, which is what the host sees. */
void usb_drv_connect(void)
{
    USB_REG(USB_CTRL) |= CTRL_CONNECT;
    diag_stage(8, "ldo on");
    usb_ldo(true);
    diag_stage(9, "online");
    diag_watch();
    crumbs_begin();
}

/* The AHB clock stays on: reaching a gated block stalls the bus for good,
 * and whatever touches the controller after the cable is gone (a late
 * class driver call, a stale completion) would freeze the phone. The LDO
 * is what the bus and the battery care about. */
void usb_drv_exit(void)
{
    diag_stage(10, "exit irq");
    REG32(0x8000000c) = 1 << USB_IRQ_LINE;
    if (irq_polled) {
        tick_remove_task(udc_poll_tick);
        irq_polled = false;
    }
    if (stash_tick_on) {
        tick_remove_task(stash_tick);
        stash_tick_on = false;
    }
    setup_stashed = false;
    diag_stage(11, "exit ctrl");
    USB_REG(USB_CTRL) = 0;
    usb_drv_cancel_all_transfers();
    for (int dir = 0; dir < 2; dir++)
        cancel(0, dir);
    diag_stage(12, "exit ldo");
    usb_ldo(false);
    diag_stage(13, "exit done");
    diag_offline();
    crumbs_end();
}
