/* SutureOS module: Network stack - 移植自 SpiritFoxOS
 * Source: C:\Users\fanqi\Desktop\自研操作系统\SpiritFoxOS\kernel\include\net.h
 * Original license: GPL-3.0
 * Changes: Adapted to SutureOS; prefix net_; integrated with SutureOS VFS/block/sched
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

/* IPv4 address */
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

/* TCP unacknowledged segment (linked list) */
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

/* Socket structure */
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

/* Network configuration globals */
extern uint32_t net_local_ip;
extern uint32_t net_gateway_ip;
extern uint32_t net_netmask;
extern uint8_t  net_local_mac[6];
extern int      net_hw_initialized;

/* Public API */
int net_init(void);
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
int net_sendmsg(int fd, const struct msghdr *msg, int flags);
int net_recvmsg(int fd, struct msghdr *msg, int flags);
void net_configure_ip(uint32_t ip, uint32_t gateway, uint32_t netmask);
uint32_t net_get_tcp_initial_seq(void);
port_t net_alloc_port(void);
void net_tick(void);

#ifdef __cplusplus
}
#endif

#endif /* STITCH_NET_H */