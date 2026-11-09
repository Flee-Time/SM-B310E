/* B310E OS — USB network adapter: Remote NDIS (RNDIS) over a CDC-style
 * control interface and a data interface.
 *
 * Interfaces (one function, grouped by an interface association):
 *   0  control, class E0/01/03 ("RNDIS", the class Android tethering
 *      uses), CDC header, call management, ACM and union descriptors;
 *      EP3 IN interrupt: RESPONSE_AVAILABLE notifications
 *   1  data, class 0A: EP1 IN and EP2 OUT bulk, RNDIS_PACKET_MSG frames
 *
 * Windows 10 and 11 bind their own RNDIS driver without an .inf in two
 * ways, and the device offers both:
 *   - the MS OS 1.0 descriptors: string 0xee "MSFT100" with vendor code
 *     MS_VENDOR_CODE, and the extended compat ID descriptor (vendor
 *     request, wIndex 4) naming interface 0 compatible "RNDIS",
 *     sub-compatible "5162001": USB\MS_COMP_RNDIS&MS_SUBCOMP_5162001,
 *     which Windows' rndiscmp.inf matches (what Linux's gadget RNDIS
 *     function offers Windows);
 *   - the class triple E0/01/03 on the association and interface 0,
 *     which rndiscmp.inf matches as well (USB\Class_E0&SubClass_01&Prot_03).
 * Linux's rndis_host binds E0/01/03 (its "RNDIS for tethering devices"
 * entry) and needs the CDC union and the interrupt endpoint.
 *
 * Windows reads the MS OS string once per VID/PID/bcdDevice and
 * remembers the answer (HKLM\SYSTEM\CurrentControlSet\Control\usbflags):
 * mass storage, 1209:0001 1.00, never had one, so the network function is
 * bcdDevice 2.00 with its own serial number (a device instance of its
 * own, never bound to the storage driver).
 *
 * Control messages arrive as SEND_ENCAPSULATED_COMMAND and are answered
 * in the interrupt handler; the answer waits for GET_ENCAPSULATED_RESPONSE
 * after a RESPONSE_AVAILABLE notification on EP3. Data: one Ethernet
 * frame per bulk transfer each way (MaxPacketsPerTransfer 1); a frame the
 * phone sends whose transfer would end on a packet boundary gets one pad
 * byte instead of a zero-length packet.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/drivers.h"
#include "os/fs.h"
#include "os/kernel.h"
#include "os/lib.h"
#include "os/net.h"
#include "os/usb.h"
#include "drivers/usb_internal.h"

#define VID             0x1209
#define PID             0x0001
#define MS_VENDOR_CODE  0x42

/* messages */
#define MSG_PACKET          0x00000001u
#define MSG_INIT            0x00000002u
#define MSG_HALT            0x00000003u
#define MSG_QUERY           0x00000004u
#define MSG_SET             0x00000005u
#define MSG_RESET           0x00000006u
#define MSG_KEEPALIVE       0x00000008u
#define MSG_CMPLT           0x80000000u

#define ST_SUCCESS          0x00000000u
#define ST_FAILURE          0xc0000001u
#define ST_INVALID_DATA     0xc0010015u
#define ST_NOT_SUPPORTED    0xc00000bbu

