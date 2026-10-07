/*
 * B310E-OS — drivers/usb_debug.h
 *
 * USB debug channel (Wave 6): routes ALL kernel logging (kprintf) to the
 * host PC's console via the phone's USB port. The host tool is fpdoom's
 * `libc_server` (fpdoom Releases: libc_server64.zip); it opens the device
 * (VID:PID 0x1782:0x4d00) and prints every debug message as "!!! <text>".
 *
 * Protocol + register init copied from fpdoom's usbio.c / libc.c
 * (Unlicense / public domain — verbatim reuse explicitly permitted).
 * On this board USB IS the debug channel: fpdoom has no SC6530 UART driver,
 * and the FDL bootloader (nor_fdl1.bin, loaded by spd_dump before our
 * image) already initialized the USB PHY — we only re-configure endpoints.
 *
 * SAFETY: this driver touches ONLY the USB block (0x20300000). It never
 * writes pin-mux registers — in particular NEVER 0x8c0002a4 (UART-TX
 * pinmux), which HANGS the B310E.
 *
 * Under HOST_TEST the hardware paths are compiled out and the pure framing
 * helpers (usb_debug_frame_build / usb_debug_frame_for_test) remain
 * callable from the host unit tests. The strong kputc override is also
 * suppressed under HOST_TEST (tests/hosttest.c defines its own capture
 * sink).
 */

#ifndef B310E_OS_USB_DEBUG_H
#define B310E_OS_USB_DEBUG_H

#include <stdint.h>

#include "kernel.h"              /* module_t, kputc declaration */

#ifdef __cplusplus
extern "C" {
#endif

/* Registered via the kernel module framework (name "usb_debug"). The
 * integration wave calls module_init_all(); until then arch/main.c calls
 * usb_debug_init() directly as a temporary boot hook. */
extern const module_t usb_debug_module;

/* Re-init the USB endpoints (0/2/3) for debug output and install the kputc
 * override. The FDL bootloader already brought the USB PHY up. Idempotent.
 * Returns 0 on success (no detectable failure mode on this chip). */
int usb_debug_init(void);

/* Arm the USB endpoints ONLY (no transfers, no waits) — MUST run as the
 * FIRST thing after the boot stub, BEFORE any slow pre-init: spd_dump's
 * post-EXEC FDL1 handshake sends BSL_CMD_CHECK_BAUD on EP2 OUT within
 * microseconds of EXEC_DATA, with a 1s libusb timeout. If EP2 is not
 * buffer-ready by then, the OUT transfer NAKs and libusb reports
 * "usb_send failed : LIBUSB_ERROR_TIMEOUT" at the DRIVER level. The
 * proven FDL1 (custom_fdl) arms USB right after its chip init. */
void usb_debug_arm_endpoints(void);

/* Bounded EP2 poll for libc_server's HOST_CONNECT byte. Split out of
 * usb_debug_init so the caller can run it AFTER the chip init has ramped
 * the clock to 208 MHz (the 10M-iteration budget is ~0.5s at 208 MHz but
 * several seconds at the ROM's slow clock). See usb_debug_poll for the
 * non-blocking variant. */
void usb_debug_wait_connect(void);

/* FDL1-role marker (menu.bin loaded at the 0x40004000 FDL slot): the next
 * usb_debug_init() sends the BSL_REP_VER frame before the ack so spd_dump's
 * post-EXEC CHECK_BAUD handshake completes (no "usb_send failed" noise).
 * os.bin (loaded as FDL2) must NOT call this — spd_dump expects only the
 * ack there, and a stray VER frame breaks the FDL2 load. */
void usb_debug_set_fdl1(void);

/* Character backend for kprintf. Line-buffered: accumulates into a small
 * static buffer and flushes ONE complete libc_server debug-message frame
 * when a '\n' arrives (or the buffer fills). Characters before init are
 * dropped. Never blocks for more than a bounded TX poll; frames are dropped
 * while the host is not reading. */
void usb_debug_putc(char c);

/* 1 once usb_debug_init() has drained libc_server's HOST_CONNECT byte on
 * EP2 (bounded wait — never blocks boot), 0 otherwise. TX frames are sent
 * regardless and stay pending on EP3 until a host reads them, so a later
 * connect still completes the handshake. */
int usb_debug_host_connected(void);

/* Non-blocking EP2 poll: drains libc_server's HOST_CONNECT byte (0x00)
 * when it arrives, always re-arming EP2 (even after the first connect —
 * an early return would leave EP2 buffer-ready clear and libc_server's
 * retry would NAK). On the FIRST HOST_CONNECT it immediately sends the
 * "connected" CMD_MESSAGE frame (usb_debug_connect_greeting), mirroring
 * fpdoom's _debug_msg right after the connect byte. Call this from every
 * scheduler round (e.g. the demo tasks' loops) so a libc_server started
 * ANY time after boot connects within one round — usb_debug_init's bounded
 * wait alone only catches a host that is already reading at boot, and
 * libc_server gives up after ~1 s of retries. Returns immediately. */
void usb_debug_poll(void);

/* Replug diagnostics: copies the last USB_DBG_REPLUG_RING (4) post-reset
 * EP0 SETUP requests + EP0 statuses stashed by usb_debug_poll — the whole
 * enumeration sequence (GET_DESCRIPTOR → SET_ADDRESS → …), oldest first in
 * the arrays (NULL pointers are skipped; each array must hold ≥4 entries).
 * Returns the number of replug SETUPs seen since boot (0 = none). A task
 * logs this once the link is healthy — kprintf in the poll path itself
 * would stall (the host is not reading EP3 IN during EP0 re-enumeration). */
uint32_t usb_debug_replug_diag(uint32_t *setup_low, uint32_t *setup_high,
                               uint32_t *sts_endp0);

/* Pure framing (no hardware, host-testable): builds the "connected"
 * CMD_MESSAGE frame the device sends the instant it drains libc_server's
 * HOST_CONNECT (fpdoom entry.c's _debug_msg right after the connect byte —
 * an immediate reply, not a wait for the next kprintf line). Same wire
 * format as usb_debug_frame_build. Returns the frame size (13), or -1 if
 * out_len is too small. */
int usb_debug_connect_greeting(uint8_t *out, int out_len);

/* Pure framing (no hardware, host-testable). Builds the full wire frame for
 * the single character c (1-byte payload) into out[]:
 *   [0] = 0x80 (CMD_MESSAGE)   [1] = payload length
 *   [2..3] = checksum LE       [4] = c
 * Returns the total frame size (5), or -1 if out_len < 5. */
int usb_debug_frame_for_test(char c, uint8_t *out, int out_len);

/* Pure framing for an arbitrary payload (0..255 bytes). Same wire format as
 * above with the payload at out[4..]. Returns total frame size (len + 4),
 * or -1 if out_len is too small. Exposed for host tests; the device uses it
 * to frame whole log lines. */
int usb_debug_frame_build(const char *payload, int len, uint8_t *out,
                          int out_len);

#ifdef __cplusplus
}
#endif

#endif /* B310E_OS_USB_DEBUG_H */
