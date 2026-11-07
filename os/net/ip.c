/* B310E OS — the network stack's core: frame buffers, the net thread,
 * Ethernet, ARP, IPv4, ICMP echo, UDP and the DHCP server.
 *
 * The link is point to point (one PC on the USB cable), so: one subnet,
 * no routing, a four-entry ARP table filled from the PC's own frames,
 * IPv4 without fragments or options, one DHCP lease (NET_HOST_IP) for
 * whoever asks. IPv6 and everything else is dropped.
 *
 * Frames come in through NET_RX_N buffers: the USB session thread fills
 * a free one (net_rx_get) and queues it (net_rx_frame); the net thread
 * takes it, runs it through the stack with net_lock held and gives it
 * back. Every frame the stack sends is built in the one transmit buffer
 * and sent by the net thread, so a slow PC holds up only that thread.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/fs.h"
#include "os/kernel.h"
#include "os/lib.h"
#include "os/rtc.h"
#include "os/ui.h"
#include "net/net_internal.h"

#define NET_RX_N        8
#define ARP_N           4
#define TX_BUF          (NET_HEADROOM + NET_FRAME_MAX + 4)

#define ETH_IP          0x0800
#define ETH_ARP         0x0806

volatile net_stat_t net_stat;
const uint8_t net_mac[6] = { 0x02, 0xb3, 0x10, 0xe0, 0x00, 0x01 };
const uint8_t net_host_mac[6] = { 0x02, 0xb3, 0x10, 0xe0, 0x00, 0x02 };
static const uint8_t bcast_mac[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

mutex_t net_lock;
volatile bool net_stopping;

typedef struct {
    uint8_t *buf;
    int off, len;
} rx_item_t;

static uint8_t *rx_pool[NET_RX_N];
static queue_t rx_free, rx_full;
static uint8_t *tx;                     /* NET_HEADROOM, then the frame */
static sem_t wake, net_exited;
static thread_t *net_thread;
static int (*link_send)(uint8_t *buf, int len);
static bool started;

static struct {
    uint32_t ip;
    uint8_t mac[6];
} arp[ARP_N];
static unsigned arp_next;

/* ---- checksums and CRC ----------------------------------------------------- */

uint32_t net_sum(uint32_t sum, const uint8_t *p, int n)
{
    for (; n > 1; n -= 2, p += 2)
        sum += (uint32_t)(p[0] << 8 | p[1]);
    if (n > 0)
        sum += (uint32_t)p[0] << 8;
    return sum;
}

uint16_t net_fold(uint32_t sum)
{
    while (sum >> 16)
        sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)sum;
}

uint32_t net_pseudo(uint32_t src, uint32_t dst, uint8_t proto, uint16_t len)
{
    return (src >> 16) + (src & 0xffff) + (dst >> 16) + (dst & 0xffff) + proto + len;
}

uint32_t net_crc32(uint32_t crc, const void *data, size_t n)
{
    static const uint32_t t[16] = {
        0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac, 0x76dc4190, 0x6b6b51f4,
        0x4db26158, 0x5005713c, 0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c,
        0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c,
    };
    const uint8_t *p = data;

    while (n--) {
        crc ^= *p++;
        crc = (crc >> 4) ^ t[crc & 15];
        crc = (crc >> 4) ^ t[crc & 15];
    }
    return crc;
}

/* ---- the link ----------------------------------------------------------- */

void net_link(bool up)
{
    net_stat.link = up;
}

uint8_t *net_rx_get(uint32_t timeout_ms)
{
    uint8_t *b;

    if (!started || net_stopping)
        return NULL;
    if (queue_recv(&rx_free, &b, timeout_ms) != WAIT_OK)
        return NULL;
    return b;
}

void net_rx_free(uint8_t *buf)
{
    if (buf)
        queue_send(&rx_free, &buf, 0);
}

void net_rx_frame(uint8_t *buf, int off, int len)
{
    rx_item_t it = { buf, off, len };

    net_stat.rx_frames++;
    net_stat.rx_bytes += (uint32_t)len;
    if (len >= 14) {                    /* by EtherType, for the log */
        unsigned t = (unsigned)buf[off + 12] << 8 | buf[off + 13];

        if (t == ETH_ARP)
            net_stat.rx_arp++;
        else if (t == ETH_IP)
            net_stat.rx_ipv4++;
        else if (t == 0x86dd)
            net_stat.rx_ipv6++;
        else
            net_stat.rx_other++;
    }
    if (queue_send(&rx_full, &it, 0) != WAIT_OK) {
        net_stat.rx_dropped++;
        net_rx_free(buf);
        return;
    }
    sem_post(&wake);
}

