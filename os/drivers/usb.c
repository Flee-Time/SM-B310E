/* B310E OS — SC6530C USB device controller ("sx_udc"), full speed,
 * programmed I/O, and the device side of enumeration.
 *
 * Ported from the Rockbox port's usb-drv-sc6530c.c, verified on the phone
 * (docs/usb.md): the register map, the stock bring-up, the FIFO byte
 * order, EP0's whole-stage TR_SIZE, one interrupt per bulk packet, the
 * bus resets inferred from the host's requests. Rockbox's USB core is
 * replaced by the few standard and class requests mass storage needs,
 * answered in the interrupt handler.
 *
 * Fixed endpoints with 64-byte FIFOs: EP0 control, EP1 IN, EP2 OUT (mass
 * storage, as in the stock descriptors), EP3 IN and EP4 OUT unused. Each
 * bulk packet is its own transfer: packet and transfer size set to its
 * length, FIFO filled (words, wire bytes most significant first), TX
 * start; "transfer end" (bit 9) when the host has taken it. OUT: buffer
 * ready, one packet, transfer end, the length in CTRL[10:0]. EP0 sets
 * TR_SIZE to the whole data stage and steps on each packet's bit 0.
 *
 * The controller answers SET_ADDRESS, SET_CONFIGURATION and every status
 * stage itself; on the phone neither request raises a SETUP. The device
 * counts as configured at the first class, interface or endpoint request
 * (GET_MAX_LUN), and a 64-byte device descriptor read on a configured
 * device is the host's bus reset, which wipes the endpoint setup.
 *
 * NEVER USB DMA: the bulk OUT DMA wrote stale FIFO words into the card's
 * root directory twice (docs/usb.md, hardware report 15). All PIO.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/arch.h"
#include "os/drivers.h"
#include "os/fs.h"
#include "os/kernel.h"
#include "os/lib.h"
#include "os/usb.h"
#include "drivers/usb_internal.h"

#define USB_REG(o)        REG32(0x20300000u + (o))
#define USB_FIFO(o)       REG32(0x20380000u + (o))

#define USB_CTRL          0x00
#define USB_FIFO_RST      0x0c
#define USB_INT_STS       0x18
#define USB_INT_CLR       0x1c
#define USB_TIMEOUT_LMT   0x28
#define USB_EP0_TRSIZE    0x40
#define USB_SETUP_LO      0x5c
#define USB_SETUP_HI      0x60
#define USB_EP0_CTRL      0x64
#define USB_EP0_INT_CTRL  0x68
#define USB_EP0_INT_STS   0x6c
#define USB_EP0_INT_CLR   0x70

#define CTRL_ENABLE       (1u << 0)
#define CTRL_CONNECT      (1u << 1)     /* D+ pull-up (stock 0x20b44) */

#define EPCTRL_COUNT      0x7ffu
#define EPCTRL_SIZE(n)    (((uint32_t)(n) & 0x7ff) << 12)
#define EPCTRL_SIZE_MASK  (0x7ffu << 12)
#define EPCTRL_TYPE_MASK  (3u << 23)
#define EPCTRL_ENABLE     (1u << 25)
#define EPCTRL_TX         (1u << 27)
#define EPCTRL_RX         (1u << 28)
#define EPCTRL_STALL      (1u << 29)

#define EPINT_XACT        (1u << 0)     /* one packet done */
#define EPINT_SETUP       (1u << 8)
#define EPINT_DONE        (1u << 9)     /* transfer done */
#define EPINT_ALL         0x3fffu

/* Analog-die USB LDO: bit 4 of the LDO power-down set/clear pair (stock
 * 0x3862c); bit 0 of the same pair is the SD card's LDO. */
#define ANA_LDO_PD_SET    0x82001184u
#define ANA_LDO_PD_CLR    0x820011a4u
#define ANA_LDO_USB       (1u << 4)