/* OIDs */
#define OID_GEN_SUPPORTED_LIST          0x00010101u
#define OID_GEN_HARDWARE_STATUS         0x00010102u
#define OID_GEN_MEDIA_SUPPORTED         0x00010103u
#define OID_GEN_MEDIA_IN_USE            0x00010104u
#define OID_GEN_MAXIMUM_LOOKAHEAD       0x00010105u
#define OID_GEN_MAXIMUM_FRAME_SIZE      0x00010106u
#define OID_GEN_LINK_SPEED              0x00010107u
#define OID_GEN_TRANSMIT_BLOCK_SIZE     0x0001010au
#define OID_GEN_RECEIVE_BLOCK_SIZE      0x0001010bu
#define OID_GEN_VENDOR_ID               0x0001010cu
#define OID_GEN_VENDOR_DESCRIPTION      0x0001010du
#define OID_GEN_CURRENT_PACKET_FILTER   0x0001010eu
#define OID_GEN_CURRENT_LOOKAHEAD       0x0001010fu
#define OID_GEN_MAXIMUM_TOTAL_SIZE      0x00010111u
#define OID_GEN_MAC_OPTIONS             0x00010113u
#define OID_GEN_MEDIA_CONNECT_STATUS    0x00010114u
#define OID_GEN_MAXIMUM_SEND_PACKETS    0x00010115u
#define OID_GEN_VENDOR_DRIVER_VERSION   0x00010116u
#define OID_GEN_NETWORK_LAYER_ADDRESSES 0x00010118u
#define OID_GEN_PHYSICAL_MEDIUM         0x00010202u
#define OID_GEN_RNDIS_CONFIG_PARAMETER  0x0001021bu
#define OID_GEN_XMIT_OK                 0x00020101u
#define OID_GEN_RCV_OK                  0x00020102u
#define OID_GEN_XMIT_ERROR              0x00020103u
#define OID_GEN_RCV_ERROR               0x00020104u
#define OID_GEN_RCV_NO_BUFFER           0x00020105u
#define OID_802_3_PERMANENT_ADDRESS     0x01010101u
#define OID_802_3_CURRENT_ADDRESS       0x01010102u
#define OID_802_3_MULTICAST_LIST        0x01010103u
#define OID_802_3_MAXIMUM_LIST_SIZE     0x01010104u
#define OID_802_3_MAC_OPTIONS           0x01010105u
#define OID_802_3_RCV_ERROR_ALIGNMENT   0x01020101u
#define OID_802_3_XMIT_ONE_COLLISION    0x01020102u
#define OID_802_3_XMIT_MORE_COLLISIONS  0x01020103u

static const uint32_t supported[] = {
    OID_GEN_SUPPORTED_LIST, OID_GEN_HARDWARE_STATUS, OID_GEN_MEDIA_SUPPORTED,
    OID_GEN_MEDIA_IN_USE, OID_GEN_MAXIMUM_LOOKAHEAD, OID_GEN_MAXIMUM_FRAME_SIZE,
    OID_GEN_LINK_SPEED, OID_GEN_TRANSMIT_BLOCK_SIZE, OID_GEN_RECEIVE_BLOCK_SIZE,
    OID_GEN_VENDOR_ID, OID_GEN_VENDOR_DESCRIPTION, OID_GEN_CURRENT_PACKET_FILTER,
    OID_GEN_CURRENT_LOOKAHEAD, OID_GEN_MAXIMUM_TOTAL_SIZE, OID_GEN_MAC_OPTIONS,
    OID_GEN_MEDIA_CONNECT_STATUS, OID_GEN_MAXIMUM_SEND_PACKETS,
    OID_GEN_VENDOR_DRIVER_VERSION, OID_GEN_PHYSICAL_MEDIUM,
    OID_GEN_XMIT_OK, OID_GEN_RCV_OK, OID_GEN_XMIT_ERROR, OID_GEN_RCV_ERROR,
    OID_GEN_RCV_NO_BUFFER, OID_802_3_PERMANENT_ADDRESS,
    OID_802_3_CURRENT_ADDRESS, OID_802_3_MULTICAST_LIST,
    OID_802_3_MAXIMUM_LIST_SIZE, OID_802_3_MAC_OPTIONS,
    OID_802_3_RCV_ERROR_ALIGNMENT, OID_802_3_XMIT_ONE_COLLISION,
    OID_802_3_XMIT_MORE_COLLISIONS,
};

/* ---- descriptors -------------------------------------------------------- */

static const uint8_t dev_desc[18] __attribute__((aligned(4))) = {
    18, 1, 0x00, 0x02,          /* device, USB 2.0 (full speed) */
    0xef, 0x02, 0x01, USB_PKT,  /* miscellaneous: interface associations */
    VID & 0xff, VID >> 8, PID & 0xff, PID >> 8,
    0x00, 0x02, 1, 2, 3, 1,     /* bcdDevice 2.00, strings, 1 config */
};

#define CFG_LEN (9 + 8 + 9 + 5 + 5 + 4 + 5 + 7 + 9 + 7 + 7)

