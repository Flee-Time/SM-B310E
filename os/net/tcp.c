/* B310E OS — TCP, enough for a shell and file transfer over one USB link.
 *
 * Passive opens only: up to TCP_LISTEN_N listening ports, TCP_CONN_N
 * connections. Each connection has an 8 KB receive ring (the advertised
 * window; no window scaling) and an 8 KB send ring holding everything
 * from the oldest unacknowledged byte. Sending: as much as the PC's
 * window allows, in MSS segments (1460, or what the PC announced), each
 * with PSH; no Nagle, no delayed ACKs (every segment that brings data is
 * answered at once), no congestion control: the link is a cable. Loss
 * (a full receive buffer on either side) is handled go-back-N: when the
 * retransmission timer runs out everything unacknowledged is sent again,
 * with the timeout (500 ms at first) doubled up to RTO_MAX; after
 * RTX_MAX tries the connection is reset. An ACK for data sent before the
 * timeout still counts (snd_max). Segments that arrive out of order are
 * dropped and answered with the ACK we expect. A zero window is probed
 * with one byte per timeout. TIME_WAIT lasts TIME_WAIT_MS; FIN_WAIT_2
 * (our side closed, the PC's still open) at most FIN_WAIT_MS.
 *
 * Only the net thread runs the protocol (tcp_input, tcp_poll, with
 * net_lock); the shell threads move bytes in and out of the rings
 * (tcp_read, tcp_write) under the same lock and wake the net thread.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/kernel.h"
#include "os/lib.h"
#include "net/net_internal.h"

#define TCP_CONN_N    4
#define TCP_LISTEN_N  4
#define RX_SIZE       8192
#define TX_SIZE       8192
#define MSS_MAX       (NET_MTU - IP_HLEN - TCP_HLEN)
#define RTO_INIT      500               /* above Windows' 200 ms delayed ACK */
#define RTO_MAX       4000
#define RTX_MAX       10
#define TIME_WAIT_MS  2000
#define FIN_WAIT_MS   15000             /* FIN_WAIT_2 with the PC's side open */

#define F_FIN 0x01
#define F_SYN 0x02
#define F_RST 0x04
#define F_PSH 0x08
#define F_ACK 0x10

enum { S_FREE, S_SYN_RCVD, S_ESTABLISHED, S_FIN_WAIT_1, S_FIN_WAIT_2,
       S_CLOSING, S_TIME_WAIT, S_CLOSE_WAIT, S_LAST_ACK, S_CLOSED };

struct tcp_conn {
    uint8_t state;
    bool owned;                 /* accepted: the shell holds it */
    bool queued;                /* established, waiting for tcp_accept */
    bool app_closed;            /* tcp_close() done: free when finished */
    bool reset;                 /* reset by either side */
    bool fin_rcvd, fin_queued, fin_sent, fin_ever;
    bool ack_now, probe;
    uint16_t lport, rport, mss;
    uint32_t rip;
    uint32_t iss, snd_una, snd_wnd;
    uint32_t snd_max;           /* the highest sequence number sent, plus one */
    uint32_t tx_sent;           /* bytes of the ring sent since snd_una (the
                                 * resend point after a timeout) */
    uint32_t rcv_nxt, rcv_adv;  /* rcv_adv: the right edge last announced */
    uint8_t *rx, *tx;
    uint32_t rx_start, rx_len, tx_start, tx_len;
    uint32_t rtx_at, rto, tw_until;
    int retries;
    sem_t rx_sem, tx_sem;
};

static tcp_conn_t conns[TCP_CONN_N];
static uint16_t listening[TCP_LISTEN_N];
static sem_t accept_sem;
static uint32_t iss_seed;

static bool seq_lt(uint32_t a, uint32_t b) { return (int32_t)(a - b) < 0; }
static bool seq_le(uint32_t a, uint32_t b) { return (int32_t)(a - b) <= 0; }

