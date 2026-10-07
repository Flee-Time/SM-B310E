/*
 * B310E-OS — drivers/usb_debug.c
 *
 * USB debug channel: all kprintf output is framed per fpdoom's libc_server
 * protocol and transmitted over USB endpoint 3 (bulk IN, address 0x83).
 *
 * Framing (libc.c _debug_msg ~113-129 == libc_server.c read_msg — the two
 * ends of the same wire protocol, Unlicense/public domain):
 *
 *   byte 0        = 0x80 (CMD_MESSAGE)
 *   byte 1        = payload length (0..255)
 *   bytes 2..3    = checksum, little-endian:
 *                   fastchk16(0x5a5a + 0x80 + (len << 8), payload, len)
 *   bytes 4..     = payload (the log line)
 *
 * fastchk16 is fpdoom's 16-bit end-around-carry sum (libc_server.c:383-394
 * — the exact C code the HOST uses to verify, so a matching implementation
 * here is guaranteed to interoperate).
 *
 * NOTE on the "0x7e 0x00 0x00 ..." bytes sometimes seen in spreadtrum
 * protocol notes: those are the FDL handshake ack (entry.c:168-169
 * { 0x7e,0,0x80,0,0,0xff,0x7f,0x7e }), NOT the debug-message framing. The
 * message framing is the CMD_MESSAGE header above — this file implements
 * exactly what the stock libc_server parser expects.
 *
 * Endpoint init is fpdoom's usb_init (usbio.c:308-325, Unlicense) with the
 * interrupt-enable bits kept verbatim — harmless because arch/start.s masks
 * CPU IRQ/FIQ at boot (msr cpsr_c, #0xd3), so no IRQ vectors are taken.
 * TX is POLLED: no interrupt handlers, no descriptor/enumeration handling
 * (the FDL bootloader left the device configured; spd_dump stays the active
 * host until it exits). This wave is TX-only by design — the RX/file-server
 * path is a later wave's job.
 *
 * TX flow: each frame is written to the EP3 FIFO and the transfer is
 * started; TRANSFER_END is polled with a bounded budget. Bulk IN data can
 * only leave when the host issues IN tokens, so with no host attached the
 * transfer stays pending and the frame is delivered later (which is also
 * what completes libc_server's HOST_CONNECT handshake: its first read sees
 * our 0x80). While a transfer is pending, further frames are dropped
 * (never corrupting the pending one) — log volume at boot is tiny.
 *
 * LINK LOSS (unplug) + RECONNECT: a TX wait timeout marks s_link_down;
 * every later send returns immediately (no USB_TX_BUDGET spin) so an
 * unplugged cable never stalls the cooperative scheduler — the "phone
 * lags when USB is removed" bug. usb_debug_poll() watches EP0 for
 * SETUP_TRANS_END: when the host replugs it re-enumerates the device via
 * GET_DESCRIPTOR, and we reply with usb_dev_desc/usb_config_desc (VID:PID
 * 0x1782:0x4d00, fpdoom usbio.c verbatim) then re-init EP0/EP2/EP3 and
 * clear the wedged TX state, so libc_server can rebind after a replug.
 *
 * SAFETY: only the USB block (0x20300000) is touched. NO pin-mux writes —
 * NEVER write 0x8c0002a4 (UART-TX pinmux) — it HANGS the B310E.
 */

#include "usb_debug.h"

#include <stddef.h>

/* chip.h's MEM4 is not on this TU's include path (arch/); identical macro
 * body, so a future #include of chip.h cannot conflict. */
#ifndef MEM4
#define MEM4(a) (*(volatile uint32_t *)(a))
#endif

/* ---- SC6530 USB register map (fpdoom usbio.c, Unlicense) ---------------- */

#define USB_BASE        0x20300000u

#define USB_CR(o)       MEM4(USB_BASE + (o))

/* max packet size: bits 12..22 of the endpoint control register */
#define USB_MAXPSIZE(o, n) \
    (USB_CR(o) = (USB_CR(o) & ~0x7ff000u) | ((uint32_t)(n) << 12))
/* transfer size: bits 0..16 of the endpoint control register */
#define USB_TRSIZE(o, n) \
    (USB_CR(o) = (USB_CR(o) & ~0x1ffffu) | (uint32_t)(n))

enum {
    USB_CTRL          = 0x00,
    INT_STS           = 0x18,
    TIMEOUT_LMT       = 0x28,

    TR_SIZE_IN_ENDP0  = 0x40,
    REQ_SETUP_LOW     = 0x5c,
    REQ_SETUP_HIGH    = 0x60,
    ENDP0_CTRL        = 0x64,
    INT_CTRL_ENDP0    = 0x68,
    INT_STS_ENDP0     = 0x6c,
    INT_CLR_ENDP0     = 0x70,

    ENDP2_CTRL        = 0x100,
    RCV_DATA_ENDP2    = 0x104,
    INT_CTRL_ENDP2    = 0x10c,
    INT_STS_ENDP2     = 0x110,
    INT_CLR_ENDP2     = 0x114,

    ENDP3_CTRL        = 0x140,
    TRANS_SIZE_ENDP3  = 0x148,
    INT_CTRL_ENDP3    = 0x14c,
    INT_STS_ENDP3     = 0x150,
    INT_CLR_ENDP3     = 0x154,
};

/* EP3 IN FIFO = 0x80000 (EP0) + 2 words (usbio.c:85-87) */
#define USB_FIFO_ENDP3  (USB_BASE + 0x80008u)

/* EP0 IN FIFO (control transfers / descriptor replies) */
#define USB_FIFO_ENDP0  (USB_BASE + 0x80000u)

/* EP2 OUT FIFO: FIFO_entry_endp_out = USB_BASE + 0x8000c for CHIP 3
 * (SC6530, usbio.c:89-94); FIFO_entry_endp2 = that + 1 word = 0x80010. */
#define USB_FIFO_ENDP2  (USB_BASE + 0x80010u)