#define AHB_EB_SET        0x20500060u
#define AHB_DMA_EB        (1u << 0)
#define AHB_USB_EB2       (1u << 4)     /* vcom_usb init (0x38b62) */
#define AHB_USB_EB        (1u << 8)     /* clock device "USB" (0x67c6c) */
#define APB_MISC_CTL0     0x8b000220u
#define APB_USB_PD        (1u << 6)     /* cleared by vcom_usb init */

#define VID               0x1209        /* pid.codes test id, as Rockbox */
#define PID               0x0001

/* 500 interrupts with nothing to do within 10 ms: a storm. Line 25 is
 * masked and the session thread services the controller instead. */
#define IRQ_IDLE_LIMIT    500

struct ep_regs {
    uint16_t ctrl, trsize, int_ctrl, int_sts, int_clr;
    uint8_t fifo;               /* FIFO window offset */
    uint8_t fifo_rst;           /* FIFO_RST bit */
};

static const struct ep_regs ep0_regs = { 0x64, 0x40, 0x68, 0x6c, 0x70, 0x00, 1 << 0 };
static const struct ep_regs ep1_regs = { 0xc0, 0xc8, 0xcc, 0xd0, 0xd4, 0x04, 1 << 2 };
static const struct ep_regs ep2_regs = { 0x100, 0x108, 0x10c, 0x110, 0x114, 0x10, 1 << 3 };

struct endpoint {
    const struct ep_regs *r;
    uint8_t *buf;
    int len;                    /* requested */
    int done;                   /* moved so far */
    int last;                   /* the packet in flight */
    volatile bool busy;
    bool armed;                 /* OUT: buffer ready set */
    bool zlp;                   /* IN: a zero length packet still to go */
    volatile int result;        /* bytes, or USB_EABORT */
    sem_t sem;                  /* posted at completion */
};

static struct endpoint ep0 = { .r = &ep0_regs };
static struct endpoint ep_in = { .r = &ep1_regs };     /* EP1 */
static struct endpoint ep_out = { .r = &ep2_regs };    /* EP2 */

#define R(e, reg) USB_REG((e)->r->reg)

volatile usb_stat_t usb_stat;

static thread_t *session_thread;
static bool running;
static volatile bool stop_req;
static volatile bool polled;
static volatile bool configured;        /* bulk endpoints set up once */
static bool set_config_seen;            /* since the last bus reset */
static volatile uint32_t gen;
static volatile bool halted_in, halted_out;
static uint8_t config_value;
static uint16_t ep0_wlength;
static uint32_t idle_window, idle_count;
static sem_t cfg_sem, exited;
static uint32_t ep0_buf[16];            /* replies built at run time */

/* ---- descriptors ------------------------------------------------------- */

static const uint8_t dev_desc[18] __attribute__((aligned(4))) = {
    18, 1, 0x00, 0x02,          /* device, USB 2.0 (full speed) */
    0, 0, 0, USB_PKT,           /* class in the interface; EP0 64 bytes */
    VID & 0xff, VID >> 8, PID & 0xff, PID >> 8,
    0x00, 0x01, 1, 2, 3, 1,     /* bcdDevice 1.00, strings, 1 config */
};

static const uint8_t cfg_desc[32] __attribute__((aligned(4))) = {
    9, 2, 32, 0, 1, 1, 0, 0x80, 250,        /* bus powered, 500 mA */
    9, 4, 0, 0, 2, 0x08, 0x06, 0x50, 0,     /* mass storage, SCSI, BOT */
    7, 5, 0x81, 2, USB_PKT, 0, 0,           /* EP1 IN bulk */
    7, 5, 0x02, 2, USB_PKT, 0, 0,           /* EP2 OUT bulk */
};

static const char *const strings[] = {
    NULL, "B310E OS", "B310E OS SD card", "B310E0000001",
};

static int string_desc(unsigned idx, uint8_t *b)
{
    int n = 2;

    if (idx == 0) {                     /* languages: US English */
        b[0] = 4;
        b[1] = 3;
        b[2] = 0x09;
        b[3] = 0x04;
        return 4;
    }
    if (idx >= ARRAY_SIZE(strings))
        return -1;
    for (const char *p = strings[idx]; *p && n < 62; p++) {
        b[n++] = (uint8_t)*p;
        b[n++] = 0;
    }
    b[0] = (uint8_t)n;
    b[1] = 3;
    return n;
}

