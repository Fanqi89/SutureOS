/* SutureOS module: IPv4 network stack (ARP + IPv4 + ICMP + UDP) over RTL8139
 * Source: C:\Users\fanqi\Desktop\自研操作系统\SpiritFoxOS\kernel\include\net.h
 *         + kernel/src/net/{arp,ip,icmp,udp}.c (protocol shapes and naming)
 * Original license: GPL-3.0 - same license as this project.
 * Changes: adapted to SutureOS; the stub socket layer is kept for ABI stability
 *          but only the datagram paths below are implemented. Addresses are
 *          stored in host byte order (0x0A00020F == 10.0.2.15 == "first octet
 *          in the high byte"), matching the convention the stub net.c already
 *          used, and converted with htonl()/ntohl() on the way to/from the
 *          wire. Ports are kept in host order for the public API and converted
 *          internally. No libc, no dynamic allocation: every buffer is static.
 */
#ifndef STITCH_NET_H
#define STITCH_NET_H

#include <stitch/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* socklen_t */
typedef uint32_t socklen_t;

/* Ethernet types */
#define ETH_TYPE_IPV4   0x0800
#define ETH_TYPE_ARP    0x0806
#define ETH_TYPE_IPV6   0x86DD

/* IP protocols */
#define IP_PROTO_ICMP   1
#define IP_PROTO_TCP    6
#define IP_PROTO_UDP    17

/* Ethernet header length / minimum frame (without FCS) */
#define NET_ETH_HDR_LEN   14
#define NET_ETH_MIN_FRAME 60

/* Address families */
#define AF_UNIX         1
#define AF_LOCAL        AF_UNIX
#define AF_INET         2

/* Socket types */
#define SOCK_STREAM     1
#define SOCK_DGRAM      2

/* Socket options */
#define SOL_SOCKET      1
#define SO_REUSEADDR    2
#define SO_BINDTODEVICE 25

/* INADDR_ANY */
#define INADDR_ANY      0
#define INADDR_LOOPBACK 0x7F000001

/* Broadcast / netmask helpers (host byte order) */
#define NET_IP_BROADCAST 0xFFFFFFFFu

/* TCP states */
#define TCP_CLOSED       0
#define TCP_LISTEN       1
#define TCP_SYN_SENT     2
#define TCP_SYN_RECEIVED 3
#define TCP_ESTABLISHED  4
#define TCP_FIN_WAIT_1   5
#define TCP_FIN_WAIT_2   6
#define TCP_CLOSING      7
#define TCP_TIME_WAIT    8
#define TCP_CLOSE_WAIT   9
#define TCP_LAST_ACK     10

/* Socket flags */
#define SOCKF_NONBLOCK  0x01
#define SOCKF_CLOEXEC   0x02

/* recvmsg/sendmsg flags */
#define MSG_OOB        0x01
#define MSG_PEEK       0x02
#define MSG_DONTWAIT   0x40
#define MSG_NOSIGNAL   0x4000
#define MSG_CTRUNC     0x8000

/* Maximum values */
#define NET_MAX_SOCKETS    256
#define NET_MAX_CONNECTIONS 64
#define NET_RX_BUF_SIZE    4096
#define NET_TX_BUF_SIZE    4096
#define NET_TCP_WINDOW     65535
#define NET_TCP_MSS        1460
#define NET_TCP_BACKLOG    16

/* ARP cache size and entry lifetime (the lifetime counts net_poll() calls
 * rather than seconds, because the stack has no wall clock of its own -
 * net_poll() is called at least once per shell command and every millisecond
 * inside a ping). Entries also fall back to round-robin replacement. */
#define NET_ARP_CACHE_SIZE  8
#define NET_ARP_TTL_POLLS   60000u

/* Ping round-trip unit: 1 tick == 1 PIT channel-0 count == 1/1193182 s
 * ~= 0.838 us. net_ping() reports rtt_ticks in those units; net_ping_us()
 * converts a tick count to microseconds. */