void tcp_init(void)
{
    memset(conns, 0, sizeof(conns));
    memset(listening, 0, sizeof(listening));
    sem_init(&accept_sem, 0);
    iss_seed = kernel_ticks() * 2654435761u;
}

int tcp_listen(uint16_t port)
{
    int r = -1;

    mutex_lock(&net_lock, WAIT_FOREVER);
    for (int i = 0; i < TCP_LISTEN_N && r; i++)
        if (!listening[i]) {
            listening[i] = port;
            r = 0;
        }
    mutex_unlock(&net_lock);
    return r;
}

static bool is_listening(uint16_t port)
{
    for (int i = 0; i < TCP_LISTEN_N; i++)
        if (listening[i] == port)
            return true;
    return false;
}

static void conn_free(tcp_conn_t *c)
{
    kfree(c->rx);
    kfree(c->tx);
    memset(c, 0, sizeof(*c));
    net_stat.tcp_active--;
}

/* gone for the protocol: free it unless the shell still holds it */
static void conn_closed(tcp_conn_t *c)
{
    c->state = S_CLOSED;
    c->rtx_at = 0;
    sem_post(&c->rx_sem);
    sem_post(&c->tx_sem);
    if (!c->owned && !c->queued)
        conn_free(c);
    else if (c->owned && c->app_closed)
        conn_free(c);
}

static uint32_t rx_window(const tcp_conn_t *c)
{
    return RX_SIZE - c->rx_len;
}

/* ---- output ----------------------------------------------------------------- */

static void send_seg(tcp_conn_t *c, uint32_t seq, uint8_t flags, uint32_t off, int len)
{
    uint8_t *t = ip_payload();
    int hl = TCP_HLEN + ((flags & F_SYN) ? 4 : 0);
    uint32_t wnd = rx_window(c);

    put16(t, c->lport);
    put16(t + 2, c->rport);
    put32(t + 4, seq);
    put32(t + 8, c->rcv_nxt);
    t[12] = (uint8_t)(hl / 4) << 4;
    t[13] = flags | F_ACK;
    put16(t + 14, (uint16_t)wnd);
    put16(t + 16, 0);
    put16(t + 18, 0);
    if (flags & F_SYN) {                /* MSS */
        t[20] = 2;
        t[21] = 4;
        put16(t + 22, MSS_MAX);
    }
    if (len) {                          /* from the ring, which may wrap */
        uint32_t at = (c->tx_start + off) % TX_SIZE, first = TX_SIZE - at;

        if (first > (uint32_t)len)
            first = (uint32_t)len;
        memcpy(t + hl, c->tx + at, first);
        memcpy(t + hl + first, c->tx, (size_t)len - first);
    }
    put16(t + 16, (uint16_t)~net_fold(net_sum(net_pseudo(net_self, c->rip, IP_PROTO_TCP,
                                                         (uint16_t)(hl + len)),
                                              t, hl + len)));
    c->rcv_adv = c->rcv_nxt + wnd;
    c->ack_now = false;
    ip_send(c->rip, IP_PROTO_TCP, hl + len);
}

/* a reset for a segment that has no connection */
static void send_rst(uint32_t dst, const uint8_t *in, int seglen)
{
    uint8_t *t = ip_payload();
    uint8_t flags = in[13];

    put16(t, get16(in + 2));
    put16(t + 2, get16(in));
    if (flags & F_ACK) {
        put32(t + 4, get32(in + 8));
        put32(t + 8, 0);
        t[13] = F_RST;
    } else {
        put32(t + 4, 0);
        put32(t + 8, get32(in + 4) + (uint32_t)seglen);
        t[13] = F_RST | F_ACK;
    }
    t[12] = 5 << 4;
    put16(t + 14, 0);
    put16(t + 16, 0);
    put16(t + 18, 0);
    put16(t + 16, (uint16_t)~net_fold(net_sum(net_pseudo(net_self, dst, IP_PROTO_TCP, TCP_HLEN),
                                              t, TCP_HLEN)));
    net_stat.tcp_resets++;
    ip_send(dst, IP_PROTO_TCP, TCP_HLEN);
}