static const uint8_t cfg_desc[CFG_LEN] __attribute__((aligned(4))) = {
    9, 2, CFG_LEN, 0, 2, 1, 0, 0x80, 250,   /* 2 interfaces, bus powered, 500 mA */
    8, 0x0b, 0, 2, 0xe0, 0x01, 0x03, 0,     /* association: interfaces 0-1, RNDIS */
    9, 4, 0, 0, 1, 0xe0, 0x01, 0x03, 0,     /* control: wireless, RF, RNDIS */
    5, 0x24, 0x00, 0x10, 0x01,              /* CDC header 1.10 */
    5, 0x24, 0x01, 0x00, 0x01,              /* call management: data on 1 */
    4, 0x24, 0x02, 0x00,                    /* ACM, no capabilities */
    5, 0x24, 0x06, 0, 1,                    /* union: master 0, slave 1 */
    7, 5, 0x83, 3, 8, 0, 1,                 /* EP3 IN interrupt, 8 bytes, 1 ms */
    9, 4, 1, 0, 2, 0x0a, 0x00, 0x00, 0,     /* data */
    7, 5, 0x81, 2, USB_PKT, 0, 0,           /* EP1 IN bulk */
    7, 5, 0x02, 2, USB_PKT, 0, 0,           /* EP2 OUT bulk */
};

static const char *const strings[] = {
    NULL, "B310E OS", "B310E OS network", "B310E0000002",
};

/* MS OS 1.0 extended compat ID: interface 0 is "RNDIS" / "5162001" */
static const uint8_t compat_id[40] __attribute__((aligned(4))) = {
    40, 0, 0, 0, 0x00, 0x01, 0x04, 0x00,    /* length, version 1.00, wIndex 4 */
    1, 0, 0, 0, 0, 0, 0, 0,                 /* one function */
    0, 1,                                   /* interface 0, reserved (1) */
    'R', 'N', 'D', 'I', 'S', 0, 0, 0,
    '5', '1', '6', '2', '0', '0', '1', 0,
    0, 0, 0, 0, 0, 0,
};

static bool rndis_request(uint8_t bm, uint8_t rq, uint16_t value,
                          uint16_t index, uint16_t len);
static void rndis_ep0_done(int len);
static void rndis_notified(void);
static void rndis_reset(void);
static int rndis_out_len(const uint8_t *buf, int got);

const usb_function_t usb_fn_network = {
    .dev_desc = dev_desc, .cfg_desc = cfg_desc, .cfg_len = CFG_LEN,
    .strings = strings, .n_strings = ARRAY_SIZE(strings),
    .ms_vendor_code = MS_VENDOR_CODE, .use_ep3 = true,
    .request = rndis_request, .ep0_out_done = rndis_ep0_done,
    .intr_done = rndis_notified, .reset = rndis_reset,
    .out_len = rndis_out_len,
    .init = usb_rndis_init, .run = usb_rndis_run, .exit = usb_rndis_exit,
};

/* ---- control messages (interrupt handler) ------------------------------- */

#define CMD_MAX   1024
#define RESP_MAX  256
#define RESP_N    4

static uint32_t cmd[CMD_MAX / 4];
static struct {
    uint32_t len;
    uint32_t w[RESP_MAX / 4];
} resp[RESP_N];
static unsigned resp_head, resp_count;
static uint32_t notification[2] = { 1, 0 };     /* RESPONSE_AVAILABLE */
static unsigned notify_pending;
static uint32_t packet_filter;
static uint32_t host_max_transfer;
static bool initialized;
static uint32_t rx_ok, tx_ok, rx_err, tx_err, rx_nobuf;
static const uint8_t zero_byte[4] __attribute__((aligned(4)));

static uint32_t le32(const void *p)
{
    const uint8_t *b = p;

    return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 |
           (uint32_t)b[3] << 24;
}

static void set_filter(uint32_t f)
{
    packet_filter = f;
    net_stat.packet_filter = f;
    net_link(f != 0);
}

static void notify(void)
{
    if (usb_intr_busy() || usb_intr_send(notification, 8) < 0)
        notify_pending++;
}

static void rndis_notified(void)
{
    if (notify_pending && usb_intr_send(notification, 8) == 0)
        notify_pending--;
}

/* A response slot; the oldest is dropped when the host reads none. */
static uint32_t *resp_new(void)
{
    unsigned i;

    if (resp_count == RESP_N) {
        resp_head = (resp_head + 1) % RESP_N;
        resp_count--;
    }
    i = (resp_head + resp_count) % RESP_N;
    memset(resp[i].w, 0, sizeof(resp[i].w));
    return resp[i].w;
}

static void resp_push(uint32_t len)
{
    unsigned i = (resp_head + resp_count) % RESP_N;

    /* a control read shorter than the host asked that ends on a packet
     * boundary would need a zero-length packet: four bytes more instead
     * (the length fields cover them) */
    if (len % USB_PKT == 0)
        len += 4;
    resp[i].w[1] = len;
    resp[i].len = len;
    resp_count++;
    notify();
}