#define NET_TICKS_PER_MS 1193u       /* 1193182 / 1000, floor             */

/* Ethernet header */
typedef struct __attribute__((packed)) {
    uint8_t  dst_mac[6];
    uint8_t  src_mac[6];
    uint16_t ethertype;
} eth_header_t;

/* ARP packet */
typedef struct __attribute__((packed)) {
    uint16_t htype;
    uint16_t ptype;
    uint8_t  hlen;
    uint8_t  plen;
    uint16_t oper;
    uint8_t  sha[6];
    uint32_t spa;
    uint8_t  tha[6];
    uint32_t tpa;
} arp_packet_t;

/* ICMP header */
typedef struct __attribute__((packed)) {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint32_t rest_of_header;
} icmp_header_t;

/* ICMP types / codes */
#define ICMP_TYPE_ECHO_REPLY   0
#define ICMP_TYPE_DEST_UNREACH 3
#define ICMP_TYPE_ECHO_REQUEST 8
#define ICMP_TYPE_TIME_EXCEED  11

/* IPv4 address (host byte order) */
typedef uint32_t ipv4_addr_t;

/* Port */
typedef uint16_t port_t;

/* Socket address (IPv4) */
typedef struct {
    uint16_t    sin_family;
    port_t      sin_port;
    ipv4_addr_t sin_addr;
    char        sin_zero[8];
} net_sockaddr_t;

/* Unix domain socket address */
#define UNIX_PATH_MAX   108

typedef struct {
    uint16_t sun_family;
    char     sun_path[UNIX_PATH_MAX];
} sockaddr_un_t;

/* IPv4 header */
typedef struct __attribute__((packed)) {
    uint8_t  version_ihl;
    uint8_t  dscp_ecn;
    uint16_t total_length;
    uint16_t identification;
    uint16_t flags_fragment;
    uint8_t  ttl;
    uint8_t  protocol;
    uint16_t header_checksum;
    ipv4_addr_t src_ip;
    ipv4_addr_t dst_ip;
} ipv4_header_t;

/* TCP header */
typedef struct __attribute__((packed)) {
    port_t   src_port;
    port_t   dst_port;
    uint32_t seq_num;
    uint32_t ack_num;
    uint8_t  data_offset_flags;
    uint8_t  flags;
    uint16_t window_size;
    uint16_t checksum;
    uint16_t urgent_ptr;
} tcp_header_t;

/* TCP flags */
#define TCP_FLAG_FIN  0x01
#define TCP_FLAG_SYN  0x02
#define TCP_FLAG_RST  0x04
#define TCP_FLAG_PSH  0x08
#define TCP_FLAG_ACK  0x10
#define TCP_FLAG_URG  0x20

/* UDP header */
typedef struct __attribute__((packed)) {
    port_t   src_port;
    port_t   dst_port;
    uint16_t length;
    uint16_t checksum;
} udp_header_t;

/* TCP unacknowledged segment (linked list) - kept for ABI stability only. */
typedef struct net_tcp_seg {
    uint32_t    seq;
    uint32_t    len;
    uint8_t    *data;
    uint64_t    send_time;
    uint8_t     retries;
    struct net_tcp_seg *next;
} net_tcp_seg_t;

/* msghdr for sendmsg/recvmsg (minimal) */
struct iovec {
    void  *iov_base;
    size_t iov_len;
};

struct cmsghdr {
    uint32_t cmsg_len;
    int      cmsg_level;
    int      cmsg_type;
};

struct msghdr {
    void         *msg_name;
    socklen_t     msg_namelen;
    struct iovec *msg_iov;
    size_t        msg_iovlen;
    void         *msg_control;
    size_t        msg_controllen;
    int           msg_flags;
};

#define CMSG_DATA(cmsg) ((void *)((char *)(cmsg) + sizeof(struct cmsghdr)))
#define CMSG_ALIGN(len) (((len) + sizeof(uint64_t) - 1) & ~(sizeof(uint64_t) - 1))
#define CMSG_SPACE(len) (CMSG_ALIGN(sizeof(struct cmsghdr)) + CMSG_ALIGN(len))
#define CMSG_LEN(len)   (CMSG_ALIGN(sizeof(struct cmsghdr)) + (len))

