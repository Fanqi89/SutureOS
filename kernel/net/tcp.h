/* SutureOS module: TCP (RFC 793 subset) over the SutureOS IPv4 stack
 *
 * SutureOS is GPL-3.0.  This is a freestanding C11 implementation written for
 * this kernel; the wire format and state-machine shape were cross-checked
 * against these read-only reference drivers in the sibling repository
 * C:\Users\fanqi\Desktop\自研操作系统\ (never modified):
 *   1. SpiritFoxOS kernel/src/net/tcp.c      (GPL-3.0) - header layout, flags,
 *      pseudo-header checksum organisation.
 *   2. Cinux       kernel/net/tcp.cpp        (MIT)     - connection states,
 *      seq/ack bookkeeping.
 *   3. XJ380       kmod/netserver/lwip/...   (BSD-3)   - reference only for the
 *      handshake/teardown ordering (no lwIP code is used).
 * The header/flag/state constants themselves already live in kernel/net/net.h.
 *
 * Scope (implemented, polled - the kernel has no TCP interrupt path):
 *   - active open (connect) with SYN retransmission
 *   - in-order bidirectional data transfer, one outstanding segment
 *     (stop-and-wait), retransmitted on a fixed RTO
 *   - active and passive close, TIME_WAIT expiry, RST handling, and an RST
 *     reply for segments that match no connection (RFC 793 section 3.9)
 *   - checksum verification on receive and generation on send
 *
 * NOT implemented (honest list): listen()/accept() backlog, out-of-order
 * reassembly, duplicate-ACK fast retransmit, SACK, window scaling, timestamps,
 * zero-window probing, MSS negotiation, congestion control, urgent data.
 */

#ifndef STITCH_TCP_H
#define STITCH_TCP_H

#include <stitch/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bounded connection table: one entry per simultaneously open socket. */
#define TCP_MAX_CONNS       4
/* Per-connection receive window / buffer (bytes). Advertised as-is. */
#define TCP_RX_RING_SZ      4096u
/* Maximum segment size we ever put on the wire. */
#define TCP_MSS_SZ          1460u
/* Retransmission timeout and retry budget for one outstanding segment.
 * Fixed (no RTT estimation): QEMU/slirp answers in well under a millisecond,
 * a real path is covered by the retries. */
#define TCP_RTO_US          500000ull      /* 500 ms */
#define TCP_MAX_RETRIES     4
/* How long a closed connection stays in TIME_WAIT before its slot is reused. */
#define TCP_TIME_WAIT_US    2000000ull     /* 2 s */

/* Reset the connection table (idempotent). */
void tcp_init(void);

/* Active open to dst_ip:dst_port. Blocks (pumping net_poll()) until the
 * handshake completes or timeout_ms expires. Returns a handle >= 0, or a
 * negative error: -1 no free slot / not established (timeout, RST, no NIC).
 * Ports are in host byte order, addresses in host byte order (0x0A00020F). */
int tcp_connect(uint32_t dst_ip, uint16_t dst_port, int timeout_ms);

/* Passive open: listen on `local_port` (host order). Returns a listener handle
 * >= 0, or negative (-3 no free slot, -2 port already being listened on).
 * The listener occupies one slot; every incoming SYN spawns a child connection
 * in another slot. */
int tcp_listen(uint16_t local_port);

/* Wait (bounded) for a child of `listener` to reach ESTABLISHED and hand it
 * over. Returns the child handle, or -1 on timeout / -2 on a bad handle /
 * -3 when the listener has no free child slot. peer_ip/peer_port may be NULL. */
int tcp_listen_accept(int listener, int timeout_ms,
                      uint32_t *peer_ip, uint16_t *peer_port);

/* Handle of the most recently created child of `listener`, or -1 when none.
 * Lets diagnostics and self-tests drive a connection they created by injecting
 * segments by hand. */
int tcp_listen_child(int listener);

/* 1 when the handle's handshake has completed (ESTABLISHED or a later state). */
int tcp_is_established(int h);

/* Send len bytes, split into MSS-sized segments, waiting for each one to be
 * acknowledged (bounded by the RTO/retry budget). Returns 0, or -1 when the
 * connection is not usable or a segment could not be acknowledged. */
int tcp_send(int h, const void *data, uint32_t len);

/* Copy up to cap received bytes into buf. Waits (pumping the stack) until at
 * least one byte arrives or timeout_ms expires. Returns the number of bytes
 * copied (> 0), 0 on timeout with nothing available, or -1 on error. */
int tcp_recv(int h, void *buf, uint32_t cap, int timeout_ms);

/* Graceful close: sends FIN and waits (bounded) for it to be acknowledged or
 * for the peer's FIN, then releases the slot. Returns 0 when the FIN was
 * acknowledged, -1 when it was not (the slot is released either way). */
int tcp_close(int h);

/* TCP_* state of a handle (see net.h), or -1 for an invalid handle. */
int tcp_state(int h);
/* 1 once the peer's FIN has been received on that handle. */
int tcp_peer_closed(int h);
/* Bytes currently buffered for reading on that handle. */
uint32_t tcp_rx_available(int h);
/* Next send / expected receive sequence number (0 for an invalid handle).
 * Diagnostics and the in-kernel self-test use these to drive the state
 * machine with hand-built segments. */
uint32_t tcp_snd_nxt(int h);
uint32_t tcp_rcv_nxt(int h);

/* Feed one received TCP segment (header + payload, no IP header). Called by
 * the IPv4 dispatch in net.c. */
void tcp_input(uint32_t src_ip, const uint8_t *seg, uint32_t len);

/* Drive retransmission timers and TIME_WAIT expiry. Called from net_poll(). */
void tcp_poll(void);

/* Print the "[tcp] ..." diagnostics block (counters + per-connection state). */
void tcp_dump_stats(void);

#ifdef __cplusplus
}
#endif

#endif /* STITCH_TCP_H */