void net_wake(void)
{
    sem_post(&wake);
}

static int eth_send(const uint8_t *dst, uint16_t type, int len)
{
    uint8_t *f = tx + NET_HEADROOM;

    if (!net_stat.link || !link_send) {
        net_stat.tx_dropped++;
        if (net_stat.tx_no_link++ < 3)
            klog("net: a frame (type %04x) not sent: %s", type,
                 !link_send ? "no link driver" : "the host has not set a packet filter");
        return -1;
    }
    memcpy(f, dst, 6);
    memcpy(f + 6, net_mac, 6);
    put16(f + 12, type);
    len += ETH_HLEN;
    if (len < 60) {                     /* the Ethernet minimum */
        memset(f + len, 0, (size_t)(60 - len));
        len = 60;
    }
    if (link_send(tx, len) < 0) {
        net_stat.tx_dropped++;
        net_stat.tx_fail++;
        return -1;
    }
    net_stat.tx_frames++;
    net_stat.tx_bytes += (uint32_t)len;
    return 0;
}

/* ---- ARP ---------------------------------------------------------------- */

uint32_t net_self = NET_IP;

static bool is_self(uint32_t ip)
{
    return ip == NET_IP || ip == NET_LL_IP;
}

static uint32_t self_mask(void)
{
    return net_self == NET_LL_IP ? NET_LL_MASK : NET_MASK;
}

/* the PC used this one of our addresses: answer from it from now on */
static void adopt(uint32_t ip)
{
    if (is_self(ip) && ip != net_self) {
        net_self = ip;
        klog("net: the PC uses %lu.%lu.%lu.%lu", (unsigned long)(ip >> 24),
             (unsigned long)(ip >> 16 & 255), (unsigned long)(ip >> 8 & 255),
             (unsigned long)(ip & 255));
    }
}

static void arp_learn(uint32_t ip, const uint8_t *mac)
{
    if (!ip || (mac[0] & 1) ||
        ((ip & NET_MASK) != (NET_IP & NET_MASK) && (ip & NET_LL_MASK) != (NET_LL_IP & NET_LL_MASK)))
        return;
    for (unsigned i = 0; i < ARP_N; i++)
        if (arp[i].ip == ip) {
            memcpy(arp[i].mac, mac, 6);
            return;
        }
    arp[arp_next].ip = ip;
    memcpy(arp[arp_next].mac, mac, 6);
    arp_next = (arp_next + 1) % ARP_N;
}

static const uint8_t *arp_find(uint32_t ip)
{
    for (unsigned i = 0; i < ARP_N; i++)
        if (arp[i].ip == ip)
            return arp[i].mac;
    return NULL;
}

static void arp_send(uint16_t op, const uint8_t *tha, uint32_t tip)
{
    uint8_t *a = tx + NET_HEADROOM + ETH_HLEN;

    put16(a, 1);                        /* Ethernet */
    put16(a + 2, ETH_IP);
    a[4] = 6;
    a[5] = 4;
    put16(a + 6, op);
    memcpy(a + 8, net_mac, 6);
    put32(a + 14, net_self);
    memcpy(a + 18, op == 1 ? (const uint8_t *)"\0\0\0\0\0\0" : tha, 6);
    put32(a + 24, tip);
    eth_send(op == 1 ? bcast_mac : tha, ETH_ARP, 28);
}

static void arp_input(const uint8_t *a, int len)
{
    uint32_t sip, tip;

    if (len < 28 || get16(a) != 1 || get16(a + 2) != ETH_IP || a[4] != 6 || a[5] != 4)
        return;
    sip = get32(a + 14);
    tip = get32(a + 24);
    if (!is_self(tip))
        return;                         /* a probe for the PC's own address, etc. */
    adopt(tip);
    arp_learn(sip, a + 8);
    if (get16(a + 6) == 1 && sip)
        arp_send(2, a + 8, sip);
}

/* ---- IPv4 --------------------------------------------------------------- */

uint8_t *ip_payload(void)
{
    return tx + NET_HEADROOM + ETH_HLEN + IP_HLEN;
}

