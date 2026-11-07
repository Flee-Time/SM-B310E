/* B310E OS — between the USB controller driver (usb.c) and the functions
 * on it: mass storage (usb_msc.c) and the RNDIS network adapter
 * (usb_rndis.c), one at a time.
 *
 * Bulk transfers belong to a "generation": a bus reset, a mass storage
 * reset, a (re)configuration or usb_stop() starts a new one and cancels
 * every transfer of the old. A transfer started with a stale generation
 * fails at once, so a command interrupted by a reset is simply dropped.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef OS_USB_INTERNAL_H
#define OS_USB_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

#define USB_PKT       64        /* bulk and EP0 packet size */
#define USB_EABORT    (-1)      /* cancelled: reset, reconfigured, stopping */

/* What the device is to the host. The callbacks marked irq run in the
 * interrupt handler. */
typedef struct {
    const uint8_t *dev_desc;            /* 18 bytes */
    const uint8_t *cfg_desc;            /* the whole configuration */
    uint16_t cfg_len;
    const char *const *strings;         /* [0] unused (the languages) */
    uint8_t n_strings;
    uint8_t ms_vendor_code;             /* MS OS 1.0: string 0xee answered */
    bool use_ep3;                       /* EP3 IN, the interrupt endpoint */
    /* irq: a class or vendor request; answer it with usb_ep0_send(), take
     * its data with usb_ep0_recv(), or return false to stall it */
    bool (*request)(uint8_t bm, uint8_t rq, uint16_t value, uint16_t index,
                    uint16_t len);
    void (*ep0_out_done)(int len);      /* irq: the usb_ep0_recv() data is in */
    void (*intr_done)(void);            /* irq: the host took usb_intr_send() */
    void (*reset)(void);                /* irq: bus reset or (re)configured */
    /* irq: a bulk OUT transfer's whole length once its first bytes are in
     * (0: not known), so one that ends on a packet boundary completes
     * without a zero-length packet */
    int (*out_len)(const uint8_t *buf, int got);
    bool (*init)(void);                 /* before the session; false: fails */
    void (*run)(void);                  /* the session thread, until usb_stopping() */
    void (*exit)(void);                 /* after it, off the bus */
} usb_function_t;

extern const usb_function_t usb_fn_storage, usb_fn_network;

/* EP0 from a function's request(): the data stage of a control read
 * (copied out of data as the host takes it: data must stay valid), or
 * the receive of a control write's data stage (ep0_out_done() when in). */
void usb_ep0_send(const void *data, int len);
void usb_ep0_recv(void *buf, int len);

/* Wait until the device is configured; true with the current generation,
 * false after about 100 ms without (call again) or when stopping. */
bool usb_wait_configured(uint32_t *gen);
bool usb_stopping(void);
bool usb_gen_valid(uint32_t gen);

/* Bulk IN (EP1) and OUT (EP2), PIO, one interrupt per packet. Buffers must
 * be word aligned. The receive ends at len bytes or at a short packet.
 * start: 0 or USB_EABORT; wait: bytes moved, or USB_EABORT. */
int usb_bulk_start_in(uint32_t gen, const void *buf, int len);
int usb_bulk_start_out(uint32_t gen, void *buf, int len);
int usb_bulk_wait_in(void);
int usb_bulk_wait_out(void);
/* the IN wait, giving up (and taking the send back) after ms */
int usb_bulk_wait_in_ms(uint32_t ms);

/* EP3 interrupt IN, at most 64 bytes, irq-safe: 0 or USB_EABORT (busy,
 * not configured). buf must stay valid until intr_done(). */
int usb_intr_send(const void *buf, int len);
bool usb_intr_busy(void);

/* Halt an endpoint (the host clears it with CLEAR_FEATURE). A send started
 * on a halted IN endpoint goes out once the halt is cleared. */
void usb_bulk_halt(bool in);
bool usb_bulk_halted(bool in);
/* Sleep until neither endpoint is halted or the generation changes. */
void usb_wait_unhalted(uint32_t gen);

/* usb_msc.c: buffers (init false without memory), then serve the card
 * until usb_stopping() */
bool usb_msc_init(void);
void usb_msc_exit(void);
void usb_msc_run(void);
void usb_msc_reset(void);           /* irq: mass storage reset, forget sense */

/* usb_rndis.c: the network adapter's session (net/ runs on top of it) */
bool usb_rndis_init(void);
void usb_rndis_run(void);
void usb_rndis_exit(void);

#endif