static void arm_rtx(tcp_conn_t *c, uint32_t now)
{
    if (!c->rtx_at)
        c->rtx_at = (now + c->rto) | 1;
}

static void sent_to(tcp_conn_t *c, uint32_t end)
{
    if (seq_lt(c->snd_max, end))
        c->snd_max = end;
}

static void output(tcp_conn_t *c, uint32_t now)
{
    bool sent = false;

    if (c->state == S_SYN_RCVD) {
        if (c->ack_now || !c->rtx_at) {
            send_seg(c, c->iss, F_SYN, 0, 0);
            arm_rtx(c, now);
        }
        return;
    }
    if (c->state == S_FREE || c->state == S_CLOSED || c->state == S_TIME_WAIT) {
        if (c->state == S_TIME_WAIT && c->ack_now)
            send_seg(c, c->snd_una + c->tx_sent, 0, 0, 0);
        return;
    }
    /* data, as far as the PC's window goes (one byte to probe a zero one) */
    while (c->tx_sent < c->tx_len && !c->fin_sent) {
        uint32_t wnd = c->snd_wnd ? c->snd_wnd : c->probe ? 1 : 0;
        uint32_t n = c->tx_len - c->tx_sent;

        if (c->tx_sent >= wnd) {
            if (!c->rtx_at)             /* the persist timer */
                arm_rtx(c, now);
            break;
        }
        if (n > wnd - c->tx_sent)
            n = wnd - c->tx_sent;
        if (n > c->mss)
            n = c->mss;
        send_seg(c, c->snd_una + c->tx_sent, F_PSH, c->tx_sent, (int)n);
        c->tx_sent += n;
        sent_to(c, c->snd_una + c->tx_sent);
        c->probe = false;
        arm_rtx(c, now);
        sent = true;
    }
    if (c->fin_queued && !c->fin_sent && c->tx_sent == c->tx_len) {
        send_seg(c, c->snd_una + c->tx_sent, F_FIN, 0, 0);
        c->fin_sent = c->fin_ever = true;
        sent_to(c, c->snd_una + c->tx_sent + 1);
        if (c->state == S_ESTABLISHED)
            c->state = S_FIN_WAIT_1;
        else if (c->state == S_CLOSE_WAIT)
            c->state = S_LAST_ACK;
        arm_rtx(c, now);
        sent = true;
    }
    /* a window that opened by a segment or more since it was announced */
    if (!sent && !c->ack_now &&
        c->rcv_nxt + rx_window(c) - c->rcv_adv >= (uint32_t)c->mss)
        c->ack_now = true;
    if (!sent && c->ack_now)
        send_seg(c, c->snd_una + c->tx_sent + (c->fin_sent ? 1 : 0), 0, 0, 0);
}

static void retransmit(tcp_conn_t *c, uint32_t now)
{
    c->rtx_at = 0;
    if (c->state == S_SYN_RCVD || seq_lt(c->snd_una, c->snd_max)) {
        if (++c->retries > RTX_MAX) {
            send_seg(c, c->snd_una + c->tx_sent, F_RST, 0, 0);
            net_stat.tcp_resets++;
            c->reset = true;
            conn_closed(c);
            return;
        }
        net_stat.tcp_retransmits++;
        c->rto = c->rto * 2 > RTO_MAX ? RTO_MAX : c->rto * 2;
        c->tx_sent = 0;                 /* go back to the oldest byte */
        c->fin_sent = false;            /* (the state stays: FIN_WAIT_1 etc.) */
        if (!c->snd_wnd && c->tx_len && c->state != S_SYN_RCVD)
            c->probe = true;            /* still a zero window: the probe again */
        if (c->state == S_SYN_RCVD)
            c->ack_now = true;          /* the SYN-ACK again */
    } else if (c->tx_len && !c->snd_wnd) {
        c->probe = true;                /* zero window: one byte */
    }
    output(c, now);
}

