/* B310E OS — the network over USB: a small IPv4 stack of our own and the
 * development shell on it.
 *
 * The link is the RNDIS adapter (drivers/usb_rndis.c): Ethernet frames
 * to and from one PC. On it (net/ip.c, net/tcp.c):
 *   ARP          answers for the phone, learns the PC
 *   IPv4         no fragments, no options, no routing (one subnet)
 *   ICMP         echo (ping)
 *   UDP          port handlers; the DHCP server on 67
 *   DHCP server  one lease: the PC gets NET_HOST_IP, no router, no DNS
 *   TCP          passive opens only (listeners), a few connections,
 *                go-back-N retransmission, in-order receive
 * The shell (net/shell.c) listens on NET_PORT_TELNET (telnet clients:
 * echo and line editing on the phone) and NET_PORT_RAW (nc, scripts,
 * file transfer: no echo, no telnet codes).
 *
 * Threads: the USB session thread receives frames into the stack's
 * buffers; the "net" thread runs the stack and sends every frame; "netsh"
 * accepts, and each connection gets a shell thread. The stack is guarded
 * by one mutex; nothing in it blocks on the PC.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef OS_NET_H
#define OS_NET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NET_IP           0xc0a80701u    /* 192.168.7.1, the phone */
#define NET_HOST_IP      0xc0a80702u    /* 192.168.7.2, the PC (DHCP) */
#define NET_MASK         0xffffff00u
/* The phone also answers at 169.254.7.1: a PC that gave itself a
 * link-local address (Windows does, when it has not asked our DHCP server
 * in time, and keeps it) reaches the phone there without DHCP. */
#define NET_LL_IP        0xa9fe0701u    /* 169.254.7.1 */
#define NET_LL_MASK      0xffff0000u
extern uint32_t net_self;               /* the address the PC uses now */
#define NET_MTU          1500
#define NET_FRAME_MAX    1514           /* Ethernet, no FCS */
#define NET_HEADROOM     44             /* in front of a sent frame: RNDIS */
#define NET_RX_BUF       1600           /* one received transfer */
#define NET_PORT_TELNET  23
#define NET_PORT_RAW     2323

#define NET_EAGAIN       (-2)           /* timed out */
#define NET_ERESET       (-3)           /* the connection is gone */
#define NET_ECLOSED      (-4)           /* the stack stopped */

/* Counters for the UI and the tests (QEMU reads the `net_stat` symbol).
 * Field order is the tests' contract: append only. */
typedef struct {
    uint32_t up;                /* the stack runs */
    uint32_t link;              /* the host enabled the data path (RNDIS filter) */
    uint32_t rx_frames, tx_frames;
    uint32_t rx_bytes, tx_bytes;
    uint32_t rx_dropped;        /* no buffer, bad frame, not for us */
    uint32_t tx_dropped;        /* link down, no ARP entry */
    uint32_t tx_timeouts;       /* the host did not take a frame */
    uint32_t rndis_msgs;        /* control messages */
    uint32_t rndis_inits;
    uint32_t dhcp_acks;         /* leases handed out */
    uint32_t pings;             /* echo requests answered */
    uint32_t tcp_accepted;
    uint32_t tcp_active;        /* connections now */
    uint32_t tcp_retransmits;
    uint32_t tcp_resets;        /* RSTs sent */
    uint32_t shell_sessions;
    uint32_t shell_commands;
    uint32_t files_in;          /* uploads finished */
    uint32_t files_out;
    uint32_t apps_run;
    uint32_t shell_open;        /* shell sessions now */
    /* what came in, by type (for finding why nothing goes out) */
    uint32_t rx_arp, rx_ipv4, rx_ipv6, rx_other, rx_dhcp;
    uint32_t tx_no_link;        /* sends refused: the host never set a filter */
    uint32_t tx_fail;           /* sends the USB side did not complete */
    uint32_t packet_filter;     /* as the host set it */
} net_stat_t;

extern volatile net_stat_t net_stat;

/* MAC addresses: the phone's, and the one RNDIS gives the PC's adapter
 * (locally administered). */
extern const uint8_t net_mac[6], net_host_mac[6];

/* ---- the link side (drivers/usb_rndis.c) -------------------------------- */
/* Start the stack and the shell with this frame sender (frame at buf +
 * NET_HEADROOM, len bytes; the headroom is the sender's); 0, or -1
 * without memory. net_stop() ends every connection and thread. */
int net_start(int (*send)(uint8_t *buf, int len));
void net_stop(void);
void net_link(bool up);                 /* the host may take frames */
/* A free receive buffer of NET_RX_BUF bytes, NULL after timeout_ms or
 * when stopping; hand it back filled (net_rx_frame, the frame at off) or
 * unused (net_rx_free). */
uint8_t *net_rx_get(uint32_t timeout_ms);
void net_rx_frame(uint8_t *buf, int off, int len);
void net_rx_free(uint8_t *buf);

/* ---- TCP for the shell (net/tcp.c) ------------------------------------- */
typedef struct tcp_conn tcp_conn_t;

int tcp_listen(uint16_t port);          /* 0, or -1 */
/* The next connection established on any listener; NULL after timeout_ms
 * or when stopping. */
tcp_conn_t *tcp_accept(uint32_t timeout_ms);
/* > 0 bytes read, 0 at the PC's end of stream, NET_EAGAIN, NET_ERESET or
 * NET_ECLOSED */
int tcp_read(tcp_conn_t *c, void *buf, int n, uint32_t timeout_ms);
/* all n bytes queued, or NET_ERESET / NET_ECLOSED */
int tcp_write(tcp_conn_t *c, const void *buf, int n);
/* Wait until everything written has been acknowledged (or ms passed). */
void tcp_flush(tcp_conn_t *c, uint32_t ms);
void tcp_close(tcp_conn_t *c);          /* FIN after the data; c is gone */
uint16_t tcp_port(const tcp_conn_t *c); /* the local port */
uint32_t tcp_peer(const tcp_conn_t *c); /* the PC's address */

/* ---- inside the stack (net/) ------------------------------------------- */
uint32_t net_crc32(uint32_t crc, const void *data, size_t n);  /* IEEE, ~ at the ends */

#endif
