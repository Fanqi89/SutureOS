/* SutureOS module: TCP (RFC 793 subset) - implementation
 *
 * See kernel/net/tcp.h for the scope statement, the provenance of the wire
 * format / state machine (read-only reference drivers under
 * C:\Users\fanqi\Desktop\自研操作系统\: SpiritFoxOS kernel/src/net/tcp.c,
 * Cinux kernel/net/tcp.cpp) and the honest list of what is NOT implemented.
 *
 * Design notes for this kernel:
 *   - Everything is static: one fixed connection table and one static buffer
 *     for the payload of the single outstanding segment. The stack has exactly
 *     one static TX frame (net_tx_payload()), so only one segment can be under
 *     construction at a time - hence the stop-and-wait design.
 *   - There is no TCP interrupt path, so receive processing happens inside
 *     net_poll(): it drains the NIC ring, dispatches to tcp_input() and then
 *     calls tcp_poll() for retransmission timers. Every wait loop in this file
 *     pumps net_poll() and is bounded by a wall-clock budget from
 *     net_now_us() (PIT-derived), never by a spin count alone.
 *   - ACK/PSH/FIN are only ever sent in response to something, so tcp_input()
 *     never blocks; the peer's MAC is already in the ARP cache by then.
 */
#include "tcp.h"
#include "net.h"

#include <stitch/types.h>
#include <stitch/string.h>
#include <stitch/console.h>
#include <stitch/io.h>

/* ===================== connection table ===================== */
struct tcp_conn {
    int      used;
    int      state;                 /* TCP_* (net.h)                          */
    uint32_t remote_ip;             /* host order                             */
    uint16_t remote_port;           /* host order                             */
    uint16_t local_port;            /* host order                             */

    uint32_t iss;                   /* initial send sequence                  */
    uint32_t snd_una;
    uint32_t snd_nxt;
    uint32_t rcv_nxt;

    /* Single outstanding segment (stop-and-wait). */
    uint8_t  out_flags;
    uint16_t out_len;               /* payload bytes, 0 for pure SYN/ACK/FIN  */
    uint32_t out_seq;
    int      out_active;
    int      out_retries;
    uint64_t out_sent_us;

    int      peer_fin;
    int      rst_received;
    uint64_t time_wait_until;

    /* Passive open bookkeeping. `listening` marks a listener slot (it owns only
     * a local port); children point back at it through `parent` and become
     * visible to tcp_listen_accept() once `established` is set. */
    int      listening;
    int      parent;                /* listener index, -1 for an active open  */
    int      established;           /* handshake completed                    */
    int      taken;                 /* already handed out by tcp_listen_accept */

    /* One out-of-order segment held back until the gap in front of it is
     * filled. A full TCP would keep a queue; a single slot already turns the
     * common "two segments arrived swapped" case into in-order delivery instead
     * of a lost segment. */
    int      ooo_valid;
    uint32_t ooo_seq;
    uint16_t ooo_len;
    uint8_t  ooo_buf[TCP_MSS_SZ];

    uint32_t rx_head;               /* byte ring (rx_tail == head when empty)  */
    uint32_t rx_tail;
    uint32_t rx_count;

    uint8_t  rx[TCP_RX_RING_SZ];
};

static struct tcp_conn g_conns[TCP_MAX_CONNS];
static uint8_t  g_out_buf[TCP_MSS_SZ];  /* payload of the outstanding segment */
static uint32_t g_isn_jitter;

/* Counters (printed by tcp_dump_stats()). */
static uint64_t g_rx_seg, g_tx_seg, g_retrans, g_badsum, g_ooo, g_rx_dropped;
static uint64_t g_rst_tx, g_rst_rx, g_connects, g_conn_fail, g_timeouts;
static uint64_t g_accepts, g_ooo_buffered, g_ooo_drained;

/* ===================== helpers ===================== */
void tcp_init(void)
{
    memset(g_conns, 0, sizeof(g_conns));
    memset(g_out_buf, 0, sizeof(g_out_buf));
    g_isn_jitter = 0;
    g_rx_seg = g_tx_seg = g_retrans = g_badsum = g_ooo = g_rx_dropped = 0;
    g_rst_tx = g_rst_rx = g_connects = g_conn_fail = g_timeouts = 0;
}