#define USB_MAXREAD     64u         /* max packet size of our bulk EP */
#define USB_TX_BUDGET   2000000u    /* bounded TRANSFER_END poll (~tens of
                                       ms @ 208 MHz); keeps boot non-blocking
                                       when no host is attached */
#define USB_CONNECT_BUDGET 10000000u /* bounded EP2 HOST_CONNECT poll (~0.5 s
                                       @ 208 MHz). fpdoom BLOCKS here forever
                                       (entry.c:173-176); we must not — the
                                       LCD banner boots without libc_server */

/* ---- libc_server debug-message protocol (fpdoom cmd_def.h) -------------- */

#define USB_DBG_CMD_MESSAGE  0x80u
#define USB_DBG_HOST_CONNECT 0x00u
#define USB_DBG_CHK_INIT     0x5a5au
#define USB_DBG_MAX_PAYLOAD  255u

/* fpdoom's 16-bit end-around-carry sum; the host's parser (libc_server.c)
 * runs this exact algorithm, so our sender matches it by construction. */
static unsigned usb_debug_fastchk16(unsigned crc, const uint8_t *src, int len)
{
    while (len > 1) {
        crc += (unsigned)src[1] << 8 | src[0];
        src += 2;
        len -= 2;
    }
    if (len)
        crc += *src;
    crc = (crc >> 16) + (crc & 0xffffu);
    crc += crc >> 16;
    return crc & 0xffffu;
}

/* ---- pure framing (compiles identically under HOST_TEST) ---------------- */

int usb_debug_frame_build(const char *payload, int len, uint8_t *out,
                          int out_len)
{
    unsigned tmp;
    int i;

    if (len > (int)USB_DBG_MAX_PAYLOAD)
        len = USB_DBG_MAX_PAYLOAD;
    if (len < 0)
        len = 0;
    if (out_len < len + 4)
        return -1;

    tmp = USB_DBG_CMD_MESSAGE | (unsigned)len << 8;
    out[0] = (uint8_t)tmp;
    out[1] = (uint8_t)(tmp >> 8);
    tmp += USB_DBG_CHK_INIT;
    tmp = usb_debug_fastchk16(tmp, (const uint8_t *)payload, len);
    out[2] = (uint8_t)tmp;
    out[3] = (uint8_t)(tmp >> 8);
    for (i = 0; i < len; i++)
        out[4 + i] = (uint8_t)payload[i];
    return len + 4;
}

int usb_debug_frame_for_test(char c, uint8_t *out, int out_len)
{
    return usb_debug_frame_build(&c, 1, out, out_len);
}

/* ---- polled TX (hardware) ------------------------------------------------ */

/* Copy len bytes to the EP3 FIFO, 32-bit words, byte-swapped to the
 * controller's big-endian wire order — mirrors fpdoom's usb_send_asm
 * (bswap32 per word) / the C fallback `*(volatile uint32_t*)fifo =
 * READ32_BE(s)` (usbio.c:162-163). Resulting wire order == source memory
 * byte order, which is what libc_server parses. src must have up to 3 bytes
 * of slack for the final partial word. */
/* Write a byte buffer into an IN FIFO, byte-swapping each 32-bit word to
 * the USB wire order (fpdoom usb_send_asm / swap_be32, usbio.c:143-160).
 * THE FIFO IS A PARAMETER — the EP0 descriptor reply must go to
 * USB_FIFO_ENDP0 (0x80000), NOT the EP3 FIFO (0x80008). Hardcoding EP3
 * here silently sent every descriptor reply to the wrong endpoint: EP0's
 * TX-start then transmitted an empty FIFO and the host retried
 * GET_DESCRIPTOR forever (the "device descriptor request failed" replug
 * failure, root-caused 2026-08-22). */
static void usb_debug_fifo_write(volatile uint32_t *fifo,
                                 const uint8_t *src, uint32_t len)
{
    uint32_t i;

    for (i = 0; i < len; i += 4, src += 4)
        *fifo = (uint32_t)src[0] << 24 | (uint32_t)src[1] << 16 |
                (uint32_t)src[2] << 8 | (uint32_t)src[3];
}

/* Copy len bytes out of the EP2 OUT FIFO, byte-swapping each 32-bit word
 * back to LE memory order — mirrors fpdoom usb_recv(3,...) (usbio.c:176-206,
 * swap_be32 per word). Re-arms the endpoint buffer so later OUT packets keep
 * being ACKed. For the 1-byte HOST_CONNECT one FIFO word is read and the low
 * byte is the wire byte. */
static void usb_debug_recv(uint32_t *dst, uint32_t len)
{
    volatile uint32_t *fifo = (volatile uint32_t *)USB_FIFO_ENDP2;

    while (len) {
        uint32_t w = *fifo;
        *dst++ = (w >> 24) | ((w >> 8) & 0xff00u) |
                 ((w << 8) & 0xff0000u) | (w << 24);
        len = len > 4 ? len - 4 : 0;
    }
    USB_CR(ENDP2_CTRL) |= 1u << 28;     /* buffer ready (re-arm RX) */
}

/* 1 while an EP3 transfer has been STARTED but not yet completed (the host
 * has not drained it). A software pending-flag, NOT a re-read of the
 * TRANSFER_END bit: that bit is cleared after every completed transfer and
 * only re-sets when a NEW transfer completes — so a "wait for it to be
 * set" gate after a completed transfer would wait forever for a completion
 * that can never start (the gate is the thing that starts transfers). The
 * flag is the only correct "is a transfer actually in flight?" test. */
static int s_tx_pending;

/* 1 while the host is believed to be GONE (cable unplugged): a TX wait
 * timed out. While down, usb_debug_send_chunk() drops immediately instead
 * of spinning USB_TX_BUDGET — that spin is the "phone lags when I unplug
 * the USB" bug. Cleared when EP0 sees the host re-enumerate (a SETUP
 * packet), which triggers usb_debug_reinit_all(). */
static int s_link_down;