/* ---- analog die: reads with the echoed index checked ------------------- */

#define ADI_RD_CMD        0x82000018u
#define ADI_RD_DATA       0x8200001cu
#define ADI_STS           0x82000020u
#define ADI_EMPTY         (1u << 8)
#define ADI_BUDGET        1000000u

static bool adi_read_checked(uint32_t addr, uint32_t *value)
{
    uint32_t s = arch_irq_save(), n = ADI_BUDGET, data;
    bool ok = false;

    while (!(REG32(ADI_STS) & ADI_EMPTY))
        if (--n == 0)
            goto out;
    REG32(ADI_RD_CMD) = addr & 0xfff;
    n = ADI_BUDGET;
    do {
        data = REG32(ADI_RD_DATA);
        if (!(data & (1u << 31))) {
            ok = ((data >> 16) & 0xfff) == (addr & 0xfff);
            if (ok)
                *value = data & 0xffff;
            break;
        }
    } while (--n);
out:
    arch_irq_restore(s);
    return ok;
}

/* stock 0x3862c: clear one register of the pair, then set the other */
static void usb_ldo(bool on)
{
    uint32_t v;
    uint32_t clear = on ? ANA_LDO_PD_SET : ANA_LDO_PD_CLR;
    uint32_t set = on ? ANA_LDO_PD_CLR : ANA_LDO_PD_SET;

    if (adi_read_checked(clear, &v))
        adi_write(clear, v & ~ANA_LDO_USB);
    if (adi_read_checked(set, &v))
        adi_write(set, v | ANA_LDO_USB);
}

/* ---- FIFOs ------------------------------------------------------------- */

/* Words, most significant byte first. Aligned buffers a word at a time;
 * anything else by bytes. Both may read up to 3 bytes past len. */