static struct tcp_conn *conn_of(int h)
{
    if (h < 0 || h >= TCP_MAX_CONNS)
        return NULL;
    if (!g_conns[h].used)
        return NULL;
    return &g_conns[h];
}

static void conn_free(struct tcp_conn *c)
{
    memset(c, 0, sizeof(*c));
}

static struct tcp_conn *conn_alloc(void);   /* forward: used by the RX helpers */
static void rx_push(struct tcp_conn *c, const uint8_t *d, uint32_t n);
static int  emit(struct tcp_conn *c, uint8_t flags, uint32_t seq,
                 const uint8_t *data, uint16_t len);

static struct tcp_conn *conn_alloc(void)
{
    int i;

    for (i = 0; i < TCP_MAX_CONNS; i++) {
        if (!g_conns[i].used) {
            memset(&g_conns[i], 0, sizeof(g_conns[i]));
            g_conns[i].used   = 1;
            g_conns[i].parent = -1;
            return &g_conns[i];
        }
    }
    return NULL;
}

static uint32_t next_isn(void)
{
    g_isn_jitter += 0x9E3779B9u;                /* spread successive ISNs */
    return net_get_tcp_initial_seq() + g_isn_jitter;
}

/* Copy received payload into the ring, advancing rcv_nxt. A full window drops
 * the tail of the segment (counted) - there is no window shrinking. */
static void tcp_ring_in(struct tcp_conn *c, const uint8_t *data, uint32_t len)
{
    uint32_t space = TCP_RX_RING_SZ - c->rx_count;
    uint32_t n = len > space ? space : len;

    rx_push(c, data, n);
    c->rcv_nxt += n;
    if (n < len)
        g_rx_dropped++;
}

static uint16_t conn_window(const struct tcp_conn *c)
{
    uint32_t free_bytes = TCP_RX_RING_SZ - c->rx_count;
    return (uint16_t)free_bytes;
}

static void rx_push(struct tcp_conn *c, const uint8_t *d, uint32_t n)
{
    uint32_t i;

    for (i = 0; i < n; i++) {
        c->rx[c->rx_head] = d[i];
        c->rx_head = (c->rx_head + 1u) % TCP_RX_RING_SZ;
    }
    c->rx_count += n;
}

static uint32_t rx_pop(struct tcp_conn *c, uint8_t *dst, uint32_t cap)
{
    uint32_t n = c->rx_count < cap ? c->rx_count : cap;
    uint32_t i;

    for (i = 0; i < n; i++) {
        dst[i] = c->rx[c->rx_tail];
        c->rx_tail = (c->rx_tail + 1u) % TCP_RX_RING_SZ;
    }
    c->rx_count -= n;
    return n;
}

/* Build one segment in the shared TX frame and hand it to IPv4. `data` may be
 * NULL when len == 0. The checksum covers the 20-byte header (no options) plus
 * the payload. Returns 0, or -1 when the frame could not be queued. */
static int emit(struct tcp_conn *c, uint8_t flags, uint32_t seq,
                const uint8_t *data, uint16_t len)
{
    uint8_t *p = net_tx_payload();
    tcp_header_t *th = (tcp_header_t *)(void *)p;
    uint32_t seglen = 20u + (uint32_t)len;

    if (seglen > net_tx_payload_max())
        return -1;

    th->src_port    = htons(c->local_port);
    th->dst_port    = htons(c->remote_port);
    th->seq_num     = htonl(seq);
    th->ack_num     = htonl(c->rcv_nxt);
    th->data_offset_flags = (uint8_t)(5u << 4);     /* 20-byte header */
    th->flags       = flags;
    th->window_size = htons(conn_window(c));
    th->checksum    = 0u;
    th->urgent_ptr  = 0u;
    if (len != 0u && data != NULL)
        memcpy(p + 20u, data, len);

    th->checksum = htons(net_pseudo_checksum(net_local_ip, c->remote_ip,
                                             IP_PROTO_TCP, p, seglen));
    if (net_ip_send(c->remote_ip, IP_PROTO_TCP, seglen) != 0)
        return -1;
    g_tx_seg++;
    return 0;
}