/* 1 while the USB session is ESTABLISHED (the host is configured and
 * reading EP3 IN). Set at boot (the ROM/FDL enumeration is complete by
 * the time we run) and re-set at the re-enumeration TAIL (SET_CONFIGURATION
 * 0x09) or when EP2 sees HOST_CONNECT (proof the host is actively reading).
 * Cleared when a post-replug SETUP arrives. While clear, all TX is dropped
 * IMMEDIATELY (no USB_TX_BUDGET spin): during EP0 re-enumeration the host
 * is not reading EP3 IN yet, so a TX would sit pending with TX-start armed,
 * time out, and leave EP3 half-armed — the "response timeout" / "unexpected
 * response" failures when libc_server reconnects after a replug. The LCD
 * console ring is fed by kputc BEFORE this gate, so replug diagnostics
 * still appear on the LCD while the USB channel is gated. */
static int s_session_ready;

/* Replug diagnostics (learnings 2026-08-21): the LAST few EP0 SETUP
 * requests + endpoint status, stashed by usb_debug_poll when
 * SETUP_TRANS_END fires, so a task can log them WITHOUT kprintf in the
 * poll path (during EP0 re-enumeration the host is not reading EP3 IN
 * yet — a kprintf would spin USB_TX_BUDGET for IN tokens the host is not
 * sending). Read via usb_debug_replug_diag().
 *
 * HISTORY RING (FIX 4 upgrade): one test must show the WHOLE enumeration
 * sequence, not just the last request — the host sends GET_DESCRIPTOR →
 * SET_ADDRESS → GET_DESCRIPTOR → SET_CONFIGURATION during a replug, and
 * WHERE the sequence stalls (e.g. SET_ADDRESS never ACKed, or the second
 * GET_DESCRIPTOR never arrives) is exactly the "device descriptor request
 * failed" diagnostic we need. Each SETUP_TRANS_END pushes {REQ_SETUP_LOW,
 * REQ_SETUP_HIGH, INT_STS_ENDP0} into a 4-deep ring; the banner task logs
 * the whole ring once the link is healthy again. */
#define USB_DBG_REPLUG_RING 4u

static uint32_t s_dbg_setup_low[USB_DBG_REPLUG_RING];
static uint32_t s_dbg_setup_high[USB_DBG_REPLUG_RING];
static uint32_t s_dbg_sts_endp0[USB_DBG_REPLUG_RING];
static uint32_t s_dbg_replug_count;
static uint32_t s_dbg_ring_wr;           /* next ring slot (mod RING) */

/* Device + config descriptors for EP0 GET_DESCRIPTOR replies. Verbatim
 * from fpdoom usbio.c (Unlicense) — VID:PID 0x1782:0x4d00, one bulk IN
 * (0x83) + one bulk OUT (0x02) interface, exactly what libc_server binds.
 * Without these the host cannot re-enumerate the device after a replug. */
static const uint8_t usb_dev_desc[] = {
    0x12, 0x01, 0x10, 0x01, 0x00, 0x00, 0x00, 0x40,
    0x82, 0x17, 0x00, 0x4d, 0x02, 0x02, 0x00, 0x00,
    0x00, 0x01
};

static const uint8_t usb_config_desc[] = {
    0x09, 0x02, 0x20, 0x00, 0x01, 0x01, 0x00, 0xc0, 0x32,
    0x09, 0x04, 0x00, 0x00, 0x02, 0xff, 0x00, 0x00, 0x00,
    0x07, 0x05, 0x83, 0x02, 0x40, 0x00, 0x00,
    0x07, 0x05, 0x02, 0x02, 0x40, 0x00, 0x00
};

#define USB_REC_MASK      0x1f
#define USB_REQ_MASK      (3u << 5)
#define USB_REC_DEVICE    0
#define USB_REQ_STANDARD  (0u << 5)
#define USB_REQUEST_GET_DESCRIPTOR 6
#define USB_DEVICE_DESCRIPTOR_TYPE  1
#define USB_CONFIGURATION_DESCRIPTOR_TYPE  2

/* One <=64-byte chunk: wait for a pending previous transfer to drain
 * (bounded), set the transfer size, copy into the FIFO, start TX, wait
 * TRANSFER_END (bounded — needs host IN tokens). Returns 0 ok, -1 if the
 * host is not reading; on -1 the current frame is dropped, but any frame
 * that was already started stays pending in the FIFO and delivers whenever
 * the host finally reads (which also completes libc_server's handshake).
 *
 * Link-down fast path: once a TX wait times out the host is treated as
 * GONE (s_link_down). Every later call returns -1 IMMEDIATELY — no
 * USB_TX_BUDGET spin — so unplugging the cable never stalls the
 * cooperative scheduler (the "phone lags when USB is removed" bug).
 * Reconnection is detected on EP0 (usb_debug_poll), which clears
 * s_link_down. */
static int usb_debug_send_chunk(const uint8_t *src, uint32_t len)
{
    uint32_t n;

    if (s_link_down || !s_session_ready)
        return -1;              /* host gone, or mid re-enumeration: drop */

    if (s_tx_pending) {
        n = USB_TX_BUDGET;
        while (!(USB_CR(INT_STS_ENDP3) & (1u << 9)) && --n != 0)
            ;
        if (n == 0) {
            s_link_down = 1;    /* previous transfer never completed */
            return -1;
        }
        USB_CR(INT_CLR_ENDP3) |= 1u << 9;
        USB_CR(ENDP3_CTRL) &= ~(1u << 27);  /* idle/NAK state (see below) */
        s_tx_pending = 0;
    }

    USB_MAXPSIZE(ENDP3_CTRL, len);
    USB_TRSIZE(TRANS_SIZE_ENDP3, len);
    usb_debug_fifo_write((volatile uint32_t *)USB_FIFO_ENDP3, src, len);
    USB_CR(ENDP3_CTRL) |= 1u << 27; /* TX start */
    s_tx_pending = 1;

    n = USB_TX_BUDGET;
    while (!(USB_CR(INT_STS_ENDP3) & (1u << 9)) && --n != 0)
        ;
    if (n == 0) {
        s_link_down = 1;        /* no host: leave pending, deliver later */
        return -1;
    }
    USB_CR(INT_CLR_ENDP3) |= 1u << 9;
    /* Clear the TX-start bit so EP3 returns to the idle/NAK state. If it
     * stays set, the host's NEXT IN read (libc_server's idle poll when the
     * phone has nothing to log) finds the endpoint "in transfer" with an
     * empty FIFO and the controller STALLs it -> LIBUSB_ERROR_PIPE -> the
     * session dies and Windows re-enumerates on the next launch. */
    USB_CR(ENDP3_CTRL) &= ~(1u << 27);
    s_tx_pending = 0;
    /* A completed transfer proves the host is reading: the link is UP.
     * This auto-recovers s_link_down after a replug, when a TX started
     * during the re-enumeration (host not IN-ing EP3 yet) timed out and
     * poisoned the flag - the greeting would otherwise be dropped and
     * libc_server would see only one stale frame then hang. */
    s_link_down = 0;
    return 0;
}