static void fifo_write(const struct endpoint *e, const uint8_t *src, int len)
{
    volatile uint32_t *fifo = &USB_FIFO(e->r->fifo);

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

static void fifo_read(const struct endpoint *e, uint8_t *dst, int len)
{
    volatile uint32_t *fifo = &USB_FIFO(e->r->fifo);

    if (((uintptr_t)dst & 3) == 0) {
        uint32_t *d = (uint32_t *)dst;
        for (; len >= 4; len -= 4)
            *d++ = __builtin_bswap32(*fifo);
        dst = (uint8_t *)d;
    }
    while (len > 0) {
        uint32_t w = *fifo;
        for (int i = 0; i < 4 && len > 0; i++, len--)
            *dst++ = (uint8_t)(w >> (24 - 8 * i));
    }
}

/* ---- transfers (interrupts off) ---------------------------------------- */

static void complete(struct endpoint *e, int result)
{
    e->busy = false;
    e->result = result ? result : e->done;
    if (e != &ep0)
        sem_post(&e->sem);
}

static void cancel(struct endpoint *e)
{
    if (!e->busy)
        return;
    e->busy = false;
    e->armed = false;
    e->result = USB_EABORT;
    if (e != &ep0)
        sem_post(&e->sem);
}

/* queue the next IN packet */
static void in_packet(struct endpoint *e)
{
    int n = e->len - e->done < USB_PKT ? e->len - e->done : USB_PKT;
    uint32_t ctrl;

    e->last = n;
    if (n == 0)
        e->zlp = false;                 /* this is the zero length packet */
    R(e, int_clr) = EPINT_ALL & ~EPINT_SETUP;
    ctrl = R(e, ctrl);
    R(e, ctrl) = (ctrl & ~EPCTRL_SIZE_MASK) | EPCTRL_SIZE(n);
    /* EP0's TR_SIZE holds the whole data stage (ep0_send()): a per-packet
     * size cut every answer at 64 bytes on the phone */
    if (e != &ep0)
        R(e, trsize) = (uint32_t)n;
    USB_REG(USB_FIFO_RST) |= e->r->fifo_rst;
    fifo_write(e, e->buf + e->done, n);
    R(e, ctrl) |= EPCTRL_TX;
}

static void out_arm(struct endpoint *e)
{
    uint32_t ctrl = R(e, ctrl);

    R(e, trsize) = USB_PKT;
    e->armed = true;
    R(e, ctrl) = (ctrl & ~EPCTRL_SIZE_MASK) | EPCTRL_SIZE(USB_PKT) | EPCTRL_RX;
}

static void in_done(struct endpoint *e)
{
    if (!e->busy)
        return;
    e->done += e->last;
    if (e->done < e->len || e->zlp) {
        in_packet(e);
        return;
    }
    complete(e, 0);
}

static void out_done(struct endpoint *e)
{
    int n;

    if (!e->busy || !e->armed)
        return;
    e->armed = false;
    n = (int)(R(e, ctrl) & EPCTRL_COUNT);
    if (n > e->len - e->done)
        n = e->len - e->done;
    fifo_read(e, e->buf + e->done, n);
    e->done += n;
    if (e->done < e->len && n == USB_PKT) {
        out_arm(e);
        return;
    }
    complete(e, 0);
}

/* bulk endpoint as stock vcom init programs it (0x38726): 64-byte packets,
 * type bits clear, enabled, transfer end interrupt */
static void ep_program(struct endpoint *e)
{
    uint32_t ctrl;

    R(e, trsize) = USB_PKT;
    ctrl = R(e, ctrl) & ~(EPCTRL_SIZE_MASK | EPCTRL_TYPE_MASK | EPCTRL_TX |
                          EPCTRL_RX | EPCTRL_STALL);
    R(e, ctrl) = ctrl | EPCTRL_SIZE(USB_PKT) | EPCTRL_ENABLE;
    R(e, int_clr) = EPINT_ALL;
    R(e, int_ctrl) = EPINT_DONE;
    USB_REG(USB_FIFO_RST) |= e->r->fifo_rst;
}

/* EP0 as stock vcom init programs it; also after every bus reset */
static void ep0_program(void)
{
    USB_REG(USB_EP0_CTRL) = (USB_REG(USB_EP0_CTRL) &
                             ~(EPCTRL_SIZE_MASK | EPCTRL_STALL | EPCTRL_TX |
                               EPCTRL_RX)) | EPCTRL_SIZE(8);
    USB_REG(USB_EP0_INT_CLR) = EPINT_ALL & ~EPINT_SETUP;
    /* bit 0 ends each EP0 packet, bit 9 the whole TR_SIZE (0xa1a4) */
    USB_REG(USB_EP0_INT_CTRL) |= EPINT_SETUP | EPINT_DONE | EPINT_XACT;
}

/* A new generation: every bulk transfer in flight is dropped. Software
 * only, so it is safe before the controller is clocked. */
static void bulk_cancel_sw(void)
{
    gen++;
    cancel(&ep_in);
    cancel(&ep_out);
}

static void bulk_cancel(void)
{
    bulk_cancel_sw();
    R(&ep_in, ctrl) &= ~(EPCTRL_TX | EPCTRL_RX);
    R(&ep_out, ctrl) &= ~(EPCTRL_TX | EPCTRL_RX);
    USB_REG(USB_FIFO_RST) |= ep_in.r->fifo_rst | ep_out.r->fifo_rst;
}

static void configure(void)
{
    bulk_cancel();
    ep_program(&ep_in);
    ep_program(&ep_out);
    halted_in = halted_out = false;
    configured = true;
    set_config_seen = true;
    usb_stat.state = USB_CONNECTED;
    sem_post(&cfg_sem);
}

/* The host has reset the bus, which on this controller wipes the
 * endpoint setup (verified on replug): program EP0 and the bulk endpoints
 * again and drop the transfers in flight. Mass storage starts over with
 * the next command block. */
static void bus_reset_seen(void)
{
    usb_stat.bus_resets++;
    bulk_cancel();
    cancel(&ep0);
    ep0_program();
    if (configured) {
        ep_program(&ep_in);
        ep_program(&ep_out);
    }
    halted_in = halted_out = false;
    set_config_seen = false;
    sem_post(&cfg_sem);
}

/* ---- control endpoint -------------------------------------------------- */

static void ep0_stall(void)
{
    cancel(&ep0);
    USB_REG(USB_EP0_CTRL) |= EPCTRL_STALL;
}

/* the data stage of a control read; the controller ends it at TR_SIZE
 * (boot loader 0xa1a4 never sends a ZLP) and completes the status stage */
static void ep0_send(const void *data, int len)
{
    if (len > ep0_wlength)
        len = ep0_wlength;
    if (len <= 0)
        return;
    ep0.buf = (uint8_t *)data;
    ep0.len = len;
    ep0.done = 0;
    ep0.zlp = false;
    ep0.busy = true;
    USB_REG(USB_EP0_TRSIZE) = (uint32_t)len;
    in_packet(&ep0);
}

static void halt_set(bool in)
{
    struct endpoint *e = in ? &ep_in : &ep_out;

    if (in)
        halted_in = true;
    else
        halted_out = true;
    R(e, ctrl) |= EPCTRL_STALL;
}

/* CLEAR_FEATURE(ENDPOINT_HALT). A transfer queued behind the halt starts
 * now (it was never put on the hardware while the endpoint stalled). */
static void halt_clear(bool in)
{
    struct endpoint *e = in ? &ep_in : &ep_out;
    bool was = in ? halted_in : halted_out;

    R(e, ctrl) &= ~EPCTRL_STALL;
    if (in)
        halted_in = false;
    else
        halted_out = false;
    if (!was || !e->busy)
        return;
    if (in)
        in_packet(e);
    else if (!e->armed)
        out_arm(e);
}

static void standard_request(uint8_t bm, uint8_t rq, uint16_t value,
                             uint16_t index)
{
    uint8_t *b = (uint8_t *)ep0_buf;
    unsigned recip = bm & 0x1f;
    int n;

    switch (rq) {
    case 0:                             /* GET_STATUS */
        if (!(bm & 0x80))
            break;
        b[0] = b[1] = 0;
        if (recip == 2 && ((index == 0x81 && halted_in) ||
                           (index == 0x02 && halted_out)))
            b[0] = 1;
        ep0_send(b, 2);
        return;
    case 1:                             /* CLEAR_FEATURE */
    case 3:                             /* SET_FEATURE */
        if (bm & 0x80)
            break;
        if (recip == 2 && value == 0) {         /* ENDPOINT_HALT */
            if (index == 0x81 || index == 0x02) {
                if (rq == 1)
                    halt_clear(index == 0x81);
                else
                    halt_set(index == 0x81);
            }
        }
        return;                         /* remote wakeup, test mode: ignored */
    case 6:                             /* GET_DESCRIPTOR */
        if (bm != 0x80)
            break;
        switch (value >> 8) {
        case 1:
            ep0_send(dev_desc, sizeof(dev_desc));
            return;
        case 2:
            if ((value & 0xff) == 0) {
                ep0_send(cfg_desc, sizeof(cfg_desc));
                return;
            }
            break;
        case 3:
            n = string_desc(value & 0xff, b);
            if (n > 0) {
                ep0_send(b, n);
                return;
            }
            break;
        }
        break;                          /* device qualifier etc.: full speed only */
    case 8:                             /* GET_CONFIGURATION */
        if (bm != 0x80)
            break;
        b[0] = config_value;
        ep0_send(b, 1);
        return;
    case 10:                            /* GET_INTERFACE */
        if (bm != 0x81)
            break;
        b[0] = 0;
        ep0_send(b, 1);
        return;
    case 11:                            /* SET_INTERFACE */
        if (bm == 0x01 && value == 0)
            return;
        break;
    }
    ep0_stall();
}

static void ep0_setup(void)
{
    union {
        uint32_t w[2];
        uint8_t b[8];
    } req;
    uint8_t bm, rq;
    uint16_t value, index;

    req.w[0] = USB_REG(USB_SETUP_LO);
    req.w[1] = USB_REG(USB_SETUP_HI);
    bm = req.b[0];
    rq = req.b[1];
    value = (uint16_t)(req.b[2] | req.b[3] << 8);
    index = (uint16_t)(req.b[4] | req.b[5] << 8);
    ep0_wlength = (uint16_t)(req.b[6] | req.b[7] << 8);
    usb_stat.setups++;
    if (usb_stat.state == USB_WAITING)
        usb_stat.state = USB_ENUMERATING;

    /* a SETUP ends the previous request, stall included */
    USB_REG(USB_EP0_CTRL) &= ~(EPCTRL_STALL | EPCTRL_TX | EPCTRL_RX);
    cancel(&ep0);

    if (bm == 0x00 && rq == 5) {
        /* not seen on the phone (the controller answers it), but a host
         * only addresses a device after resetting it */
        bus_reset_seen();
        return;
    }
    /* A host opens enumeration after a bus reset with a 64-byte device
     * descriptor read; with no reset interrupt and no SET_ADDRESS to see,
     * that read is the reset. */
    if (bm == 0x80 && rq == 6 && (value >> 8) == 1 && ep0_wlength == 64 &&
        set_config_seen)
        bus_reset_seen();
    if ((bm & 0x60) == 0 && rq == 9) {          /* SET_CONFIGURATION */
        config_value = (uint8_t)value;
        if (value)
            configure();
        return;
    }
    if (!set_config_seen && ((bm & 0x60) != 0 || (bm & 0x1f) != 0)) {
        /* the first request for an interface or endpoint, or a class
         * request: the host has configured the device */
        config_value = 1;
        configure();
    }
    if ((bm & 0x60) == 0) {
        standard_request(bm, rq, value, index);
        return;
    }
    if ((bm & 0x7f) == 0x21 && index == 0) {    /* class, interface 0 */
        if (bm == 0xa1 && rq == 0xfe) {         /* GET_MAX_LUN */
            ((uint8_t *)ep0_buf)[0] = 0;
            ep0_send(ep0_buf, 1);
            return;
        }
        if (bm == 0x21 && rq == 0xff) {         /* Bulk-Only mass storage reset */
            bulk_cancel();
            R(&ep_in, ctrl) &= ~EPCTRL_STALL;
            R(&ep_out, ctrl) &= ~EPCTRL_STALL;
            halted_in = halted_out = false;
            usb_msc_reset();
            sem_post(&cfg_sem);
            return;
        }
    }
    ep0_stall();
}

/* ---- interrupt --------------------------------------------------------- */

static void udc_service(void)
{
    uint32_t sts = USB_REG(USB_INT_STS), s;

    if (sts & (1u << 8)) {
        s = USB_REG(USB_EP0_INT_STS);
        USB_REG(USB_EP0_INT_CLR) = s;
        /* completions first: a SETUP ends the request they belong to. The
         * TX bit is not consulted (it stays set until TR_SIZE on the
         * phone); bit 0 alone steps, as the boot loader does. */
        if ((s & (EPINT_XACT | EPINT_DONE)) && ep0.busy)
            in_done(&ep0);
        if (s & EPINT_SETUP)
            ep0_setup();
    }
    if (sts & (1u << 9)) {
        s = R(&ep_in, int_sts);
        R(&ep_in, int_clr) = s;
        if (s & EPINT_DONE)
            in_done(&ep_in);
    }
    if (sts & (1u << 10)) {
        s = R(&ep_out, int_sts);
        R(&ep_out, int_clr) = s;
        if (s & EPINT_DONE)
            out_done(&ep_out);
    }
    if (sts & (1u << 11))               /* EP3, EP4: unused */
        USB_REG(0x154) = USB_REG(0x150);
    if (sts & (1u << 12))
        USB_REG(0x194) = USB_REG(0x190);
    USB_REG(USB_INT_CLR) = 0x7f;
}

/* Line 25 is only known to carry the endpoint bits of INT_STS. Should it
 * stay asserted with none of them set, it would re-enter forever: a storm
 * masks it, and the session thread polls the controller instead. */
static void usb_irq(void)
{
    uint32_t sts = USB_REG(USB_INT_STS);

    usb_stat.irqs++;
    if (!(sts & 0x1f00)) {
        uint32_t w = kernel_ticks() / 10;

        USB_REG(USB_INT_CLR) = 0x7f;
        if (w != idle_window) {
            idle_window = w;
            idle_count = 0;
        }
        if (++idle_count >= IRQ_IDLE_LIMIT && !polled) {
            REG32(INTC_DISABLE) = 1u << IRQ_USB;
            polled = true;
            usb_stat.polled = 1;
        }
        return;
    }
    udc_service();
}

/* Wait on a semaphore, servicing the controller every millisecond once
 * the interrupt line had to be given up. */
static void wait_on(sem_t *s, uint32_t ms)
{
    if (!polled) {
        sem_wait(s, ms);
        return;
    }
    for (uint32_t t = 0; t < ms; t++) {
        uint32_t i = arch_irq_save();

        udc_service();
        arch_irq_restore(i);
        if (sem_wait(s, 1) == WAIT_OK)
            return;
    }
}

/* ---- for mass storage -------------------------------------------------- */

bool usb_stopping(void)
{
    return stop_req;
}

bool usb_gen_valid(uint32_t g)
{
    return g == gen && !stop_req;
}

bool usb_wait_configured(uint32_t *g)
{
    if (stop_req)
        return false;
    if (!configured) {
        wait_on(&cfg_sem, 100);
        if (!configured || stop_req)
            return false;
    }
    *g = gen;
    return true;
}

int usb_bulk_start_in(uint32_t g, const void *buf, int len)
{
    struct endpoint *e = &ep_in;
    uint32_t i = arch_irq_save();

    if (g != gen || stop_req || e->busy) {
        arch_irq_restore(i);
        return USB_EABORT;
    }
    e->buf = (uint8_t *)buf;
    e->len = len;
    e->done = 0;
    e->zlp = len == 0;
    e->result = 0;
    e->busy = true;
    if (!halted_in)                     /* else when the host clears it */
        in_packet(e);
    arch_irq_restore(i);
    return 0;
}

int usb_bulk_start_out(uint32_t g, void *buf, int len)
{
    struct endpoint *e = &ep_out;
    uint32_t i = arch_irq_save();

    if (g != gen || stop_req || e->busy || len <= 0) {
        arch_irq_restore(i);
        return USB_EABORT;
    }
    e->buf = buf;
    e->len = len;
    e->done = 0;
    e->result = 0;
    e->armed = false;
    e->busy = true;
    if (!halted_out)
        out_arm(e);
    arch_irq_restore(i);
    return 0;
}

static int bulk_wait(struct endpoint *e)
{
    uint32_t i;
    int r;

    while (e->busy && !stop_req)
        wait_on(&e->sem, 100);
    i = arch_irq_save();
    if (e->busy) {                      /* stopping */
        e->busy = false;
        e->armed = false;
        e->result = USB_EABORT;
    }
    r = e->result;
    arch_irq_restore(i);
    return r;
}

int usb_bulk_wait_in(void)
{
    return bulk_wait(&ep_in);
}

int usb_bulk_wait_out(void)
{
    return bulk_wait(&ep_out);
}

void usb_bulk_halt(bool in)
{
    uint32_t i = arch_irq_save();

    halt_set(in);
    arch_irq_restore(i);
}

bool usb_bulk_halted(bool in)
{
    return in ? halted_in : halted_out;
}

void usb_wait_unhalted(uint32_t g)
{
    while ((halted_in || halted_out) && usb_gen_valid(g))
        wait_on(&cfg_sem, 10);
}

/* ---- controller -------------------------------------------------------- */

/* The stock vcom_usb bring-up (0x38b62), step for step, with the "USB"
 * clock device's enable (0x67c6c) where it calls it, as the Rockbox port
 * does. The DMA block's AHB clock is enabled as there too (the bring-up
 * verified on the phone); no DMA channel is ever programmed. */
static void hw_start(void)
{
    REG32(AHB_EB_SET) = AHB_DMA_EB;
    usb_ldo(false);
    REG32(AHB_EB_SET) = AHB_USB_EB2;
    REG32(APB_MISC_CTL0) &= ~APB_USB_PD;
    REG32(AHB_EB_SET) = AHB_USB_EB;
    thread_sleep(300);                  /* stock: 300 ms */

    USB_REG(USB_CTRL) |= CTRL_ENABLE;
    USB_REG(USB_EP0_INT_CLR) = EPINT_ALL;
    ep0_program();
    USB_REG(USB_TIMEOUT_LMT) = 15;
    irq_register(IRQ_USB, usb_irq);
    irq_enable_line(IRQ_USB);
}

/* on the bus: the pull-up, then the LDO the host sees (stock order) */
static void hw_connect(void)
{
    USB_REG(USB_CTRL) |= CTRL_CONNECT;
    usb_ldo(true);
}

/* The AHB clock stays on: reaching a gated block stalls the bus for good,
 * and nothing must be able to touch it late. The LDO is what the bus and
 * the battery care about. */
static void hw_stop(void)
{
    uint32_t i;

    irq_disable_line(IRQ_USB);
    polled = false;
    USB_REG(USB_CTRL) = 0;
    i = arch_irq_save();
    bulk_cancel();
    cancel(&ep0);
    configured = false;
    arch_irq_restore(i);
    usb_ldo(false);
}

static void session(void *arg)
{
    hw_start();
    if (!stop_req) {
        hw_connect();
        usb_stat.state = USB_WAITING;
        usb_msc_run();
    }
    hw_stop();
    usb_msc_exit();
    usb_stat.state = USB_OFF;
    sem_post(&exited);
    thread_exit();
}

/* The charger's EIC level reads only with its debounce-mask bit set (stock
 * CHG_PHY_IsChargerPresent 0x663d4; the Rockbox port's power init sets
 * it, verified on the phone). Set once, the other channels kept. */
#define ANA_EIC_DMSK      0x82001904u
#define EIC_CHARGER       (1u << 2)

bool usb_cable(void)
{
    static bool mask_set;
    uint32_t m;

    if (!mask_set && adi_read_checked(ANA_EIC_DMSK, &m)) {
        if (!(m & EIC_CHARGER))
            adi_write(ANA_EIC_DMSK, m | EIC_CHARGER);
        mask_set = true;
    }
    return charger_present();
}

bool usb_running(void)
{
    return running;
}

int usb_start(void)
{
    uint32_t sessions = usb_stat.sessions;

    if (running)
        return 0;
    if (!sd_present() || !usb_msc_init())
        return -1;
    memset((void *)&usb_stat, 0, sizeof(usb_stat));
    usb_stat.sessions = sessions + 1;
    usb_stat.state = USB_STARTING;
    stop_req = false;
    polled = false;
    configured = false;
    set_config_seen = false;
    halted_in = halted_out = false;
    config_value = 0;
    idle_count = 0;
    ep0.busy = ep_in.busy = ep_out.busy = false;
    sem_init(&ep_in.sem, 0);
    sem_init(&ep_out.sem, 0);
    sem_init(&cfg_sem, 0);
    sem_init(&exited, 0);
    gen++;
    session_thread = thread_create("usb", PRIO_HIGH, 4096, session, NULL);
    if (!session_thread) {
        usb_msc_exit();
        usb_stat.state = USB_OFF;
        return -1;
    }
    running = true;
    return 0;
}

void usb_stop(void)
{
    uint32_t i;

    if (!running)
        return;
    stop_req = true;
    i = arch_irq_save();
    bulk_cancel_sw();                   /* the session thread stops the hardware */
    arch_irq_restore(i);
    sem_post(&cfg_sem);
    if (sem_wait(&exited, 10000) != WAIT_OK)
        return;                         /* still running: usb_running() */
    running = false;
    session_thread = NULL;
}