/* RFC 793 section 3.9: answer a segment that matches no connection with RST. */
static void emit_rst(uint32_t dst_ip, uint16_t dst_port, uint16_t src_port,
                     uint32_t ack, uint32_t seq, uint32_t plen, uint8_t flags)
{
    uint8_t *p = net_tx_payload();
    tcp_header_t *th = (tcp_header_t *)(void *)p;

    th->src_port    = htons(dst_port);
    th->dst_port    = htons(src_port);
    th->data_offset_flags = (uint8_t)(5u << 4);
    th->window_size = 0u;
    th->checksum    = 0u;
    th->urgent_ptr  = 0u;
    if ((flags & TCP_FLAG_ACK) != 0u) {
        /* <SEQ=SEG.ACK><CTL=RST> */
        th->flags   = TCP_FLAG_RST;
        th->seq_num = htonl(ack);
        th->ack_num = 0u;
    } else {
        /* <SEQ=0><ACK=SEG.SEQ+SEG.LEN><CTL=RST,ACK> */
        th->flags   = (uint8_t)(TCP_FLAG_RST | TCP_FLAG_ACK);
        th->seq_num = 0u;
        th->ack_num = htonl(seq + plen
                            + ((flags & TCP_FLAG_SYN) ? 1u : 0u)
                            + ((flags & TCP_FLAG_FIN) ? 1u : 0u));
    }
    th->checksum = htons(net_pseudo_checksum(net_local_ip, dst_ip,
                                             IP_PROTO_TCP, p, 20u));
    if (net_ip_send(dst_ip, IP_PROTO_TCP, 20u) == 0)
        g_rst_tx++;
}

/* Deliver `len` bytes starting at `seq` into the receive ring, using the
 * one-slot out-of-order buffer. Always answers with an ACK: the duplicate ACK
 * sent for a future segment tells the peer exactly which byte we are waiting
 * for, which is what makes the buffered segment usable once the gap is filled. */
static void tcp_deliver(struct tcp_conn *c, uint32_t seq,
                        const uint8_t *data, uint32_t len)
{
    if (seq == c->rcv_nxt) {
        tcp_ring_in(c, data, len);
        /* The held-back segment may now be contiguous: drain it too. */
        if (c->ooo_valid && c->ooo_seq == c->rcv_nxt) {
            c->ooo_valid = 0;
            tcp_ring_in(c, c->ooo_buf, c->ooo_len);
            g_ooo_drained++;
        }
        (void)emit(c, TCP_FLAG_ACK, c->snd_nxt, NULL, 0);
        return;
    }
    if (seq > c->rcv_nxt) {
        if (!c->ooo_valid && len <= TCP_MSS_SZ) {
            memcpy(c->ooo_buf, data, len);
            c->ooo_seq   = seq;
            c->ooo_len   = (uint16_t)len;
            c->ooo_valid = 1;
            g_ooo_buffered++;
        } else {
            g_ooo++;                    /* slot busy or oversized: drop it    */
        }
        (void)emit(c, TCP_FLAG_ACK, c->snd_nxt, NULL, 0);
        return;
    }
    /* Below rcv_nxt: a retransmission of data already delivered. */
    g_ooo++;
    (void)emit(c, TCP_FLAG_ACK, c->snd_nxt, NULL, 0);
}

static const char *state_name(int st)
{
    switch (st) {
    case TCP_CLOSED:       return "CLOSED";
    case TCP_LISTEN:       return "LISTEN";
    case TCP_SYN_SENT:     return "SYN_SENT";
    case TCP_SYN_RECEIVED: return "SYN_RCVD";
    case TCP_ESTABLISHED:  return "ESTABLISHED";
    case TCP_FIN_WAIT_1:   return "FIN_WAIT_1";
    case TCP_FIN_WAIT_2:   return "FIN_WAIT_2";
    case TCP_CLOSING:      return "CLOSING";
    case TCP_TIME_WAIT:    return "TIME_WAIT";
    case TCP_CLOSE_WAIT:   return "CLOSE_WAIT";
    case TCP_LAST_ACK:     return "LAST_ACK";
    default:               return "?";
    }
}