/* Socket structure (unchanged layout; the datagram subset is implemented) */
typedef struct net_socket {
    int         fd;
    int         domain;
    int         type;
    int         protocol;
    int         state;
    uint32_t    flags;

    ipv4_addr_t local_ip;
    port_t      local_port;
    ipv4_addr_t remote_ip;
    port_t      remote_port;

    uint32_t    seq_num;
    uint32_t    ack_num;
    uint32_t    remote_seq;

    uint32_t    cwnd;
    uint32_t    ssthresh;
    uint32_t    rto;
    uint32_t    srtt;
    uint32_t    rttvar;
    uint64_t    retransmit_timer;
    uint32_t    dup_ack_count;
    uint32_t    bytes_in_flight;
    uint32_t    last_ack_sent;
    uint8_t     congestion_state;
    uint64_t    time_wait_expire;
    uint32_t    retransmit_count;
    net_tcp_seg_t *unacked_list;

    uint8_t    *rx_buf;
    uint32_t    rx_read_pos;
    uint32_t    rx_write_pos;
    uint32_t    rx_count;
    uint32_t    rx_size;

    uint8_t    *tx_buf;
    uint32_t    tx_read_pos;
    uint32_t    tx_write_pos;
    uint32_t    tx_count;
    uint32_t    tx_size;

    struct net_socket *listen_parent;
    struct net_socket *pending_conns[NET_TCP_BACKLOG];
    int         pending_count;
    struct net_socket *next_pending;
    int         blocking_pid;

    char                unix_path[UNIX_PATH_MAX];
    struct net_socket   *unix_peer;
    int                 peer_pid;
    int                 peer_uid;
    int                 peer_gid;

    int             pending_fds[8];
    int             pending_fd_count;
    void *pending_files[8];
} net_socket_t;

/* ===================== network configuration globals ===================== */
/* Addresses are in host byte order (10.0.2.15 == 0x0A00020F). */
extern uint32_t net_local_ip;
extern uint32_t net_gateway_ip;
extern uint32_t net_netmask;
extern uint32_t net_dns_ip;
extern uint8_t  net_local_mac[6];
extern int      net_hw_initialized;

/* ===================== byte order helpers ===================== */
static inline uint16_t htons(uint16_t v) { return (uint16_t)(((v & 0x00FFu) << 8) | ((v >> 8) & 0x00FFu)); }
static inline uint16_t ntohs(uint16_t v) { return htons(v); }
static inline uint32_t htonl(uint32_t v)
{
    return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
           ((v & 0x00FF0000u) >> 8)  | ((v & 0xFF000000u) >> 24);
}
static inline uint32_t ntohl(uint32_t v) { return htonl(v); }

/* ===================== link / bring-up ===================== */
/* Find + reset the RTL8139, read its MAC, program the RX ring and TX
 * descriptors and apply the static slirp configuration. Idempotent; returns 0
 * on success and -1 after logging "[net] init failed: <reason>". */
int net_init(void);

/* Non-blocking: drain the RX ring and hand every frame to the stack. Safe to
 * call from a shell command, an idle loop or inside a ping. NEVER call it from
 * an interrupt handler (it calls user callbacks and does bounded polling). */
void net_poll(void);

/* Feed one complete ethernet frame (14-byte header + payload, no FCS) into the
 * stack. This is what the polled RX path calls per frame; it is public so a
 * test harness (or a future loopback/packet-socket) can inject frames. The
 * buffer must stay valid for the call. Returns nothing; frames that are too
 * short or that arrive before net_init() are counted as drops. */
void net_input(const void *frame, uint32_t len);

/* 1 once net_init() brought the NIC up (frames can actually flow). */
int net_ready(void);

