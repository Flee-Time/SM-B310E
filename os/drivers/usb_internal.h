/* B310E OS — between the USB controller driver (usb.c) and mass storage
 * (usb_msc.c).
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

#endif