static uint32_t query(uint32_t oid, uint32_t *out, uint32_t *status)
{
    uint8_t *b = (uint8_t *)out;
    uint32_t v;

    *status = ST_SUCCESS;
    switch (oid) {
    case OID_GEN_SUPPORTED_LIST:
        memcpy(out, supported, sizeof(supported));
        return sizeof(supported);
    case OID_GEN_VENDOR_DESCRIPTION:
        memcpy(b, "B310E OS RNDIS", 15);
        return 16;
    case OID_802_3_PERMANENT_ADDRESS:
    case OID_802_3_CURRENT_ADDRESS:
        memcpy(b, net_host_mac, 6);
        return 6;
    case OID_802_3_MULTICAST_LIST:
        return 0;                       /* every frame is passed up anyway */
    case OID_GEN_HARDWARE_STATUS:       v = 0; break;      /* ready */
    case OID_GEN_MEDIA_SUPPORTED:
    case OID_GEN_MEDIA_IN_USE:          v = 0; break;      /* 802.3 */
    case OID_GEN_PHYSICAL_MEDIUM:       v = 0; break;      /* unspecified */
    case OID_GEN_MAXIMUM_LOOKAHEAD:
    case OID_GEN_CURRENT_LOOKAHEAD:
    case OID_GEN_MAXIMUM_FRAME_SIZE:    v = NET_MTU; break;
    case OID_GEN_LINK_SPEED:            v = 120000; break; /* 12 Mbit/s, in 100 bit/s */
    case OID_GEN_TRANSMIT_BLOCK_SIZE:
    case OID_GEN_RECEIVE_BLOCK_SIZE:    v = NET_FRAME_MAX; break;
    case OID_GEN_MAXIMUM_TOTAL_SIZE:    v = NET_HEADROOM + NET_FRAME_MAX; break;
    case OID_GEN_VENDOR_ID:             v = 0x00ffffff; break;
    case OID_GEN_VENDOR_DRIVER_VERSION: v = 0x0100; break;
    case OID_GEN_CURRENT_PACKET_FILTER: v = packet_filter; break;
    case OID_GEN_MAC_OPTIONS:
    case OID_802_3_MAC_OPTIONS:         v = 0; break;
    case OID_GEN_MEDIA_CONNECT_STATUS:  v = 0; break;      /* connected */
    case OID_GEN_MAXIMUM_SEND_PACKETS:  v = 1; break;
    case OID_802_3_MAXIMUM_LIST_SIZE:   v = 32; break;
    case OID_GEN_XMIT_OK:               v = tx_ok; break;
    case OID_GEN_RCV_OK:                v = rx_ok; break;
    case OID_GEN_XMIT_ERROR:            v = tx_err; break;
    case OID_GEN_RCV_ERROR:             v = rx_err; break;
    case OID_GEN_RCV_NO_BUFFER:         v = rx_nobuf; break;
    case OID_802_3_RCV_ERROR_ALIGNMENT:
    case OID_802_3_XMIT_ONE_COLLISION:
    case OID_802_3_XMIT_MORE_COLLISIONS: v = 0; break;
    default:
        *status = ST_NOT_SUPPORTED;
        return 0;
    }
    out[0] = v;
    return 4;
}

static uint32_t set(uint32_t oid, const uint8_t *data, uint32_t len)
{
    switch (oid) {
    case OID_GEN_CURRENT_PACKET_FILTER:
        if (len < 4)
            return ST_INVALID_DATA;
        set_filter(le32(data));
        return ST_SUCCESS;
    case OID_802_3_MULTICAST_LIST:      /* nothing filtered: accepted */
    case OID_GEN_CURRENT_LOOKAHEAD:
    case OID_GEN_RNDIS_CONFIG_PARAMETER:
    case OID_GEN_NETWORK_LAYER_ADDRESSES:
        return ST_SUCCESS;
    }
    return ST_NOT_SUPPORTED;
}