/* Send [len] bytes as 64-byte chunks (fpdoom usbio.c usb_write chunking,
 * incl. the full-packet 32/32 split). One call = one atomic frame. */
static int usb_debug_send_raw(const uint8_t *s, uint32_t len)
{
    for (; len > USB_MAXREAD; len -= USB_MAXREAD, s += USB_MAXREAD)
        if (usb_debug_send_chunk(s, USB_MAXREAD) != 0)
            return -1;
    if (len) {
        if (len == USB_MAXREAD) {
            len >>= 1;
            if (usb_debug_send_chunk(s, len) != 0)
                return -1;
            s += len;
        }
        if (usb_debug_send_chunk(s, len) != 0)
            return -1;
    }
    return 0;
}

/* frame buffer: 4 header + 255 payload + 3 slack for the FIFO word over-read.
 * Shared by usb_debug_flush() and usb_debug_send_connected() — both run in
 * the same cooperative context and never nest. */
static uint8_t s_frame[4 + USB_DBG_MAX_PAYLOAD + 3];

/* Pure framing of the "connected" greeting (no hardware): the CMD_MESSAGE
 * frame the device sends the INSTANT it drains libc_server's HOST_CONNECT —
 * mirrors fpdoom's _debug_msg("entry1") right after the connect byte
 * (entry.c:185-188), so the reply never depends on a later kprintf line. */
int usb_debug_connect_greeting(uint8_t *out, int out_len)
{
    static const char s_connected[] = "connected";

    return usb_debug_frame_build(s_connected,
                                 (int)sizeof(s_connected) - 1,
                                 out, out_len);
}

static void usb_debug_send_connected(void)
{
    /* s_frame is idle here: flush (its only other user) runs in the same
     * cooperative context and is never nested inside the EP2 poll. */
    int n = usb_debug_connect_greeting(s_frame, (int)sizeof(s_frame));

    if (n >= 0)
        usb_debug_send_raw(s_frame, (uint32_t)n);
}

/* Drain whatever EP2 OUT received (HOST_CONNECT, 1 byte) and re-arm the
 * endpoint buffer. Mirrors fpdoom usb_int_endp2 (usbio.c:254-279): read the
 * received length from ENDP2_CTRL & 0x3ff (cap 64), usb_debug_recv (re-arms
 * via ENDP2_CTRL bit 28), THEN clear INT_CLR_ENDP2 = 0x3fff — the controller
 * must see the buffer re-armed before the status flag is cleared. Returns
 * the first received byte. */
static uint8_t usb_debug_ep2_drain(void)
{
    uint32_t len = USB_CR(ENDP2_CTRL) & 0x3ffu;
    uint8_t buf[USB_MAXREAD];

    if (len == 0)
        len = 1;                    /* always re-arm, even on a spurious flag */
    if (len > USB_MAXREAD)
        len = USB_MAXREAD;
    usb_debug_recv((uint32_t *)buf, len);
    USB_CR(INT_CLR_ENDP2) = 0x3fff;
    return buf[0];
}

/* Bounded EP2 poll for libc_server's HOST_CONNECT byte (0x00). fpdoom
 * blocks forever on this (entry.c:173-176 — their binaries stay black
 * without libc_server); we must not, so the poll has a hard budget and the
 * banner boots regardless. On a hit the byte is drained (re-arming EP2) and
 * the "connected" CMD_MESSAGE frame is sent immediately, satisfying
 * libc_server's usb_recv(io,1)==0x80 + read_msg (libc_server.c:550-567).
 * On budget expiry we just return: any frame already pending on EP3
 * delivers the same 0x80 whenever the host first reads, and libc_server
 * retries HOST_CONNECT 10x over ~1 s. */
static int s_host_connected;            /* set once EP2 saw HOST_CONNECT */
void usb_debug_wait_connect(void)
{
    uint32_t n = USB_CONNECT_BUDGET;

    while (--n != 0) {
        if (USB_CR(INT_STS_ENDP2) & 1) {        /* TRANSACTION_END */
            uint8_t b = usb_debug_ep2_drain();

            if (b == USB_DBG_HOST_CONNECT && !s_host_connected) {
                s_host_connected = 1;
                usb_debug_send_connected();
            }
            if (s_host_connected)
                return;
        }
    }
}

int usb_debug_host_connected(void)
{
    return s_host_connected;
}

/* ---- EP0 control (descriptor replies) + reconnect ----------------------- */

/* defined below (endpoint re-init); forward-declared for the reconnect path */
static void usb_debug_init_endp0(void);
static void usb_debug_init_endp2(void);
static void usb_debug_init_endp3(void);
static void usb_debug_replug_reinit(void);

/* Reply to a GET_DESCRIPTOR (device/config) SETUP request on EP0, exactly
 * like fpdoom usb_int_endp0 -> usb_send_desc (usbio.c:228-257). Required
 * for the host to re-enumerate the device after a cable replug — the
 * initial enumeration was done by the FDL bootloader, and libc_server
 * rebinds via the same VID:PID the descriptors carry. */