/* ===================== active open ===================== */
int tcp_connect(uint32_t dst_ip, uint16_t dst_port, int timeout_ms)
{
    struct tcp_conn *c = NULL;
    uint64_t start, budget;

    if (!net_hw_initialized)
        return -2;
    if (dst_port == 0u)
        return -2;

    c = conn_alloc();
    if (c == NULL)
        return -3;

    c->remote_ip   = dst_ip;
    c->remote_port = dst_port;
    c->local_port  = net_alloc_port();
    if (c->local_port == 0u)
        c->local_port = (uint16_t)(49152u + (uint32_t)(c - g_conns) * 7u);

    c->iss     = next_isn();
    c->snd_una = c->iss;
    c->snd_nxt = c->iss + 1u;                    /* SYN consumes one seq   */
    c->rcv_nxt = 0;
    c->state   = TCP_SYN_SENT;

    c->out_flags    = TCP_FLAG_SYN;
    c->out_len      = 0;
    c->out_seq      = c->iss;
    c->out_active   = 1;
    c->out_retries  = 0;
    c->out_sent_us  = net_now_us();

    if (emit(c, TCP_FLAG_SYN, c->iss, NULL, 0) != 0) {
        conn_free(c);
        return -4;
    }
    g_connects++;

    start  = net_now_us();
    budget = (uint64_t)(timeout_ms > 0 ? timeout_ms : 3000) * 1000ull;
    for (;;) {
        net_poll();
        if (c->state == TCP_ESTABLISHED)
            return (int)(c - g_conns);
        if (c->state == TCP_CLOSED) {            /* RST or retry budget spent */
            conn_free(c);
            g_conn_fail++;
            return -5;
        }
        if (net_now_us() - start >= budget) {
            conn_free(c);
            g_conn_fail++;
            g_timeouts++;
            return -1;
        }
        io_wait();
    }
}

/* ===================== passive open ===================== */
int tcp_listen(uint16_t local_port)
{
    struct tcp_conn *c;
    int i;

    if (!net_hw_initialized)
        return -2;
    if (local_port == 0u)
        return -2;

    for (i = 0; i < TCP_MAX_CONNS; i++) {
        struct tcp_conn *k = &g_conns[i];
        if (k->used && k->listening && k->local_port == local_port)
            return -2;                            /* already listening */
    }

    c = conn_alloc();
    if (c == NULL)
        return -3;

    c->listening  = 1;
    c->state      = TCP_LISTEN;
    c->local_port = local_port;
    return (int)(c - g_conns);
}

int tcp_listen_accept(int listener, int timeout_ms,
                      uint32_t *peer_ip, uint16_t *peer_port)
{
    struct tcp_conn *l = conn_of(listener);
    uint64_t start, budget;

    if (l == NULL || !l->listening)
        return -2;

    start  = net_now_us();
    budget = (uint64_t)(timeout_ms > 0 ? timeout_ms : 1000) * 1000ull;
    for (;;) {
        int i;

        for (i = 0; i < TCP_MAX_CONNS; i++) {
            struct tcp_conn *k = &g_conns[i];
            if (k->used && k->parent == listener && !k->taken &&
                (k->state == TCP_ESTABLISHED || k->state == TCP_CLOSE_WAIT)) {
                k->taken = 1;
                if (peer_ip)   *peer_ip   = k->remote_ip;
                if (peer_port) *peer_port = k->remote_port;
                return i;
            }
        }
        net_poll();
        if (net_now_us() - start >= budget)
            return -1;                            /* nobody connected */
        io_wait();
    }
}

int tcp_listen_child(int listener)
{
    int i, best = -1;

    for (i = 0; i < TCP_MAX_CONNS; i++)
        if (g_conns[i].used && g_conns[i].parent == listener)
            best = i;                             /* most recently created */
    return best;
}

