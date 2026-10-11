/* SutureOS module: Network stack stub - 移植自 SpiritFoxOS
 * Source: C:\Users\fanqi\Desktop\自研操作系统\SpiritFoxOS\kernel\src\net\*.c
 * Original license: GPL-3.0
 * Changes: Adapted to SutureOS; stub implementation for compilation
 */
#include "net.h"
#include <stitch/types.h>
#include <stitch/string.h>
#include <stitch/console.h>
#include "heap.h"

/* 全局状态 */
static net_socket_t g_sockets[NET_MAX_SOCKETS];
static int g_socket_used[NET_MAX_SOCKETS] = {0};
static uint8_t g_net_mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
static uint32_t g_net_ip = 0x0A00020F;  /* 10.0.2.15 */
static uint32_t g_net_gateway = 0x0A000202;  /* 10.0.2.2 */
static uint32_t g_net_netmask = 0xFFFFFF00;  /* 255.255.255.0 */
static int g_net_initialized = 0;
static port_t g_next_port = 49152;
static uint32_t g_tcp_seq = 0x12345678;

/* 网络配置导出 */
uint32_t net_local_ip = 0;
uint32_t net_gateway_ip = 0;
uint32_t net_netmask = 0;
uint8_t  net_local_mac[6] = {0};
int      net_hw_initialized = 0;

int net_init(void) {
    memset(g_sockets, 0, sizeof(g_sockets));
    memset(g_socket_used, 0, sizeof(g_socket_used));
    g_next_port = 49152;
    g_tcp_seq = 0x12345678;
    
    net_local_ip = g_net_ip;
    net_gateway_ip = g_net_gateway;
    net_netmask = g_net_netmask;
    memcpy(net_local_mac, g_net_mac, 6);
    net_hw_initialized = 1;
    g_net_initialized = 1;
    
    console_puts("[net] Network stack init (stub)\n");
    return 0;
}

int net_socket(int domain, int type, int protocol) {
    (void)domain; (void)type; (void)protocol;
    if (!g_net_initialized) net_init();
    
    for (int i = 0; i < NET_MAX_SOCKETS; i++) {
        if (!g_socket_used[i]) {
            g_socket_used[i] = 1;
            memset(&g_sockets[i], 0, sizeof(net_socket_t));
            g_sockets[i].fd = i;
            g_sockets[i].domain = domain;
            g_sockets[i].type = type;
            g_sockets[i].protocol = protocol;
            g_sockets[i].state = TCP_CLOSED;
            g_sockets[i].rx_size = NET_RX_BUF_SIZE;
            g_sockets[i].tx_size = NET_TX_BUF_SIZE;
            return i;
        }
    }
    return -1;
}

int net_bind(int fd, const net_sockaddr_t *addr, socklen_t addrlen) {
    (void)fd; (void)addr; (void)addrlen;
    return 0;
}

int net_listen(int fd, int backlog) {
    (void)fd; (void)backlog;
    return 0;
}

int net_accept(int fd, net_sockaddr_t *addr, socklen_t *addrlen) {
    (void)fd; (void)addr; (void)addrlen;
    return -1;
}

int net_connect(int fd, const net_sockaddr_t *addr, socklen_t addrlen) {
    (void)fd; (void)addr; (void)addrlen;
    return 0;
}

int net_send(int fd, const void *buf, size_t len, int flags) {
    (void)fd; (void)buf; (void)len; (void)flags;
    return len;
}

int net_recv(int fd, void *buf, size_t len, int flags) {
    (void)fd; (void)buf; (void)len; (void)flags;
    return 0;
}

int net_sendto(int fd, const void *buf, size_t len, int flags,
               const net_sockaddr_t *dest_addr, socklen_t addrlen) {
    (void)fd; (void)buf; (void)len; (void)flags; (void)dest_addr; (void)addrlen;
    return len;
}

int net_recvfrom(int fd, void *buf, size_t len, int flags,
                 net_sockaddr_t *src_addr, socklen_t *addrlen) {
    (void)fd; (void)buf; (void)len; (void)flags; (void)src_addr; (void)addrlen;
    return 0;
}

int net_shutdown(int fd, int how) {
    (void)fd; (void)how;
    return 0;
}

int net_close(int fd) {
    if (fd >= 0 && fd < NET_MAX_SOCKETS) {
        g_socket_used[fd] = 0;
        memset(&g_sockets[fd], 0, sizeof(net_socket_t));
    }
    return 0;
}

int net_setsockopt(int fd, int level, int optname, const void *optval, socklen_t optlen) {
    (void)fd; (void)level; (void)optname; (void)optval; (void)optlen;
    return 0;
}

int net_getsockopt(int fd, int level, int optname, void *optval, socklen_t *optlen) {
    (void)fd; (void)level; (void)optname; (void)optval; (void)optlen;
    return 0;
}

int net_getsockname(int fd, net_sockaddr_t *addr, socklen_t *addrlen) {
    (void)fd; (void)addr; (void)addrlen;
    return 0;
}

int net_getpeername(int fd, net_sockaddr_t *addr, socklen_t *addrlen) {
    (void)fd; (void)addr; (void)addrlen;
    return 0;
}

int net_sendmsg(int fd, const struct msghdr *msg, int flags) {
    (void)fd; (void)msg; (void)flags;
    return 0;
}

int net_recvmsg(int fd, struct msghdr *msg, int flags) {
    (void)fd; (void)msg; (void)flags;
    return 0;
}

void net_configure_ip(uint32_t ip, uint32_t gateway, uint32_t netmask) {
    net_local_ip = ip;
    net_gateway_ip = gateway;
    net_netmask = netmask;
}

uint32_t net_get_tcp_initial_seq(void) {
    return g_tcp_seq;
}

port_t net_alloc_port(void) {
    for (int i = 0; i < 16384; i++) {
        port_t candidate = g_next_port++;
        if (g_next_port > 65535) g_next_port = 49152;
        
        int in_use = 0;
        for (int j = 0; j < NET_MAX_SOCKETS; j++) {
            if (g_socket_used[j] && g_sockets[j].local_port == candidate) {
                in_use = 1;
                break;
            }
        }
        if (!in_use) return candidate;
    }
    return 0;
}

void net_tick(void) {
    /* Called from timer ISR - stub */
}

uint16_t htons(uint16_t v) { return ((v & 0xFF) << 8) | ((v >> 8) & 0xFF); }
uint32_t htonl(uint32_t v) { return ((v & 0xFF) << 24) | (((v >> 8) & 0xFF) << 16) | (((v >> 16) & 0xFF) << 8) | ((v >> 24) & 0xFF); }