uint32_t tcp_poll(uint32_t now)
{
    uint32_t next = WAIT_FOREVER;

    for (int i = 0; i < TCP_CONN_N; i++) {
        tcp_conn_t *c = &conns[i];

        if (c->state == S_FREE)
            continue;
        if ((c->state == S_TIME_WAIT || c->state == S_FIN_WAIT_2) &&
            (int32_t)(now - c->tw_until) >= 0) {
            conn_closed(c);
            continue;
        }
        if (c->state == S_CLOSED) {
            if ((c->owned && c->app_closed) || (!c->owned && !c->queued))
                conn_free(c);
            continue;
        }
        if (c->rtx_at && (int32_t)(now - c->rtx_at) >= 0)
            retransmit(c, now);
        else
            output(c, now);
        if (c->state == S_FREE || c->state == S_CLOSED)
            continue;
        if (c->rtx_at) {
            uint32_t d = (int32_t)(c->rtx_at - now) > 0 ? c->rtx_at - now : 0;

            if (d < next)
                next = d;
        }
        if (c->state == S_TIME_WAIT || c->state == S_FIN_WAIT_2) {
            uint32_t d = (int32_t)(c->tw_until - now) > 0 ? c->tw_until - now : 0;

            if (d < next)
                next = d;
        }
    }
    return next;
}

/* ---- input ------------------------------------------------------------------ */

/* An ACK up to anything ever sent counts, also past the resend point
 * after a timeout (the ACK was late, not lost). */
static void ack_in(tcp_conn_t *c, uint32_t ack, uint32_t now)
{
    uint32_t n, d;
    bool fin_acked;

    if (!seq_lt(c->snd_una, ack) || seq_lt(c->snd_max, ack))
        return;                         /* old, or for data never sent */
    n = ack - c->snd_una;
    /* nothing is written after a FIN, which takes the sequence number
     * after the ring's last byte */
    fin_acked = c->fin_ever && n == c->tx_len + 1;
    d = n < c->tx_len ? n : c->tx_len;
    c->tx_start = (c->tx_start + d) % TX_SIZE;
    c->tx_len -= d;
    c->tx_sent = c->tx_sent > d ? c->tx_sent - d : 0;
    c->snd_una += d;
    c->retries = 0;
    c->rto = RTO_INIT;
    c->rtx_at = 0;
    if (d)
        sem_post(&c->tx_sem);
    if (fin_acked) {
        c->snd_una++;
        c->fin_sent = c->fin_ever = false;
        c->fin_queued = false;
        if (c->state == S_FIN_WAIT_1) {
            c->state = S_FIN_WAIT_2;
            c->tw_until = now + FIN_WAIT_MS;
        } else if (c->state == S_CLOSING) {
            c->state = S_TIME_WAIT;
            c->tw_until = now + TIME_WAIT_MS;
        } else if (c->state == S_LAST_ACK) {
            conn_closed(c);
            return;
        }
    }
    if (seq_lt(c->snd_una, c->snd_max))
        arm_rtx(c, now);
}