int tcp_is_established(int h)
{
    struct tcp_conn *c = conn_of(h);

    if (c == NULL || c->listening)
        return 0;
    switch (c->state) {
    case TCP_ESTABLISHED:
    case TCP_CLOSE_WAIT:
    case TCP_FIN_WAIT_1:
    case TCP_FIN_WAIT_2:
    case TCP_TIME_WAIT:
    case TCP_LAST_ACK:
        return 1;
    default:
        return 0;
    }
}

/* ===================== data transfer ===================== */
int tcp_send(int h, const void *data, uint32_t len)
{
    struct tcp_conn *c = conn_of(h);
    const uint8_t *p = (const uint8_t *)data;

    if (c == NULL)
        return -1;
    if (c->state != TCP_ESTABLISHED && c->state != TCP_CLOSE_WAIT)
        return -1;
    if (len != 0u && data == NULL)
        return -1;

    while (len > 0u) {
        uint32_t n = len > TCP_MSS_SZ ? TCP_MSS_SZ : len;
        uint64_t start;

        memcpy(g_out_buf, p, n);
        c->out_flags   = (uint8_t)(TCP_FLAG_ACK | TCP_FLAG_PSH);
        c->out_len     = (uint16_t)n;
        c->out_seq     = c->snd_nxt;
        c->out_active  = 1;
        c->out_retries = 0;
        c->out_sent_us = net_now_us();

        if (emit(c, c->out_flags, c->out_seq, g_out_buf, (uint16_t)n) != 0) {
            c->out_active = 0;
            return -1;
        }
        c->snd_nxt += n;

        /* Wait for the ACK; tcp_poll() retransmits while we spin here. */
        start = net_now_us();
        while (c->out_active) {
            net_poll();
            if (c->state == TCP_CLOSED)
                return -1;                       /* peer reset the connection */
            if (net_now_us() - start >= 5000000ull) {
                c->out_active = 0;
                g_timeouts++;
                return -1;
            }
            io_wait();
        }
        p   += n;
        len -= n;
    }
    return 0;
}

int tcp_recv(int h, void *buf, uint32_t cap, int timeout_ms)
{
    struct tcp_conn *c = conn_of(h);
    uint64_t start, budget;

    if (c == NULL || buf == NULL || cap == 0u)
        return -1;
    if (c->rx_count > 0u)
        return (int)rx_pop(c, (uint8_t *)buf, cap);

    start  = net_now_us();
    budget = (uint64_t)(timeout_ms > 0 ? timeout_ms : 1000) * 1000ull;
    for (;;) {
        net_poll();
        if (c->rx_count > 0u)
            return (int)rx_pop(c, (uint8_t *)buf, cap);
        if (c->peer_fin || c->state == TCP_CLOSED)
            return 0;                            /* EOF / reset, nothing left */
        if (net_now_us() - start >= budget)
            return 0;                            /* timeout: 0 bytes         */
        io_wait();
    }
}