static void usb_debug_endp0_setup(void)
{
    uint32_t a, len, req, b;

    if (!(USB_CR(INT_STS_ENDP0) & (1u << 8)))   /* SETUP_TRANS_END */
        return;

    a = USB_CR(REQ_SETUP_LOW);
    len = USB_CR(REQ_SETUP_HIGH) >> 16;         /* wLength */
    req = (a >> 8) & 0xff;

    b = a & (USB_REC_MASK | USB_REQ_MASK);
    if (b == (USB_REC_DEVICE | USB_REQ_STANDARD) &&
        req == USB_REQUEST_GET_DESCRIPTOR) {
        const uint8_t *p;
        int n, type = (int)(a >> 24);

        if (type == USB_DEVICE_DESCRIPTOR_TYPE) {
            p = usb_dev_desc; n = (int)sizeof(usb_dev_desc);
        } else if (type == USB_CONFIGURATION_DESCRIPTOR_TYPE) {
            p = usb_config_desc; n = (int)sizeof(usb_config_desc);
        } else {
            p = NULL; n = 0;
        }
        if (p && (int)len < n)
            n = (int)len;
        if (p && n > 0) {
            USB_MAXPSIZE(ENDP0_CTRL, (uint32_t)n);
            USB_TRSIZE(TR_SIZE_IN_ENDP0, (uint32_t)n);
            usb_debug_fifo_write((volatile uint32_t *)USB_FIFO_ENDP0,
                                 p, (uint32_t)n);
            USB_CR(ENDP0_CTRL) |= 1u << 27;     /* TX start (EP0 IN) */
        }
    }
    USB_CR(INT_CLR_ENDP0) = 0x3fff;
}

/* Re-arm EP0 buffer-ready + SETUP int-enable WITHOUT clearing SETUP_TRANS_END.
 * The full usb_debug_init_endp0() clears INT_CLR_ENDP0 bit 8 — that write is
 * only valid at init time, when no SETUP can be pending. In the poll path a
 * pending SETUP means "the host is waiting for a descriptor right now"; clearing
 * its flag discards the request (see usb_debug_poll). A bus reset wipes EP0
 * (address 0, buffer-ready clear), so this is re-run every round the host is
 * not mid-SETUP. */
/* Re-arm EP0 buffer-ready + SETUP int-enable WITHOUT clearing SETUP_TRANS_END.
 * The full usb_debug_init_endp0() clears INT_CLR_ENDP0 bit 8 — that write is
 * only valid at init time, when no SETUP can be pending. In the poll path a
 * pending SETUP means "the host is waiting for a descriptor right now"; clearing
 * its flag discards the request (see usb_debug_poll). A bus reset wipes EP0
 * (address 0, buffer-ready clear), so this is re-run every round the host is
 * not mid-SETUP. MAXPSIZE is re-asserted here too: a bus reset may wipe it,
 * and without MAXPSIZE the post-reset SETUP is rejected at the hardware level.
 * NOTE: the re-arm is DEFERRED for a few ticks after a descriptor reply — the
 * reply sets MAXPSIZE = descriptor length for the transfer, and re-arming
 * MAXPSIZE=8 mid-transfer clobbers it (the host then retries GET_DESCRIPTOR
 * forever). See s_ep0_arm_until. */
static uint32_t s_ep0_arm_until;    /* sched_ticks() deadline; skip until then */

static void usb_debug_arm_endp0(void)
{
    USB_MAXPSIZE(ENDP0_CTRL, 8);        /* a bus reset may wipe EP0's size */
    USB_CR(INT_CTRL_ENDP0) |= 1u << 8;      /* SETUP int-enable */
    USB_CR(ENDP0_CTRL) |= 1u << 28;         /* buffer ready */
}

/* One non-blocking EP2 check (same logic as one usb_debug_wait_connect
 * loop iteration). Call every scheduler round from the demo tasks so EP2
 * stays armed for the whole boot: libc_server's HOST_CONNECT can arrive
 * ANY time after spd_dump exits (its usb_send retries 10x over ~1 s), not
 * just during usb_debug_init's bounded wait. ALWAYS drains + re-arms on
 * TRANSACTION_END (never bails once s_host_connected is set — an early
 * return would leave EP2 buffer-ready clear and the host's retry would
 * NAK: "usb_send failed : LIBUSB_ERROR_TIMEOUT"). On the first HOST_CONNECT
 * the "connected" CMD_MESSAGE frame is sent immediately. Returns
 * immediately — safe to call from cooperative tasks every iteration. */