int ip_send(uint32_t dst, uint8_t proto, int len)
{
    static uint16_t id;
    uint8_t *h = tx + NET_HEADROOM + ETH_HLEN;
    const uint8_t *mac;

    if (dst == 0xffffffffu || dst == (net_self | ~self_mask())) {
        mac = bcast_mac;
    } else if (!(mac = arp_find(dst))) {
        arp_send(1, NULL, dst);         /* ask; the caller's frame is lost */
        net_stat.tx_dropped++;
        return -1;
    }
    h[0] = 0x45;
    h[1] = 0;
    put16(h + 2, (uint16_t)(IP_HLEN + len));
    put16(h + 4, id++);
    put16(h + 6, 0x4000);               /* don't fragment */
    h[8] = 64;
    h[9] = proto;
    put16(h + 10, 0);
    put32(h + 12, net_self);
    put32(h + 16, dst);
    put16(h + 10, (uint16_t)~net_fold(net_sum(0, h, IP_HLEN)));
    return eth_send(mac, ETH_IP, IP_HLEN + len);
}

static void icmp_input(uint32_t src, const uint8_t *p, int len)
{
    uint8_t *o = ip_payload();

    if (len < 8 || p[0] != 8 || net_fold(net_sum(0, p, len)) != 0xffff)
        return;
    if (len > NET_MTU - IP_HLEN)
        return;
    memcpy(o, p, (size_t)len);
    o[0] = 0;                           /* echo reply */
    put16(o + 2, 0);
    put16(o + 2, (uint16_t)~net_fold(net_sum(0, o, len)));
    if (ip_send(src, IP_PROTO_ICMP, len) == 0)
        net_stat.pings++;
}

/* ---- UDP and the DHCP server -------------------------------------------- */

static int udp_send(uint32_t dst, uint16_t sport, uint16_t dport, int len)
{
    uint8_t *u = ip_payload();
    uint16_t sum;

    put16(u, sport);
    put16(u + 2, dport);
    put16(u + 4, (uint16_t)(UDP_HLEN + len));
    put16(u + 6, 0);
    sum = (uint16_t)~net_fold(net_sum(net_pseudo(net_self, dst, IP_PROTO_UDP,
                                                 (uint16_t)(UDP_HLEN + len)),
                                      u, UDP_HLEN + len));
    put16(u + 6, sum ? sum : 0xffff);
    return ip_send(dst, IP_PROTO_UDP, UDP_HLEN + len);
}

#define DHCP_LEASE_S    86400u

static uint8_t *dhcp_opt(uint8_t *o, uint8_t code, uint8_t len, uint32_t v)
{
    *o++ = code;
    *o++ = len;
    if (len == 1)
        *o++ = (uint8_t)v;
    else {
        put32(o, v);
        o += 4;
    }
    return o;
}

static void dhcp_reply(const uint8_t *req, uint8_t type, uint32_t yiaddr)
{
    uint8_t *d = ip_payload() + UDP_HLEN, *o;
    int len;

    memset(d, 0, 300);
    d[0] = 2;                           /* BOOTREPLY */
    d[1] = 1;
    d[2] = 6;
    memcpy(d + 4, req + 4, 4);          /* xid */
    memcpy(d + 10, req + 10, 2);        /* flags */
    memcpy(d + 12, req + 12, 4);        /* ciaddr */
    put32(d + 16, yiaddr);
    put32(d + 20, type == 6 ? 0 : NET_IP);      /* siaddr */
    memcpy(d + 28, req + 28, 16);       /* chaddr */
    put32(d + 236, 0x63825363u);        /* magic cookie */
    o = d + 240;
    o = dhcp_opt(o, 53, 1, type);
    o = dhcp_opt(o, 54, 4, NET_IP);     /* server */
    if (type != 6) {                    /* not a NAK */
        if (type != 5 || yiaddr) {
            o = dhcp_opt(o, 51, 4, DHCP_LEASE_S);
            o = dhcp_opt(o, 58, 4, DHCP_LEASE_S / 2);
            o = dhcp_opt(o, 59, 4, DHCP_LEASE_S / 8 * 7);
        }
        o = dhcp_opt(o, 1, 4, NET_MASK);
        o = dhcp_opt(o, 28, 4, NET_IP | ~NET_MASK);
        /* no router and no DNS: the PC keeps its own internet */
    }
    *o++ = 255;
    len = (int)(o - d);
    if (len < 300)                      /* the BOOTP minimum */
        len = 300;
    udp_send(0xffffffffu, 67, 68, len);
}