/* Primary IPv4 address / gateway / netmask / DNS in host byte order. */
uint32_t net_ip(void);
uint32_t net_gw(void);
uint32_t net_mask(void);
uint32_t net_dns(void);
/* Copy the NIC MAC (6 bytes). Returns 0, or -1 when the stack is down. */
int net_mac(uint8_t out[6]);

/* ===================== address helpers ===================== */
/* "10.0.2.15" -> 0x0A00020F. Returns 0, or -1 when the text is not a plain
 * dotted-quad (no hostnames, no CIDR suffix). */
int net_ip_from_str(const char *s, uint32_t *out);
/* 0x0A00020F -> "10.0.2.15" in the caller's buffer (>= 16 bytes). */
void net_ip_to_str(uint32_t ip, char *buf, uint32_t buflen);

/* ===================== ARP ===================== */
/* Look up `ip` in the cache. Returns 0 and copies 6 bytes into mac_out, or -1
 * on a miss. Never sends anything. */
int net_arp_lookup(uint32_t ip, uint8_t mac_out[6]);
/* Resolve `ip`, sending up to `retries` requests and pumping net_poll() while
 * waiting. timeout_ms is a wall-clock budget (PIT-derived). Returns 0 and
 * copies the MAC, or -1 on timeout. */
int net_arp_resolve(uint32_t ip, uint8_t mac_out[6]);
/* Number of live cache entries. */
int net_arp_entries(void);
/* Send one ARP request for `ip` (broadcast) and return 0, or -1 when the
 * stack is down / the frame could not be queued. */
int net_arp_request(uint32_t ip);

/* ===================== ICMP ===================== */
/* Send an echo request to `ip` and wait for the matching reply. `timeout_ms`
 * is a wall-clock budget. Returns 0 when the first reply arrived and stores
 * the round-trip time in *rtt_ticks (1 tick = 1/1193182 s ~ 0.838 us; 0 is
 * possible for a slirp round trip faster than one PIT count). Returns -1 on
 * timeout or when the NIC is down. `rtt_ticks` may be NULL. */
int net_ping(uint32_t ip, int timeout_ms, uint32_t *rtt_ticks);
/* Same round trip expressed in microseconds (rtt_ticks * 1000000 / 1193182). */
uint32_t net_ping_us(uint32_t rtt_ticks);

/* ===================== UDP ===================== */
/* Inbound datagram hook. `data` points into a static 1500-byte buffer that is
 * reused by the next datagram, so copy what must outlive the call. */
typedef void (*net_udp_handler_t)(uint32_t src_ip, uint16_t sport,
                                  uint16_t dport, const void *data, uint32_t len);
void net_udp_set_handler(net_udp_handler_t cb);

/* Send one UDP datagram. Ports are in host order. Returns 0 on success, -1
 * when the NIC is down or the frame could not be queued, -2 when the payload
 * exceeds the 1472-byte ethernet limit. ARP resolution for the next hop is
 * attempted first (bounded). */
int net_udp_send(uint32_t ip, uint16_t sport, uint16_t dport,
                 const void *data, uint32_t len);

/* ===================== raw IPv4 TX bridge (kernel/net/tcp.c) =============
 * The stack owns one static TX frame; these three calls let another protocol
 * module (TCP) build a payload in it and hand it to the link layer without
 * duplicating the IPv4/ARP path. Only one packet can be under construction at
 * a time. */
/* Writable payload area (NET_IP payload limit) of the shared TX frame. */
uint8_t *net_tx_payload(void);
/* Bytes available at the pointer returned by net_tx_payload(). */
uint32_t net_tx_payload_max(void);
/* Send payload_len staged bytes as one IPv4 packet to dst_ip (host order).
 * Resolves the next hop (bounded). Returns 0, or -1 when the frame could not
 * be queued (no NIC, ARP failure, oversized). */
int net_ip_send(uint32_t dst_ip, uint8_t proto, uint32_t payload_len);
/* One's-complement pseudo-header checksum (src/dst/proto/length + data).
 * Unlike the UDP helper this never maps a zero result to 0xffff, so a received
 * TCP segment validates with `net_pseudo_checksum(...) == 0`. */