void usb_debug_poll(void)
{
#ifdef SD_BOOT_NO_USB
    /* Card boot (`make os-sd`): the USB block is UNPOWERED — ANY register
     * access (read or write) on an unpowered SC6530 peripheral stalls the
     * AHB bus and hangs the phone. usb_debug_init() is already guarded;
     * the banner task calls this poll every loop iteration, so an unguarded
     * read here froze the FIRST task the scheduler ran (os.bin boot stage:
     * backlight 100% = sched_start fired, then AHB stall on the first
     * USB_CR read). No host exists on a card boot — skip everything. */
    return;
#else
    /* Replug: the host re-enumerates us. A pending EP0 SETUP is answered
     * FIRST — before ANY EP0 re-arm. usb_debug_init_endp0() (the old
     * link-down re-arm) clears INT_CLR_ENDP0 bit 8 = SETUP_TRANS_END, so
     * running it before the check below cleared the very flag the check
     * waits for: the host's post-reset SETUP(GET_DESCRIPTOR) was discarded
     * every round and the descriptor never answered -> Windows "device
     * descriptor request failed". Only after answering do we re-arm EP2/EP3
     * for the fresh session. */
    if (USB_CR(INT_STS_ENDP0) & (1u << 8)) {    /* SETUP_TRANS_END */
        uint32_t req = (USB_CR(REQ_SETUP_LOW) >> 8) & 0xffu;

        s_link_down = 0;
        s_tx_pending = 0;
        s_host_connected = 0;
        /* A replug starts a NEW enumeration: the host is not reading EP3 IN
         * until SET_CONFIGURATION, so any TX now would sit pending, time
         * out, and poison the fresh session (see s_session_ready). */
        s_session_ready = 0;
        /* Diagnostics (replug suspects, learnings 2026-08-21): stash the
         * request + status into the history ring so a task can log the WHOLE
         * enumeration sequence WITHOUT a kprintf here — during EP0
         * re-enumeration the host is not reading EP3 IN yet, so a kprintf
         * would spin USB_TX_BUDGET waiting for IN tokens the host is not
         * sending (bounded, but it stalls the scheduler). */
        {
            uint32_t w = s_dbg_ring_wr % USB_DBG_REPLUG_RING;

            s_dbg_setup_low[w]  = USB_CR(REQ_SETUP_LOW);
            s_dbg_setup_high[w] = USB_CR(REQ_SETUP_HIGH);
            s_dbg_sts_endp0[w]  = USB_CR(INT_STS_ENDP0);
            s_dbg_ring_wr++;
        }
        s_dbg_replug_count++;
        usb_debug_endp0_setup();                /* read REQ_SETUP + reply */
        /* Do not re-arm EP0's MAXPSIZE for a few ticks: the reply just set
         * it to the descriptor length, and re-arming MAXPSIZE=8 mid-transfer
         * clobbers the reply (host retries GET_DESCRIPTOR forever). After
         * the window the arm restores MAXPSIZE=8 + buffer-ready for the
         * host's next SETUP. */
        s_ep0_arm_until = sched_ticks() + 3;
        /* Re-arm the DATA endpoints only at the enumeration TAIL
         * (SET_ADDRESS 0x05 / SET_CONFIGURATION 0x09), NOT on every SETUP.
         * Re-initing EP2/EP3 while the host is still pulling descriptors
         * aborts any frame that is mid-stream on EP3 -> the post-connect
         * read gets a truncated frame ("response timeout") or garbage
         * ("unknown command"). By the time SET_ADDRESS lands, the
         * descriptors are accepted and no data frame is in flight. */
        if (req == 0x05u || req == 0x09u)
            usb_debug_replug_reinit();
        /* SET_CONFIGURATION (0x09) = enumeration TAIL: the host has accepted
         * the descriptors and is about to read EP3 IN. Re-open the TX gate —
         * anything before this point would have sat pending + timed out. */
        if (req == 0x09u)
            s_session_ready = 1;
    }

    /* Keep EP0 armed for a post-bus-reset SETUP at ANY time, NOT only while
     * s_link_down: that flag is set by a TX timeout, and an idle unplug (no
     * kprintf after boot) never TXes -> s_link_down stays clear -> EP0 was
     * never re-armed -> the host's first SETUP found EP0 not buffer-ready
     * and was dropped (the other "device descriptor request failed" path).
     * The re-arm is idempotent while EP0 is idle and never clears a pending
     * SETUP flag. Deferred for a few ticks after a reply (see above). */
    if (!(USB_CR(INT_STS_ENDP0) & (1u << 8)) &&
        (int32_t)(sched_ticks() - s_ep0_arm_until) >= 0)
        usb_debug_arm_endp0();

    if (USB_CR(INT_STS_ENDP2) & 1) {            /* TRANSACTION_END */
        uint8_t b = usb_debug_ep2_drain();

        if (b == USB_DBG_HOST_CONNECT && !s_host_connected) {
            s_host_connected = 1;
            /* The host is back and reading: clear a stale s_link_down (a
             * TX that timed out during the re-enumeration poisoned it) so
             * the greeting below is NOT dropped. HOST_CONNECT also proves
             * the enumeration finished, so the TX gate is open again even
             * if the SET_CONFIGURATION req was missed. */
            s_link_down = 0;
            s_session_ready = 1;
            usb_debug_send_connected();
        }
    }
#endif
}

/* ---- line buffering / kputc override ------------------------------------- */

/* fpdoom entry.c:168-172 — the 8-byte FDL ack spd_dump waits for after
 * streaming os.bin into RAM. Sent RAW on EP3 (usb_write -> usb_send(4,...),
 * usbio.c:379-393) — NOT CMD_MESSAGE framed. Until the host sees this
 * packet, spd_dump considers the load incomplete and times out. */
static const uint8_t s_fdl_ack[8] = {
    0x7e, 0, 0x80, 0, 0, 0xff, 0x7f, 0x7e
};

/* FDL1-role version reply (fpdoom sdboot entry.c:211-222 verbatim): the
 * BSL_REP_VER (0x81) frame spd_dump's post-EXEC FDL1 handshake awaits.
 * spd_dump loads the image, EXEC_DATA jumps to it, then it sends
 * BSL_CMD_CHECK_BAUD and expects THIS frame back (type 0x0081, len 0x22 =
 * 34, "Spreadtrum Boot Block version 1.2", checksum 0xa9e9). Without it
 * spd_dump's CHECK_BAUD send times out -> "usb_send failed :
 * LIBUSB_ERROR_TIMEOUT" even though the load succeeded (the menu boots,
 * libc_server connects — the error is spd_dump's cosmetic FDL1 noise).
 * The ack (0x80) alone is not enough; sdboot sends VER before the ack.
 * ONLY for the FDL1 role (menu.bin at the 0x40004000 FDL slot). os.bin is
 * loaded as an FDL2 (`spd_dump fdl nor_fdl1.bin 0x40004000 fdl os.bin
 * ram`): spd_dump expects ONLY the ack there, so it must NOT send this. */