static void rndis_command(const uint8_t *m, int len)
{
    uint32_t type, id, *r;

    if (len < 12 || le32(m + 4) > (uint32_t)len)
        return;
    type = le32(m);
    id = le32(m + 8);
    net_stat.rndis_msgs++;
    {                                   /* the first messages, for the log */
        static unsigned logged;

        if (logged++ < 40)
            klog("rndis: msg %08lx, %d bytes, oid %08lx, data %08lx",
                 (unsigned long)type, len,
                 (unsigned long)(len >= 16 && (type == MSG_QUERY || type == MSG_SET) ?
                                 le32(m + 12) : 0),
                 (unsigned long)(type == MSG_SET && len >= 28 &&
                                 le32(m + 20) <= (uint32_t)len - 12 ?
                                 le32(m + le32(m + 20) + 8) : 0));
    }
    switch (type) {
    case MSG_INIT:
        if (len < 24)
            return;
        host_max_transfer = le32(m + 20);
        initialized = true;
        net_host_restarted();
        set_filter(0);
        net_stat.rndis_inits++;
        r = resp_new();
        r[0] = MSG_INIT | MSG_CMPLT;
        r[2] = id;
        r[3] = ST_SUCCESS;
        r[4] = 1;                       /* version 1.0 */
        r[5] = 0;
        r[6] = 1;                       /* RNDIS_DF_CONNECTIONLESS */
        r[7] = 0;                       /* medium 802.3 */
        r[8] = 1;                       /* packets per transfer */
        r[9] = NET_RX_BUF;              /* the largest transfer we take */
        r[10] = 0;                      /* alignment: 2^0 */
        r[11] = r[12] = 0;              /* no AF list */
        resp_push(52);
        return;
    case MSG_HALT:
        initialized = false;
        set_filter(0);
        return;                         /* no answer */
    case MSG_QUERY: {
        uint32_t status, n;

        if (len < 28)
            return;
        r = resp_new();
        n = query(le32(m + 12), r + 6, &status);
        r[0] = MSG_QUERY | MSG_CMPLT;
        r[2] = id;
        r[3] = status;
        r[4] = n;
        r[5] = n ? 16 : 0;              /* from the request id */
        resp_push(24 + n);
        return;
    }
    case MSG_SET: {
        uint32_t n, off;

        if (len < 28)
            return;
        n = le32(m + 16);
        off = le32(m + 20) + 8;
        r = resp_new();
        r[0] = MSG_SET | MSG_CMPLT;
        r[2] = id;
        r[3] = off > (uint32_t)len || n > (uint32_t)len - off ? ST_INVALID_DATA : set(le32(m + 12), m + off, n);
        resp_push(16);
        return;
    }
    case MSG_RESET:
        /* the host found the device hung: drop what it never read */
        resp_head = resp_count = 0;
        notify_pending = 0;
        r = resp_new();
        r[0] = MSG_RESET | MSG_CMPLT;
        r[2] = ST_SUCCESS;              /* (no request id) */
        r[3] = 0;                       /* addressing kept: filter stays */
        resp_push(16);
        return;
    case MSG_KEEPALIVE:
        r = resp_new();
        r[0] = MSG_KEEPALIVE | MSG_CMPLT;
        r[2] = id;
        r[3] = ST_SUCCESS;
        resp_push(16);
        return;
    }
}

static void rndis_ep0_done(int len)
{
    rndis_command((const uint8_t *)cmd, len);
}

static bool rndis_request(uint8_t bm, uint8_t rq, uint16_t value,
                          uint16_t index, uint16_t len)
{
    if ((bm & 0x60) == 0x40 && rq == MS_VENDOR_CODE) {
        if ((bm & 0x80) && index == 4) {        /* extended compat ID */
            usb_ep0_send(compat_id, sizeof(compat_id));
            return true;
        }
        return false;                   /* no extended properties */
    }
    if ((bm & 0x7f) != 0x21 || index != 0)      /* class, interface 0 */
        return false;
    if (bm == 0x21 && rq == 0x00) {     /* SEND_ENCAPSULATED_COMMAND */
        if (len == 0 || len > CMD_MAX)
            return false;
        usb_ep0_recv(cmd, len);
        return true;
    }
    if (bm == 0xa1 && rq == 0x01) {     /* GET_ENCAPSULATED_RESPONSE */
        if (!resp_count) {
            usb_ep0_send(zero_byte, 1); /* none: a single zero byte */
            return true;
        }
        /* the slot stays as it is until RESP_N - 1 more answers: the
         * reply is read from it while the host takes it */
        usb_ep0_send(resp[resp_head].w, (int)resp[resp_head].len);
        resp_head = (resp_head + 1) % RESP_N;
        resp_count--;
        return true;
    }
    return false;
}

/* bus reset or a new configuration: the host starts over with INITIALIZE */
static void rndis_reset(void)
{
    initialized = false;
    resp_head = resp_count = 0;
    notify_pending = 0;
    set_filter(0);
}