uint16_t net_pseudo_checksum(uint32_t src, uint32_t dst, uint8_t proto,
                             const uint8_t *data, uint32_t len);
/* Monotonic microsecond clock (PIT channel 0; wrap-safe). Also used by TCP for
 * its retransmission timer. */
uint64_t net_now_us(void);

/* ===================== statistics / diagnostics ===================== */
/* Print one "[net] ..." line per counter plus the last received packet's
 * summary. */
void net_dump_stats(void);

/* Reset every counter (does not touch the ARP cache or the NIC). */
void net_stats_reset(void);

/* Boot/shell self test: ping the gateway and print exactly one of
 * "[net] selftest ping 10.0.2.2 -> reply in <n>" (n = microseconds) or
 * "[net] selftest ping TIMEOUT". Returns 0 on reply, -1 on timeout (or when
 * the NIC is down, in which case the error is logged first). */
int net_selftest(void);

/* ===================== socket API (kept for ABI; datagram subset) ========= */
int net_socket(int domain, int type, int protocol);
int net_bind(int fd, const net_sockaddr_t *addr, socklen_t addrlen);
int net_listen(int fd, int backlog);
int net_accept(int fd, net_sockaddr_t *addr, socklen_t *addrlen);
int net_connect(int fd, const net_sockaddr_t *addr, socklen_t addrlen);
int net_send(int fd, const void *buf, size_t len, int flags);
int net_recv(int fd, void *buf, size_t len, int flags);
int net_sendto(int fd, const void *buf, size_t len, int flags,
               const net_sockaddr_t *dest_addr, socklen_t addrlen);
int net_recvfrom(int fd, void *buf, size_t len, int flags,
                 net_sockaddr_t *src_addr, socklen_t *addrlen);
int net_shutdown(int fd, int how);
int net_close(int fd);
int net_setsockopt(int fd, int level, int optname, const void *optval, socklen_t optlen);
int net_getsockopt(int fd, int level, int optname, void *optval, socklen_t *optlen);
int net_getsockname(int fd, net_sockaddr_t *addr, socklen_t *addrlen);
int net_getpeername(int fd, net_sockaddr_t *addr, socklen_t *addrlen);
/* SOCK_STREAM / SOCK_DGRAM of an open socket, or a negative errno. */
int net_socket_type(int fd);
int net_sendmsg(int fd, const struct msghdr *msg, int flags);
int net_recvmsg(int fd, struct msghdr *msg, int flags);

/* Configuration setters kept from the stub API. */
void net_configure_ip(uint32_t ip, uint32_t gateway, uint32_t netmask);
/* DHCP/DNS program the resolver address through this setter (there was only a
 * constant before); 0 clears it, net_dns() then answers 0. */
void net_set_dns_ip(uint32_t dns);
/* Source address used by the transmit path (IP header + UDP pseudo-header).
 * Defaults to net_local_ip. DHCP sets it to 0.0.0.0 for DISCOVER/REQUEST, since
 * a client without a lease may not claim an address, then clears it. */
void net_set_tx_src_ip(uint32_t ip);
void net_clear_tx_src_ip(void);
/* Port the legacy single UDP handler listens on (0 = accept every port). The
 * datagram socket path does not need it: a bound SOCK_DGRAM socket stages its
 * own datagrams by port. */
uint16_t net_udp_listen_port(void);
void net_udp_listen_set(uint16_t port);
/* Datagrams dropped because the target socket still had an unread one. */
uint64_t net_udp_dgram_drops(void);
uint32_t net_get_tcp_initial_seq(void);
port_t net_alloc_port(void);

/* Kept for callers that already tick the stack from a timer; harmless no-op
 * because this stack is fully synchronous/polled. */
void net_tick(void);

#ifdef __cplusplus
}
#endif

#endif /* STITCH_NET_H */