static const uint8_t s_fdl_ver[44] = {
    0x7e, 0, 0x81, 0, 0x22,                 /* HDLC header, BSL_REP_VER, len 0x22 */
    'S','p','r','e','a','d','t','r','u','m',' ','B','o','o','t',' ',
    'B','l','o','c','k',' ','v','e','r','s','i','o','n',' ','1','.','2',
    0,                                      /* NUL terminator (len 0x22 = 34) */
    0xa9, 0xe9, 0x7e, 0, 0                  /* checksum, end marker, pad */
};

static char s_line[USB_DBG_MAX_PAYLOAD];
static int s_line_len;
static int s_ready;                     /* set by usb_debug_init() */
static int s_fdl1_role;                 /* FDL1 role: send BSL_REP_VER first */

static void usb_debug_flush(void)
{
    int len = s_line_len;
    int p = len;
    uint8_t *f = s_frame;

    /* strip one trailing CR/LF so the host's "!!! %s\n" prints cleanly */
    if (p > 0 && s_line[p - 1] == '\n')
        p--;
    if (p > 0 && s_line[p - 1] == '\r')
        p--;
    if (p == 0)
        p = len;                        /* blank line: keep the '\n' */

    if (usb_debug_frame_build(s_line, p, f, (int)sizeof(s_frame)) >= 0)
        usb_debug_send_raw(f, 4u + (uint32_t)p);
    s_line_len = 0;
}

void usb_debug_putc(char c)
{
    if (!s_ready)
        return;                         /* before init: drop (weak ring owns it) */
    if (s_line_len < (int)sizeof(s_line))
        s_line[s_line_len++] = c;
    if (c == '\n' || s_line_len == (int)sizeof(s_line))
        usb_debug_flush();
}

#ifndef HOST_TEST
/* Strong kputc override: the kernel's weak default (printk.c ring buffer)
 * loses at link time, so ALL kprintf output streams to the host console. */
void kputc(char c)
{
    usb_debug_putc(c);
    console_putc(c);    /* also feed the LCD console ring (printk.h) */
}
#endif

/* ---- endpoint re-init (fpdoom usbio.c:103-134, 308-325, Unlicense) ------ */

static void usb_debug_init_endp0(void)
{
    USB_MAXPSIZE(ENDP0_CTRL, 8);
    USB_CR(INT_CLR_ENDP0) |= 1u << 8;
    USB_CR(INT_CTRL_ENDP0) |= 1u << 8;
    USB_CR(ENDP0_CTRL) |= 1u << 28;     /* buffer ready */
}

static void usb_debug_init_endp2(void)
{
    USB_MAXPSIZE(ENDP2_CTRL, 0x40);
    USB_TRSIZE(RCV_DATA_ENDP2, 0x2000);
    USB_CR(INT_CLR_ENDP2) = 0x3fff;
    USB_CR(INT_CTRL_ENDP2) = 0;
    USB_CR(INT_CLR_ENDP2) |= 1;
    USB_CR(INT_CTRL_ENDP2) |= 1;
    USB_CR(ENDP2_CTRL) |= 1u << 25;     /* endpoint enable */
    USB_CR(ENDP2_CTRL) |= 1u << 28;     /* buffer ready */
}

static void usb_debug_init_endp3(void)
{
    USB_MAXPSIZE(ENDP3_CTRL, 0x40);
    USB_TRSIZE(TRANS_SIZE_ENDP3, 0x40);
    USB_CR(INT_CLR_ENDP3) = 0x3fff;
    USB_CR(INT_CTRL_ENDP3) = 0;
    USB_CR(INT_CLR_ENDP3) |= 1u << 9;
    USB_CR(INT_CTRL_ENDP3) |= 1u << 9;
    /* clear a possibly-still-armed TX (a replug mid-frame aborts the
     * transfer but leaves bit 27 set -> the post-replug host read gets
     * garbage -> libc_server "unknown command") */
    USB_CR(ENDP3_CTRL) &= ~(1u << 27);
    USB_CR(ENDP3_CTRL) |= 1u << 25;     /* endpoint enable */
}

/* Full controller re-init for a replug session (learnings 2026-08-21,
 * replug suspect (a)): a USB bus reset may need MORE than endpoint re-arms
 * — re-assert USB_ENABLE, rewrite TIMEOUT_LMT, and re-run the complete
 * endpoint init (the whole usb_debug_init() minus fdl_ack + connect wait).
 * fpdoom never does this (its binaries block on HOST_CONNECT at boot, so
 * the ROM bootloader's enumeration is never disturbed); the SC6530C's
 * post-reset controller state is undocumented, so this covers the case
 * where the reset clears USB_CTRL/TIMEOUT_LMT too. MUST be called AFTER
 * usb_debug_endp0_setup() consumed the pending SETUP: usb_debug_init_endp0
 * clears INT_CLR_ENDP0 bit 8 (SETUP_TRANS_END), and clearing it before the
 * answer would discard the very request we must reply to (FIX 3 rule). */
static void usb_debug_replug_reinit(void)
{
    /* Data endpoints only. EP0 is managed by the answer path + the idle
     * re-arm (arm_endp0, with MAXPSIZE now) — re-running init_endp0 here
     * clears SETUP_TRANS_END (INT_CLR bit 8) and can EAT the host's NEXT
     * SETUP during the multi-request enumeration (GET_DESCRIPTOR(config),
     * SET_ADDRESS, SET_CONFIGURATION), failing the rebind after the first
     * descriptor. The data endpoints are wiped by the bus reset, so they
     * DO need re-init for the fresh libc_server session. */
    usb_debug_init_endp2();
    usb_debug_init_endp3();
    USB_CR(TIMEOUT_LMT) = 15;           /* 12 MHz / 15 = 800 kHz */
}

/* Copy the replug SETUP history ring out (see the statics above). The ring
 * holds the last USB_DBG_REPLUG_RING SETUPs: {REQ_SETUP_LOW, REQ_SETUP_HIGH,
 * INT_STS_ENDP0} per slot, oldest first. `n` must be >= USB_DBG_REPLUG_RING.
 * Returns the total number of replug SETUPs seen (0 = none since boot). */
