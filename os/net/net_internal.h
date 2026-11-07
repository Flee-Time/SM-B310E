/* B310E OS — inside the network stack: between ip.c, tcp.c and shell.c.
 *
 * Everything below with "locked" runs with net_lock held, in the net
 * thread (which also sends every frame), unless said otherwise.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef OS_NET_INTERNAL_H
#define OS_NET_INTERNAL_H

#include "os/kernel.h"
#include "os/net.h"

#define ETH_HLEN   14
#define IP_HLEN    20
#define TCP_HLEN   20
#define UDP_HLEN   8
#define IP_PROTO_ICMP 1
#define IP_PROTO_TCP  6
#define IP_PROTO_UDP  17

extern mutex_t net_lock;
extern volatile bool net_stopping;

void net_wake(void);                    /* the net thread: something to send */

/* big-endian fields */
static inline uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static inline uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static inline void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static inline void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

/* one's complement sum, folded but not inverted */
uint32_t net_sum(uint32_t sum, const uint8_t *p, int n);
uint16_t net_fold(uint32_t sum);
/* the TCP/UDP pseudo header's sum */
uint32_t net_pseudo(uint32_t src, uint32_t dst, uint8_t proto, uint16_t len);

/* locked: the IP payload area of the frame being built, and sending it */
uint8_t *ip_payload(void);
int ip_send(uint32_t dst, uint8_t proto, int len);

/* tcp.c, locked */
void tcp_init(void);
void tcp_input(uint32_t src, uint8_t *seg, int len);
uint32_t tcp_poll(uint32_t now);        /* sends what is due; ms to the next timer */
void tcp_shutdown(void);                /* the stack stops: wake every waiter */
void tcp_free_all(void);                /* after the threads are gone */

/* shell.c, from net_start() / net_stop() (not locked) */
int shell_start(void);
bool shell_stop(void);

#endif