int tcp_close(int h)
{
    struct tcp_conn *c = conn_of(h);
    int ok = 1;
    uint64_t start;

    if (c == NULL)
        return -1;

    if (c->listening) {
        /* Closing a listener drops it plus any child whose handshake never
         * completed; established children are the caller's to close. */
        int i;
        for (i = 0; i < TCP_MAX_CONNS; i++) {
            struct tcp_conn *k = &g_conns[i];
            if (k->used && k->parent == (int)(c - g_conns) && !k->taken &&
                !k->established)
                conn_free(k);
        }
        conn_free(c);
        return 0;
    }

    if (c->state == TCP_ESTABLISHED || c->state == TCP_CLOSE_WAIT) {
        c->out_flags   = (uint8_t)(TCP_FLAG_ACK | TCP_FLAG_FIN);
        c->out_len     = 0;
        c->out_seq     = c->snd_nxt;
        c->out_active  = 1;
        c->out_retries = 0;
        c->out_sent_us = net_now_us();
        if (emit(c, c->out_flags, c->out_seq, NULL, 0) != 0) {
            conn_free(c);
            return -1;
        }
        c->snd_nxt += 1u;                        /* FIN consumes one seq */
        c->state = (c->state == TCP_ESTABLISHED) ? TCP_FIN_WAIT_1 : TCP_LAST_ACK;

        start = net_now_us();
        while (c->out_active && (net_now_us() - start) < 2000000ull) {
            net_poll();
            if (c->state == TCP_CLOSED) {        /* reset while closing */
                ok = 0;
                break;
            }
            io_wait();
        }
        if (c->out_active)
            ok = 0;                              /* FIN never acknowledged   */
    }

    /* Keep the slot for a short TIME_WAIT when the peer has not closed yet, so
     * its late FIN still matches this connection; tcp_poll() reaps it. */
    if (ok && !c->peer_fin && c->state != TCP_TIME_WAIT) {
        c->state           = TCP_TIME_WAIT;
        c->time_wait_until = net_now_us() + TCP_TIME_WAIT_US;
        return 0;
    }
    conn_free(c);
    return ok ? 0 : -1;
}

int tcp_state(int h)
{
    struct tcp_conn *c = conn_of(h);
    return c ? c->state : -1;
}

int tcp_peer_closed(int h)
{
    struct tcp_conn *c = conn_of(h);
    return c ? c->peer_fin : 0;
}

uint32_t tcp_rx_available(int h)
{
    struct tcp_conn *c = conn_of(h);
    return c ? c->rx_count : 0u;
}

/* Sequence-number introspection: diagnostics and the in-kernel self-test need
 * to know where the connection thinks it is (the test injects segments by hand
 * and must acknowledge exactly snd_nxt). */
uint32_t tcp_snd_nxt(int h)
{
    struct tcp_conn *c = conn_of(h);
    return c ? c->snd_nxt : 0u;
}

uint32_t tcp_rcv_nxt(int h)
{
    struct tcp_conn *c = conn_of(h);
    return c ? c->rcv_nxt : 0u;
}