static tcp_conn_t *conn_new(uint32_t src, const uint8_t *t, const uint8_t *opt, int optlen)
{
    tcp_conn_t *c = NULL;

    for (int i = 0; i < TCP_CONN_N && !c; i++)
        if (conns[i].state == S_FREE)
            c = &conns[i];
    if (!c)
        return NULL;
    c->rx = kmalloc(RX_SIZE);
    c->tx = kmalloc(TX_SIZE);
    if (!c->rx || !c->tx) {
        kfree(c->rx);
        kfree(c->tx);
        c->rx = c->tx = NULL;
        return NULL;
    }
    net_stat.tcp_active++;
    c->state = S_SYN_RCVD;
    c->rip = src;
    c->rport = get16(t);
    c->lport = get16(t + 2);
    c->rcv_nxt = get32(t + 4) + 1;
    c->iss = iss_seed += 0x10203 + kernel_ticks() * 4099;
    c->snd_una = c->iss;
    c->snd_max = c->iss + 1;
    c->snd_wnd = get16(t + 14);
    c->mss = 536;
    for (int i = 0; i < optlen; ) {
        if (opt[i] == 0)
            break;
        if (opt[i] == 1) {
            i++;
            continue;
        }
        if (i + 1 >= optlen || opt[i + 1] < 2)
            break;
        if (opt[i] == 2 && opt[i + 1] == 4 && i + 3 < optlen)
            c->mss = get16(opt + i + 2);
        i += opt[i + 1];
    }
    if (c->mss > MSS_MAX)
        c->mss = MSS_MAX;
    if (c->mss < 64)
        c->mss = 64;
    c->rto = RTO_INIT;
    sem_init(&c->rx_sem, 0);
    sem_init(&c->tx_sem, 0);
    return c;
}

void tcp_input(uint32_t src, uint8_t *t, int len)
{
    uint32_t now = kernel_ticks(), seq, ack;
    uint16_t sport, dport;
    uint8_t flags;
    int hl, dlen;
    tcp_conn_t *c = NULL;
    const uint8_t *data;

    if (len < TCP_HLEN)
        return;
    if (net_fold(net_sum(net_pseudo(src, net_self, IP_PROTO_TCP, (uint16_t)len), t, len)) != 0xffff)
        return;
    hl = (t[12] >> 4) * 4;
    if (hl < TCP_HLEN || hl > len)
        return;
    sport = get16(t);
    dport = get16(t + 2);
    seq = get32(t + 4);
    ack = get32(t + 8);
    flags = t[13];
    data = t + hl;
    dlen = len - hl;

    for (int i = 0; i < TCP_CONN_N && !c; i++)
        if (conns[i].state != S_FREE && conns[i].state != S_CLOSED &&
            conns[i].rip == src && conns[i].rport == sport && conns[i].lport == dport)
            c = &conns[i];

    if (!c) {
        if (flags & F_RST)
            return;
        if ((flags & (F_SYN | F_ACK)) == F_SYN && is_listening(dport)) {
            c = conn_new(src, t, t + TCP_HLEN, hl - TCP_HLEN);
            if (c) {
                c->ack_now = true;      /* the SYN-ACK */
                output(c, now);
                return;
            }
        }
        send_rst(src, t, dlen + ((flags & F_SYN) ? 1 : 0) + ((flags & F_FIN) ? 1 : 0));
        return;
    }

    if (flags & F_RST) {
        /* in the window (or exactly where we are): the PC gave up */
        if (seq_le(c->rcv_nxt, seq) && seq_lt(seq, c->rcv_nxt + RX_SIZE + 1)) {
            c->reset = true;
            conn_closed(c);
        }
        return;
    }
    if (flags & F_SYN) {
        if (c->state == S_SYN_RCVD && seq + 1 == c->rcv_nxt) {
            c->ack_now = true;          /* our SYN-ACK was lost */
            output(c, now);
        } else {
            c->ack_now = true;          /* RFC 5961: answer with an ACK */
            output(c, now);
        }
        return;
    }
    if (!(flags & F_ACK))
        return;

    if (c->state == S_SYN_RCVD) {
        if (ack != c->iss + 1) {
            send_rst(src, t, dlen);
            return;
        }
        c->snd_una = c->snd_max = c->iss + 1;
        c->state = S_ESTABLISHED;
        c->rtx_at = 0;
        c->retries = 0;
        c->queued = true;
        net_stat.tcp_accepted++;
        sem_post(&accept_sem);
    } else {
        ack_in(c, ack, now);
        if (c->state == S_FREE || c->state == S_CLOSED)
            return;
    }
    c->snd_wnd = get16(t + 14);
    if (!c->snd_wnd)                    /* answering, just not reading: alive */
        c->retries = 0;

    /* data, in order only */
    if (dlen || (flags & F_FIN)) {
        bool ok = true;

        if (seq_lt(seq, c->rcv_nxt)) {          /* partly old */
            uint32_t skip = c->rcv_nxt - seq;

            if (skip >= (uint32_t)dlen && !(skip == (uint32_t)dlen && (flags & F_FIN))) {
                ok = false;                     /* all of it seen before */
            } else if (skip > (uint32_t)dlen) {
                ok = false;
            } else {
                data += skip;
                dlen -= (int)skip;
                seq = c->rcv_nxt;
            }
        } else if (seq != c->rcv_nxt) {
            ok = false;                         /* a gap: dropped */
        }
        if (ok && dlen && (c->state == S_ESTABLISHED || c->state == S_FIN_WAIT_1 ||
                           c->state == S_FIN_WAIT_2) && !c->fin_rcvd) {
            uint32_t room = rx_window(c), n = (uint32_t)dlen < room ? (uint32_t)dlen : room;
            uint32_t at = (c->rx_start + c->rx_len) % RX_SIZE, first = RX_SIZE - at;

            if (first > n)
                first = n;
            memcpy(c->rx + at, data, first);
            memcpy(c->rx, data + first, n - first);
            c->rx_len += n;
            c->rcv_nxt += n;
            if (n < (uint32_t)dlen)
                flags &= (uint8_t)~F_FIN;       /* the FIN is beyond what we took */
            if (n)
                sem_post(&c->rx_sem);
        }
        if (ok && (flags & F_FIN) && !c->fin_rcvd) {
            c->rcv_nxt++;
            c->fin_rcvd = true;
            sem_post(&c->rx_sem);
            if (c->state == S_ESTABLISHED)
                c->state = S_CLOSE_WAIT;
            else if (c->state == S_FIN_WAIT_1)
                c->state = c->fin_sent ? S_CLOSING : S_CLOSE_WAIT;
            else if (c->state == S_FIN_WAIT_2) {
                c->state = S_TIME_WAIT;
                c->tw_until = now + TIME_WAIT_MS;
            }
        }
        c->ack_now = true;
    }
    output(c, now);
}