static void dhcp_input(const uint8_t *d, int len)
{
    const uint8_t *o, *end = d + len;
    uint8_t type = 0;
    uint32_t want = 0, server = 0, ciaddr;

    if (len < 240 || d[0] != 1 || d[1] != 1 || d[2] != 6 || get32(d + 236) != 0x63825363u)
        return;
    for (o = d + 240; o < end && *o != 255; ) {
        uint8_t code = *o++, n;

        if (code == 0)
            continue;
        if (o >= end)
            break;
        n = *o++;
        if (o + n > end)
            break;
        if (code == 53 && n == 1)
            type = o[0];
        else if (code == 50 && n == 4)
            want = get32(o);
        else if (code == 54 && n == 4)
            server = get32(o);
        o += n;
    }
    ciaddr = get32(d + 12);
    switch (type) {
    case 1:                             /* DISCOVER */
        dhcp_reply(d, 2, NET_HOST_IP);  /* OFFER */
        break;
    case 3:                             /* REQUEST */
        if (server && server != NET_IP)
            break;                      /* the PC took another server's offer */
        if (!want)
            want = ciaddr;
        if (want == NET_HOST_IP) {
            dhcp_reply(d, 5, NET_HOST_IP);      /* ACK */
            net_stat.dhcp_acks++;
            klog("net: DHCP lease %lu.%lu.%lu.%lu to %02x:%02x:%02x:%02x:%02x:%02x",
                 (unsigned long)(want >> 24), (unsigned long)(want >> 16 & 255),
                 (unsigned long)(want >> 8 & 255), (unsigned long)(want & 255),
                 d[28], d[29], d[30], d[31], d[32], d[33]);
        } else {
            dhcp_reply(d, 6, 0);        /* NAK: start over with DISCOVER */
        }
        break;
    case 8:                             /* INFORM */
        dhcp_reply(d, 5, 0);
        break;
    }
}

/* ---- SNTP: the clock from the PC (RFC 4330) ---------------------------------
 * Over USB the phone reaches only the PC, so the PC is the server: the
 * Windows Time service with its NTP server on, or ntpd/chrony; without
 * one, os/tools/b310e-shell.py time sets the clock through the shell. */

#define NTP_PORT        123
#define NTP_RETRY_MS    10000u          /* until the first answer; then */
#define NTP_EVERY_MS    3600000u        /* hourly */
#define NTP_TRIES       6               /* then every NTP_EVERY_MS anyway */
#define NTP_1970        2208988800u     /* NTP seconds at the Unix epoch */

static uint32_t pc_ip;                  /* the PC: the source of its frames */
static uint32_t ntp_next, ntp_asked;
static int ntp_tries;
static volatile uint32_t ntp_got;       /* Unix seconds to set, 0 none */
static uint32_t ntp_from;

static void ntp_ask(uint32_t now)
{
    uint8_t *d = ip_payload() + UDP_HLEN;

    memset(d, 0, 48);
    d[0] = 0x23;                        /* no leap warning, version 4, client */
    ntp_asked = now;
    if (ntp_tries < NTP_TRIES && ntp_tries++ < 2)
        klog("net: time asked of %lu.%lu.%lu.%lu", (unsigned long)(pc_ip >> 24),
             (unsigned long)(pc_ip >> 16 & 255), (unsigned long)(pc_ip >> 8 & 255),
             (unsigned long)(pc_ip & 255));
    udp_send(pc_ip, NTP_PORT, NTP_PORT, 48);
}

/* the net thread: when to ask; returns ms to the next question */
static uint32_t ntp_poll(uint32_t now)
{
    if (!pc_ip || !ui_settings.net_time)
        return WAIT_FOREVER;
    if ((int32_t)(now - ntp_next) >= 0) {
        ntp_ask(now);
        ntp_next = now + (ntp_tries < NTP_TRIES ? NTP_RETRY_MS : NTP_EVERY_MS);
    }
    return ntp_next - now;
}

static void ntp_input(uint32_t src, const uint8_t *d, int len)
{
    uint32_t tx;

    if (len < 48 || (d[0] & 7) != 4 || !d[1] || d[1] > 15)
        return;                         /* not a server's answer, or unsynchronised */
    tx = get32(d + 40);
    if (tx < NTP_1970 + 1700000000u)
        return;                         /* before 2023: not a real clock */
    ntp_got = tx - NTP_1970 + ((d[44] & 0x80) ? 1 : 0);    /* rounded */
    ntp_from = src;
    ntp_tries = NTP_TRIES;
    ntp_next = kernel_ticks() + NTP_EVERY_MS;
}