/* ===================== receive path ===================== */
void tcp_input(uint32_t src_ip, const uint8_t *seg, uint32_t len)
{
    const tcp_header_t *th;
    uint32_t hlen, seq, ack, plen;
    uint8_t flags;
    struct tcp_conn *c = NULL;
    uint16_t sport, dport;
    int i;

    g_rx_seg++;
    if (len < 20u)
        return;
    if (net_pseudo_checksum(src_ip, net_local_ip, IP_PROTO_TCP, seg, len) != 0u) {
        g_badsum++;
        return;
    }

    th    = (const tcp_header_t *)(const void *)seg;
    hlen  = (uint32_t)(th->data_offset_flags >> 4) * 4u;
    if (hlen < 20u || hlen > len)
        return;
    flags = th->flags;
    seq   = ntohl(th->seq_num);
    ack   = ntohl(th->ack_num);
    plen  = len - hlen;
    sport = ntohs(th->src_port);
    dport = ntohs(th->dst_port);

    for (i = 0; i < TCP_MAX_CONNS; i++) {
        struct tcp_conn *k = &g_conns[i];
        if (k->used && !k->listening && k->remote_ip == src_ip &&
            k->remote_port == sport && k->local_port == dport) {
            c = k;
            break;
        }
    }

    if (c == NULL) {
        /* Passive open: a bare SYN addressed to a listening port creates a
         * child connection and answers with SYN+ACK. */
        struct tcp_conn *l = NULL;

        for (i = 0; i < TCP_MAX_CONNS; i++) {
            struct tcp_conn *k = &g_conns[i];
            if (k->used && k->listening && k->local_port == dport) {
                l = k;
                break;
            }
        }
        if (l != NULL && (flags & TCP_FLAG_SYN) != 0u &&
            (flags & TCP_FLAG_ACK) == 0u) {
            struct tcp_conn *ch = conn_alloc();

            if (ch == NULL) {
                g_conn_fail++;
                return;                          /* no slot: peer will retry */
            }
            ch->listening   = 0;
            ch->parent      = (int)(l - g_conns);
            ch->local_port  = l->local_port;
            ch->remote_ip   = src_ip;
            ch->remote_port = sport;
            ch->rcv_nxt     = seq + 1u;          /* the peer's SYN consumed 1 */
            ch->iss         = next_isn();
            ch->snd_una     = ch->iss;
            ch->snd_nxt     = ch->iss + 1u;      /* our SYN consumes 1        */
            ch->state       = TCP_SYN_RECEIVED;
            /* Record the SYN+ACK as the outstanding segment even if the send
             * itself fails (the peer's MAC may not be resolvable yet): a real
             * server keeps the TCB and lets tcp_poll() retransmit it, instead of
             * throwing the connection away because one frame could not leave. */
            ch->out_flags   = (uint8_t)(TCP_FLAG_SYN | TCP_FLAG_ACK);
            ch->out_len     = 0;
            ch->out_seq     = ch->iss;
            ch->out_active  = 1;
            ch->out_retries = 0;
            ch->out_sent_us = net_now_us();
            (void)emit(ch, ch->out_flags, ch->iss, NULL, 0);
            g_accepts++;
            return;
        }
        if ((flags & TCP_FLAG_RST) == 0u)
            emit_rst(src_ip, sport, dport, ack, seq, plen, flags);
        return;
    }

    if ((flags & TCP_FLAG_RST) != 0u) {
        g_rst_rx++;
        c->rst_received = 1;
        c->out_active   = 0;
        c->state        = TCP_CLOSED;
        return;
    }

    /* Handshake completion: SYN+ACK in SYN_SENT. */
    if (c->state == TCP_SYN_SENT) {
        if ((flags & (TCP_FLAG_SYN | TCP_FLAG_ACK)) ==
            (TCP_FLAG_SYN | TCP_FLAG_ACK) && ack == c->snd_nxt) {
            c->snd_una    = ack;
            c->rcv_nxt    = seq + 1u;
            c->out_active = 0;
            c->state      = TCP_ESTABLISHED;
            (void)emit(c, TCP_FLAG_ACK, c->snd_nxt, NULL, 0);
        }
        return;
    }

    /* Passive open completion: the third handshake segment (ACK of our
     * SYN+ACK) turns a child of a listener into ESTABLISHED and makes it
     * available to tcp_listen_accept(). Data may be piggybacked, so fall
     * through to the normal ACK/data handling instead of returning. */
    if (c->state == TCP_SYN_RECEIVED) {
        if ((flags & TCP_FLAG_ACK) != 0u && ack == c->snd_nxt) {
            c->snd_una     = ack;
            c->out_active  = 0;
            c->state       = TCP_ESTABLISHED;
            c->established = 1;
        } else {
            return;                     /* not the completing ACK yet */
        }
    }

    /* Acknowledge our outstanding segment. */
    if ((flags & TCP_FLAG_ACK) != 0u) {
        if (ack > c->snd_una && ack <= c->snd_nxt) {
            c->snd_una = ack;
            if (c->out_active) {
                uint32_t end = c->out_seq + (uint32_t)c->out_len
                             + ((c->out_flags & TCP_FLAG_SYN) ? 1u : 0u)
                             + ((c->out_flags & TCP_FLAG_FIN) ? 1u : 0u);
                if (ack >= end)
                    c->out_active = 0;
            }
        }
    }

    /* Data: in-order delivery plus the single out-of-order slot (see
     * tcp_deliver()). Anything that cannot be placed is dropped and re-ACKed,
     * which makes the peer retransmit it. */
    if (plen > 0u)
        tcp_deliver(c, seq, (const uint8_t *)(const void *)(seg + hlen), plen);

    if ((flags & TCP_FLAG_FIN) != 0u) {
        uint32_t fin_seq = seq + plen;

        if (fin_seq == c->rcv_nxt) {
            c->rcv_nxt = fin_seq + 1u;
            c->peer_fin = 1;
            (void)emit(c, TCP_FLAG_ACK, c->snd_nxt, NULL, 0);
            if (c->state == TCP_ESTABLISHED) {
                c->state = TCP_CLOSE_WAIT;
            } else if (c->state == TCP_FIN_WAIT_1) {
                c->state = TCP_TIME_WAIT;
                c->time_wait_until = net_now_us() + TCP_TIME_WAIT_US;
            } else if (c->state == TCP_FIN_WAIT_2) {
                c->state = TCP_TIME_WAIT;
                c->time_wait_until = net_now_us() + TCP_TIME_WAIT_US;
            } else if (c->state == TCP_LAST_ACK) {
                conn_free(c);
            }
        } else {
            g_ooo++;
            (void)emit(c, TCP_FLAG_ACK, c->snd_nxt, NULL, 0);
        }
    }
}