/* ---- the shell's side --------------------------------------------------------- */

void tcp_shutdown(void)
{
    for (int i = 0; i < TCP_CONN_N; i++)
        if (conns[i].state != S_FREE) {
            sem_post(&conns[i].rx_sem);
            sem_post(&conns[i].tx_sem);
        }
    sem_post(&accept_sem);
    sem_post(&accept_sem);
}

void tcp_drop_all(void)
{
    for (int i = 0; i < TCP_CONN_N; i++) {
        tcp_conn_t *c = &conns[i];

        if (c->state != S_FREE && c->state != S_CLOSED) {
            c->reset = true;
            conn_closed(c);
        }
    }
}

void tcp_free_all(void)
{
    for (int i = 0; i < TCP_CONN_N; i++)
        if (conns[i].state != S_FREE)
            conn_free(&conns[i]);
}

tcp_conn_t *tcp_accept(uint32_t timeout_ms)
{
    tcp_conn_t *c = NULL;

    if (net_stopping || sem_wait(&accept_sem, timeout_ms) != WAIT_OK || net_stopping)
        return NULL;
    mutex_lock(&net_lock, WAIT_FOREVER);
    for (int i = 0; i < TCP_CONN_N && !c; i++)
        if (conns[i].state != S_FREE && conns[i].queued) {
            c = &conns[i];
            c->queued = false;
            c->owned = true;
        }
    mutex_unlock(&net_lock);
    return c;
}

static bool gone(const tcp_conn_t *c)
{
    return c->reset || c->state == S_CLOSED || c->state == S_FREE;
}