static void udp_input(uint32_t src, uint32_t dst, const uint8_t *u, int len)
{
    int ulen;

    static unsigned drops;

    if (len < UDP_HLEN)
        return;
    ulen = get16(u + 4);
    if (ulen < UDP_HLEN || ulen > len) {
        if (drops++ < 6)
            klog("net: udp dropped: length %d of %d", ulen, len);
        return;
    }
    if (get16(u + 6) && net_fold(net_sum(net_pseudo(src, dst, IP_PROTO_UDP, (uint16_t)ulen),
                                         u, ulen)) != 0xffff) {
        if (drops++ < 6)
            klog("net: udp dropped: checksum (port %u -> %u, %d bytes, sum %04x)",
                 get16(u), get16(u + 2), ulen,
                 net_fold(net_sum(net_pseudo(src, dst, IP_PROTO_UDP, (uint16_t)ulen), u, ulen)));
        return;
    }
    if (get16(u + 2) == 67 && get16(u) == 68) {
        if (net_stat.rx_dhcp++ < 4)
            klog("net: DHCP message in, %d bytes", ulen - UDP_HLEN);
        dhcp_input(u + UDP_HLEN, ulen - UDP_HLEN);
    } else if (get16(u) == NTP_PORT && get16(u + 2) == NTP_PORT) {
        ntp_input(src, u + UDP_HLEN, ulen - UDP_HLEN);
    }
}

static void ip_input(const uint8_t *eth, uint8_t *h, int len)
{
    int hl, tl;
    uint32_t src, dst;

    static unsigned drops;

    if (len < IP_HLEN || h[0] >> 4 != 4) {
        if (drops++ < 6)
            klog("net: ip dropped: %d bytes, version %u", len, h[0] >> 4);
        return;
    }
    hl = (h[0] & 15) * 4;
    tl = get16(h + 2);
    if (hl < IP_HLEN || tl < hl || tl > len || net_fold(net_sum(0, h, hl)) != 0xffff) {
        if (drops++ < 6)
            klog("net: ip dropped: hl %d, total %d, frame %d, sum %04x", hl, tl, len,
                 net_fold(net_sum(0, h, hl < IP_HLEN ? IP_HLEN : hl)));
        return;
    }
    if (get16(h + 6) & 0x3fff) {        /* a fragment */
        if (drops++ < 6)
            klog("net: ip dropped: a fragment");
        return;
    }
    src = get32(h + 12);
    dst = get32(h + 16);
    if (!pc_ip && !is_self(src) &&
        ((src & NET_MASK) == (NET_IP & NET_MASK) || (src & NET_LL_MASK) == (NET_LL_IP & NET_LL_MASK))) {
        pc_ip = src;                    /* a time question soon */
        ntp_next = kernel_ticks() + 500;
    }
    arp_learn(src, eth + 6);
    if (is_self(dst)) {
        adopt(dst);
        if (h[9] == IP_PROTO_ICMP)
            icmp_input(src, h + hl, tl - hl);
        else if (h[9] == IP_PROTO_TCP)
            tcp_input(src, h + hl, tl - hl);
        else if (h[9] == IP_PROTO_UDP)
            udp_input(src, dst, h + hl, tl - hl);
    } else if ((dst == 0xffffffffu || dst == (NET_IP | ~NET_MASK) ||
                dst == (NET_LL_IP | ~NET_LL_MASK)) && h[9] == IP_PROTO_UDP) {
        udp_input(src, dst, h + hl, tl - hl);
    }
}

static void eth_input(uint8_t *f, int len)
{
    uint16_t type;

    if (len < ETH_HLEN)
        return;
    if (!(f[0] & 1) && memcmp(f, net_mac, 6))
        return;                         /* someone else's */
    type = get16(f + 12);
    if (type == ETH_ARP)
        arp_input(f + ETH_HLEN, len - ETH_HLEN);
    else if (type == ETH_IP)
        ip_input(f, f + ETH_HLEN, len - ETH_HLEN);
}

/* ---- the net thread ----------------------------------------------------- */