static int rndis_out_len(const uint8_t *buf, int got)
{
    uint32_t n;

    if (got < 8 || le32(buf) != MSG_PACKET)
        return 0;
    n = le32(buf + 4);
    return n >= 8 && n <= NET_RX_BUF ? (int)n : 0;
}

/* ---- data --------------------------------------------------------------- */

/* The net thread: one frame, at buf + NET_HEADROOM, as one transfer. */
static int rndis_send(uint8_t *buf, int len)
{
    uint32_t gen, *h = (uint32_t *)buf, total = NET_HEADROOM + (uint32_t)len;
    int r;

    if (!packet_filter || !usb_wait_configured(&gen)) {
        if (tx_err++ < 3)
            klog("rndis: send refused (%s)", !packet_filter ? "no filter" : "not configured");
        return -1;
    }
    if (total % USB_PKT == 0)
        buf[total++] = 0;               /* a pad byte, not a zero-length packet */
    h[0] = MSG_PACKET;
    h[1] = total;
    h[2] = NET_HEADROOM - 8;            /* data offset, from byte 8 */
    h[3] = (uint32_t)len;
    for (int i = 4; i < NET_HEADROOM / 4; i++)
        h[i] = 0;
    if (usb_bulk_start_in(gen, buf, (int)total) < 0) {
        if (tx_err++ < 3)
            klog("rndis: send of %lu bytes not started", (unsigned long)total);
        return -1;
    }
    /* a PC reads all the time; a frame it has not taken in a second is
     * dropped (TCP sends it again). 300 ms was short for the QEMU test's
     * Python host, which can stall that long under load. */
    r = usb_bulk_wait_in_ms(1000);
    if (r < 0) {
        if (tx_err++ < 3)
            klog("rndis: send of %lu bytes not taken by the host in 1 s (%d)",
                 (unsigned long)total, r);
        net_stat.tx_timeouts++;
        return -1;
    }
    tx_ok++;
    return 0;
}

bool usb_rndis_init(void)
{
    rndis_reset();
    rx_ok = tx_ok = rx_err = tx_err = rx_nobuf = 0;
    return net_start(rndis_send) == 0;
}

void usb_rndis_exit(void)
{
    net_stop();
}

/* The session thread: receive frames into the stack's buffers. */
void usb_rndis_run(void)
{
    uint32_t gen = 0;

    while (!usb_stopping()) {
        uint8_t *b;
        int n;
        uint32_t mlen, off, dlen;

        if (!usb_wait_configured(&gen))
            continue;
        b = net_rx_get(100);
        if (!b) {
            rx_nobuf++;
            continue;
        }
        if (usb_bulk_start_out(gen, b, NET_RX_BUF) < 0) {
            net_rx_free(b);
            continue;
        }
        n = usb_bulk_wait_out();
        if (n < 0 || !usb_gen_valid(gen)) {
            net_rx_free(b);
            continue;
        }
        if (n < 44 || le32(b) != MSG_PACKET) {
            net_rx_free(b);             /* the pad byte or zero-length packet after a frame */
            continue;
        }
        mlen = le32(b + 4);
        off = le32(b + 8) + 8;
        dlen = le32(b + 12);
        if (mlen > (uint32_t)n || off > mlen || dlen > mlen - off || dlen < 14 ||
            dlen > NET_FRAME_MAX) {
            rx_err++;
            net_stat.rx_dropped++;
            net_rx_free(b);
            continue;
        }
        rx_ok++;
        {                               /* the first IPv4 frames, for the log */
            static unsigned logged;
            const uint8_t *f = b + off;

            if (dlen >= 34 && f[12] == 0x08 && f[13] == 0x00 && logged++ < 6)
                klog("rndis: frame in: transfer %d, msg %lu, off %lu, len %lu; ip %02x %02x "
                     "%02x%02x .. proto %u %u.%u.%u.%u -> %u.%u.%u.%u; tail %02x %02x %02x %02x",
                     n, (unsigned long)mlen, (unsigned long)off, (unsigned long)dlen,
                     f[14], f[15], f[16], f[17], f[23], f[26], f[27], f[28], f[29],
                     f[30], f[31], f[32], f[33], f[dlen - 4], f[dlen - 3], f[dlen - 2],
                     f[dlen - 1]);
        }
        net_rx_frame(b, (int)off, (int)dlen);
    }
}