uint32_t usb_debug_replug_diag(uint32_t *setup_low, uint32_t *setup_high,
                               uint32_t *sts_endp0)
{
    uint32_t w, n = s_dbg_replug_count < USB_DBG_REPLUG_RING
                        ? s_dbg_replug_count : USB_DBG_REPLUG_RING;
    uint32_t start = (s_dbg_ring_wr - n) & (USB_DBG_REPLUG_RING - 1u);
    uint32_t i;

    for (i = 0; i < n; i++) {
        w = (start + i) & (USB_DBG_REPLUG_RING - 1u);
        if (setup_low)   setup_low[i]   = s_dbg_setup_low[w];
        if (setup_high)  setup_high[i]  = s_dbg_setup_high[w];
        if (sts_endp0)   sts_endp0[i]   = s_dbg_sts_endp0[w];
    }
    return s_dbg_replug_count;
}

/* Arm the USB endpoints ONLY (no transfers, no waits) — MUST run as the
 * FIRST thing after the boot stub, BEFORE any slow pre-init: spd_dump's
 * post-EXEC FDL1 handshake sends BSL_CMD_CHECK_BAUD on EP2 OUT within
 * microseconds of EXEC_DATA, with a 1s libusb timeout. If EP2 is not
 * buffer-ready by then, the OUT transfer NAKs for the full second and
 * libusb reports "usb_send failed : LIBUSB_ERROR_TIMEOUT" at the DRIVER
 * level (not a protocol error). The proven FDL1 (custom_fdl) arms USB
 * right after its chip init; our menu's keypad pre-check takes >1s at the
 * ROM clock, so the arm must come first. */
void usb_debug_arm_endpoints(void)
{
    USB_CR(USB_CTRL) |= 1u;             /* USB_ENABLE */
    usb_debug_init_endp0();
    usb_debug_init_endp2();
    usb_debug_init_endp3();
    USB_CR(TIMEOUT_LMT) = 15;           /* 12 MHz / 15 = 800 kHz */
    /* Boot session: the ROM/FDL bootloader already enumerated + configured
     * the device (we inherited its address), so the session is READY. The
     * fdl_ack and the connect greeting below must NOT be gated. */
    s_session_ready = 1;
}

/* Boot handshake frames on EP3 (fdl_ver if FDL1-role, then the fdl_ack).
 * Run AFTER the chip init has ramped the clock to 208MHz so the bounded
 * TRANSFER_END waits are fast. */
static void usb_debug_send_boot_frames(void)
{
    /* FDL1-role (menu.bin at the FDL slot): send the BSL_REP_VER frame
     * FIRST — spd_dump's post-EXEC FDL1 handshake (BSL_CMD_CHECK_BAUD)
     * awaits it. The ack alone is not enough; without the VER, spd_dump's
     * CHECK_BAUD send times out ("usb_send failed : LIBUSB_ERROR_TIMEOUT")
     * even though the load succeeded. Same transfer pattern as the ack:
     * first transfer on EP3, no previous-transfer gate. */
    if (s_fdl1_role) {
        USB_MAXPSIZE(ENDP3_CTRL, 44u);
        USB_TRSIZE(TRANS_SIZE_ENDP3, 44u);
        usb_debug_fifo_write((volatile uint32_t *)USB_FIFO_ENDP3, s_fdl_ver, 44u);
        USB_CR(ENDP3_CTRL) |= 1u << 27;     /* TX start */
        s_tx_pending = 1;
        {
            uint32_t n = USB_TX_BUDGET;
            while (!(USB_CR(INT_STS_ENDP3) & (1u << 9)) && --n != 0)
                ;
            USB_CR(INT_CLR_ENDP3) |= 1u << 9;
            if (n != 0)
                s_tx_pending = 0;
        }
    }

    /* FDL ack: spd_dump awaits this 8-byte response before it considers
     * the RAM-load done and lets libc_server open the device. This is the
     * FIRST transfer on EP3 (or the second, after the VER), so there is no
     * previous-transfer gate — the exact fpdoom usb_write(fdl_ack, 8)
     * flow (usbio.c:379-393). */
    USB_MAXPSIZE(ENDP3_CTRL, 8u);
    USB_TRSIZE(TRANS_SIZE_ENDP3, 8u);
    usb_debug_fifo_write((volatile uint32_t *)USB_FIFO_ENDP3, s_fdl_ack, 8u);
    USB_CR(ENDP3_CTRL) |= 1u << 27;     /* TX start */
    s_tx_pending = 1;
    {
        uint32_t n = USB_TX_BUDGET;     /* bounded: needs spd_dump IN tokens */
        while (!(USB_CR(INT_STS_ENDP3) & (1u << 9)) && --n != 0)
            ;
        USB_CR(INT_CLR_ENDP3) |= 1u << 9;
        if (n != 0)
            s_tx_pending = 0;           /* ack read: EP3 free for the next frame */
    }

    s_line_len = 0;
    s_ready = 1;
}

int usb_debug_init(void)
{
#ifdef SD_BOOT_NO_USB
    /* Card boot (`make os-sd`): the USB block is UNPOWERED — a register
     * write to an unpowered SC6530 peripheral stalls the AHB bus and hangs
     * the phone, and no host exists anyway. Skip the whole init; the
     * send path already no-ops (s_session_ready == 0). */
    return 0;
#else
    usb_debug_arm_endpoints();
    usb_debug_send_boot_frames();
    usb_debug_wait_connect();
    return 0;
#endif
}

const module_t usb_debug_module = { "usb_debug", usb_debug_init };

/* FDL1-role marker (menu.bin loaded at the 0x40004000 FDL slot): the next
 * usb_debug_init() sends the BSL_REP_VER frame before the ack so spd_dump's
 * post-EXEC CHECK_BAUD handshake completes (no "usb_send failed" noise).
 * os.bin (loaded as FDL2) must NOT call this — spd_dump expects only the
 * ack there, and a stray VER frame breaks the FDL2 load. */
void usb_debug_set_fdl1(void)
{
    s_fdl1_role = 1;
}