/* ===================== timers ===================== */
void tcp_poll(void)
{
    uint64_t now = net_now_us();
    int i;

    for (i = 0; i < TCP_MAX_CONNS; i++) {
        struct tcp_conn *c = &g_conns[i];

        if (!c->used)
            continue;

        if (c->state == TCP_TIME_WAIT && now >= c->time_wait_until) {
            conn_free(c);
            continue;
        }
        if (!c->out_active)
            continue;
        if (now - c->out_sent_us < TCP_RTO_US)
            continue;

        if (c->out_retries >= TCP_MAX_RETRIES) {
            /* The peer stopped answering: fail honestly instead of pretending
             * the segment went through. */
            console_printf("[tcp] give up: %u retransmits unacknowledged "
                           "(state=%s seq=%u len=%u)\n",
                           (uint64_t)c->out_retries, state_name(c->state),
                           (uint64_t)c->out_seq, (uint64_t)c->out_len);
            c->out_active = 0;
            c->state      = TCP_CLOSED;
            g_conn_fail++;
            g_timeouts++;
            continue;
        }

        c->out_retries++;
        g_retrans++;
        (void)emit(c, c->out_flags, c->out_seq,
                   c->out_len != 0u ? g_out_buf : NULL, c->out_len);
        c->out_sent_us = now;
    }
}

/* ===================== diagnostics ===================== */
void tcp_dump_stats(void)
{
    char a[16], b[16];
    uint32_t in_use = 0u;
    int i;

    for (i = 0; i < TCP_MAX_CONNS; i++)
        if (g_conns[i].used)
            in_use++;

    console_printf("[tcp] segments rx=%llu tx=%llu retrans=%llu badsum=%llu\n",
                   g_rx_seg, g_tx_seg, g_retrans, g_badsum);
    console_printf("[tcp] connects=%llu failed=%llu timeouts=%llu rst_tx=%llu rst_rx=%llu\n",
                   g_connects, g_conn_fail, g_timeouts, g_rst_tx, g_rst_rx);
    console_printf("[tcp] listens=%llu ooo_buffered=%llu ooo_drained=%llu ooo_dropped=%llu "
                   "rx_dropped=%llu conns=%u/%u\n",
                   g_accepts, g_ooo_buffered, g_ooo_drained, g_ooo, g_rx_dropped,
                   (uint64_t)in_use, (uint64_t)TCP_MAX_CONNS);

    for (i = 0; i < TCP_MAX_CONNS; i++) {
        struct tcp_conn *c = &g_conns[i];
        if (!c->used)
            continue;
        net_ip_to_str(c->remote_ip, a, sizeof(a));
        net_ip_to_str(net_local_ip, b, sizeof(b));
        console_printf("[tcp] conn%d %s %s:%u -> %s:%u snd_una=%u snd_nxt=%u "
                       "rcv_nxt=%u rx=%u retries=%u peer_fin=%u\n",
                       (int64_t)i, state_name(c->state),
                       a, (uint64_t)c->remote_port,
                       b, (uint64_t)c->local_port,
                       (uint64_t)c->snd_una, (uint64_t)c->snd_nxt,
                       (uint64_t)c->rcv_nxt, (uint64_t)c->rx_count,
                       (uint64_t)c->out_retries, (uint64_t)c->peer_fin);
    }
}