int tcp_read(tcp_conn_t *c, void *buf, int n, uint32_t timeout_ms)
{
    uint32_t t0 = kernel_ticks();

    for (;;) {
        uint32_t waited;

        mutex_lock(&net_lock, WAIT_FOREVER);
        if (net_stopping) {
            mutex_unlock(&net_lock);
            return NET_ECLOSED;
        }
        if (c->rx_len) {
            uint32_t k = (uint32_t)n < c->rx_len ? (uint32_t)n : c->rx_len;
            uint32_t first = RX_SIZE - c->rx_start;

            if (first > k)
                first = k;
            memcpy(buf, c->rx + c->rx_start, first);
            memcpy((uint8_t *)buf + first, c->rx, k - first);
            c->rx_start = (c->rx_start + k) % RX_SIZE;
            c->rx_len -= k;
            /* a window update once a segment's worth has opened */
            if (c->rcv_nxt + rx_window(c) - c->rcv_adv >= (uint32_t)c->mss) {
                c->ack_now = true;
                net_wake();
            }
            mutex_unlock(&net_lock);
            return (int)k;
        }
        if (c->fin_rcvd) {
            mutex_unlock(&net_lock);
            return 0;
        }
        if (gone(c)) {
            mutex_unlock(&net_lock);
            return NET_ERESET;
        }
        mutex_unlock(&net_lock);
        waited = kernel_ticks() - t0;
        if (timeout_ms != WAIT_FOREVER && waited >= timeout_ms)
            return NET_EAGAIN;
        sem_wait(&c->rx_sem, timeout_ms == WAIT_FOREVER ? 500 : timeout_ms - waited);
    }
}

int tcp_write(tcp_conn_t *c, const void *buf, int n)
{
    const uint8_t *p = buf;
    int left = n;

    while (left > 0) {
        uint32_t room, k, at, first;

        mutex_lock(&net_lock, WAIT_FOREVER);
        if (net_stopping) {
            mutex_unlock(&net_lock);
            return NET_ECLOSED;
        }
        if (gone(c) || c->fin_queued || c->state == S_TIME_WAIT || c->state == S_LAST_ACK) {
            mutex_unlock(&net_lock);
            return NET_ERESET;
        }
        room = TX_SIZE - c->tx_len;
        if (!room) {
            mutex_unlock(&net_lock);
            sem_wait(&c->tx_sem, 200);
            continue;
        }
        k = (uint32_t)left < room ? (uint32_t)left : room;
        at = (c->tx_start + c->tx_len) % TX_SIZE;
        first = TX_SIZE - at;
        if (first > k)
            first = k;
        memcpy(c->tx + at, p, first);
        memcpy(c->tx, p + first, k - first);
        c->tx_len += k;
        mutex_unlock(&net_lock);
        net_wake();
        p += k;
        left -= (int)k;
    }
    return n;
}

void tcp_flush(tcp_conn_t *c, uint32_t ms)
{
    uint32_t t0 = kernel_ticks();

    while (kernel_ticks() - t0 < ms && !net_stopping) {
        bool done;

        mutex_lock(&net_lock, WAIT_FOREVER);
        done = gone(c) || c->tx_len == 0;
        mutex_unlock(&net_lock);
        if (done)
            return;
        sem_wait(&c->tx_sem, 50);
    }
}

void tcp_close(tcp_conn_t *c)
{
    if (!c)
        return;
    mutex_lock(&net_lock, WAIT_FOREVER);
    c->app_closed = true;
    if (c->state == S_CLOSED || c->state == S_FREE || net_stopping) {
        if (c->state != S_FREE && !net_stopping)
            conn_free(c);
    } else if (c->state == S_ESTABLISHED || c->state == S_CLOSE_WAIT) {
        c->fin_queued = true;
    }
    /* SYN_RCVD cannot be owned; FIN_WAIT, CLOSING, TIME_WAIT, LAST_ACK end
     * by themselves and free it (owned and app_closed) */
    mutex_unlock(&net_lock);
    net_wake();
}

uint16_t tcp_port(const tcp_conn_t *c)
{
    return c->lport;
}

uint32_t tcp_peer(const tcp_conn_t *c)
{
    return c->rip;
}