static void net_main(void *arg)
{
    uint32_t wait = WAIT_FOREVER;

    while (!net_stopping) {
        rx_item_t it;

        sem_wait(&wake, wait);
        if (net_stopping)
            break;
        mutex_lock(&net_lock, WAIT_FOREVER);
        while (queue_recv(&rx_full, &it, 0) == WAIT_OK) {
            eth_input(it.buf + it.off, it.len);
            net_rx_free(it.buf);
        }
        wait = tcp_poll(kernel_ticks());
        {
            uint32_t w = ntp_poll(kernel_ticks());

            if (w < wait)
                wait = w;
        }
        mutex_unlock(&net_lock);
        if (ntp_got) {                  /* the RTC waits for its acknowledgement:
                                         * outside the stack's lock */
            uint32_t unix_s = ntp_got, was = rtc_unix(ui_settings.tz_min);

            ntp_got = 0;
            if (rtc_set_unix(unix_s, ui_settings.tz_min) == 0) {
                klog("net: clock set from %lu.%lu.%lu.%lu (it was %ld s off)",
                     (unsigned long)(ntp_from >> 24), (unsigned long)(ntp_from >> 16 & 255),
                     (unsigned long)(ntp_from >> 8 & 255), (unsigned long)(ntp_from & 255),
                     (long)((int32_t)(was - unix_s)));
                rtc_save();
            }
        }
    }
    sem_post(&net_exited);
    thread_exit();
}

static void free_all(void)
{
    for (int i = 0; i < NET_RX_N; i++) {
        kfree(rx_pool[i]);
        rx_pool[i] = NULL;
    }
    kfree(tx);
    tx = NULL;
    kfree(rx_free.buf);
    kfree(rx_full.buf);
    rx_free.buf = rx_full.buf = NULL;
}

int net_start(int (*send)(uint8_t *buf, int len))
{
    if (started)
        return 0;
    memset((void *)&net_stat, 0, sizeof(net_stat));
    memset(arp, 0, sizeof(arp));
    net_self = NET_IP;
    pc_ip = ntp_got = 0;
    ntp_tries = 0;
    net_stopping = false;
    link_send = send;
    mutex_init(&net_lock);
    sem_init(&wake, 0);
    sem_init(&net_exited, 0);
    if (queue_init(&rx_free, sizeof(uint8_t *), NET_RX_N) ||
        queue_init(&rx_full, sizeof(rx_item_t), NET_RX_N))
        goto fail;
    tx = kmalloc(TX_BUF);
    if (!tx)
        goto fail;
    for (int i = 0; i < NET_RX_N; i++) {
        rx_pool[i] = kmalloc(NET_RX_BUF);
        if (!rx_pool[i])
            goto fail;
        queue_send(&rx_free, &rx_pool[i], 0);
    }
    tcp_init();
    started = true;
    net_thread = thread_create("net", PRIO_NORMAL + 2, 3072, net_main, NULL);
    if (!net_thread) {
        started = false;
        goto fail;
    }
    if (shell_start() < 0) {
        net_stop();
        return -1;
    }
    net_stat.up = 1;
    klog("net: up, 192.168.7.1, shell on ports %d and %d", NET_PORT_TELNET, NET_PORT_RAW);
    return 0;
fail:
    free_all();
    return -1;
}

/* Every thread of the stack is gone when this returns (or, after their
 * timeouts, logged as still running and their memory kept). */
void net_stop(void)
{
    bool stuck = false;

    if (!started)
        return;
    net_stopping = true;
    mutex_lock(&net_lock, WAIT_FOREVER);
    tcp_shutdown();
    mutex_unlock(&net_lock);
    if (!shell_stop())
        stuck = true;
    sem_post(&wake);
    if (sem_wait(&net_exited, 3000) != WAIT_OK)
        stuck = true;
    started = false;
    net_stat.up = 0;
    net_stat.link = 0;
    if (stuck) {
        klog("net: a thread did not stop; the stack's memory is kept");
        return;
    }
    tcp_free_all();
    free_all();
    klog("net: down: %lu frames in, %lu out, %lu dropped, %lu shell sessions",
         (unsigned long)net_stat.rx_frames, (unsigned long)net_stat.tx_frames,
         (unsigned long)net_stat.rx_dropped, (unsigned long)net_stat.shell_sessions);
    klog("net: in: %lu ARP, %lu IPv4, %lu IPv6, %lu other, %lu DHCP; out refused %lu "
         "(no filter), failed %lu (USB), timeouts %lu; filter %08lx",
         (unsigned long)net_stat.rx_arp, (unsigned long)net_stat.rx_ipv4,
         (unsigned long)net_stat.rx_ipv6, (unsigned long)net_stat.rx_other,
         (unsigned long)net_stat.rx_dhcp, (unsigned long)net_stat.tx_no_link,
         (unsigned long)net_stat.tx_fail, (unsigned long)net_stat.tx_timeouts,
         (unsigned long)net_stat.packet_filter);
}
