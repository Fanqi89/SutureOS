/* SutureOS: the Linux socket syscalls (41..55) over the in-kernel socket ABI.
 *
 * There is already a socket object in kernel/net/net.c (net_socket/net_bind/
 * net_connect/net_listen/net_accept/net_send/net_recv/net_sendto/net_recvfrom/
 * net_shutdown/net_close + the TCP connection table in kernel/net/tcp.c). This
 * file is ONLY the ABI boundary: it validates every user pointer, converts the
 * Linux sockaddr_in (network byte order) to the kernel's net_sockaddr_t (host
 * order address, network order port), and installs the net socket in the
 * per-task descriptor table as a third descriptor kind next to VFS files and
 * pipe ends (see struct fd_entry in syscalls.c).
 *
 * Supported: AF_INET with SOCK_STREAM or SOCK_DGRAM. Every other family answers
 * -EAFNOSUPPORT, other types -ESOCKTNOSUPPORT, other protocols -EPROTONOSUPPORT:
 * the kernel has no AF_UNIX/AF_NETLINK transports, and pretending otherwise would
 * be a lie a libc would act on.
 *
 * Blocking behaviour: there is no wait queue yet, so a receive pumps net_poll()
 * and gives up after a bounded wall-clock budget (net.c:
 * NET_DGRAM_RECV_TIMEOUT_MS = 3000 ms for a datagram, 3000 ms inside tcp_recv()
 * for a stream) and then answers -EAGAIN. MSG_DONTWAIT turns a datagram receive
 * into a single poll with no wait. close() on a socket descriptor calls
 * net_close(), which also releases the TCP handle and the datagram slot.
 */
#include "sockets.h"

#include <stitch/console.h>
#include <stitch/string.h>
#include "el0_validate.h"
#include "net.h"
#include "syscall.h"

/* Linux x86-64 struct sockaddr_in: family, port, address, padding. The kernel
 * side (net_sockaddr_t in net.h) is the same 16 bytes but stores the address in
 * host byte order, so every crossing converts exactly once. */
typedef struct __attribute__((packed)) {
    uint16_t sin_family;
    uint16_t sin_port;
    uint32_t sin_addr;
    uint8_t  sin_zero[8];
} user_sockaddr_in_t;

_Static_assert(sizeof(user_sockaddr_in_t) == 16, "sockaddr_in must be 16 bytes");

/* Linux socket-level option numbers (asm-generic/socket.h). */
#define SOL_SOCKET_   1
#define SO_TYPE_      3
#define SO_ERROR_     4
#define SO_SNDBUF_    7
#define SO_RCVBUF_    8
#define SO_REUSEADDR_ 2

#define SOCK_STREAM_  1
#define SOCK_DGRAM_   2
#define IPPROTO_TCP_  6
#define IPPROTO_UDP_  17
#define SOCK_TYPE_MASK 0x0Fu          /* SOCK_NONBLOCK/SOCK_CLOEXEC live above */

static uint64_t rt_err(int e)
{
    return (uint64_t)(int64_t)(-e);
}

static int uok(uint64_t va, uint64_t len)
{
    return el0_ptr_ok(va, len);
}

/* Descriptor -> kernel net socket slot, or a negative errno. */
static int sock_from_fd(uint64_t fd)
{
    const int s = fd_sock_fd((int)fd);
    return (s >= 0) ? s : -EBADF;
}

static int addr_from_user(uint64_t va, uint64_t len, net_sockaddr_t *out)
{
    user_sockaddr_in_t in;

    if (out == NULL)
        return -EINVAL;
    if (va == 0)
        return -EFAULT;
    if (len < sizeof(in))
        return -EINVAL;
    if (!uok(va, sizeof(in)))
        return -EFAULT;
    memcpy(&in, (const void *)(uintptr_t)va, sizeof(in));
    if (in.sin_family != AF_INET)
        return -EAFNOSUPPORT;
    memset(out, 0, sizeof(*out));
    out->sin_family = AF_INET;
    out->sin_port   = in.sin_port;                    /* stays network order */
    out->sin_addr   = ntohl(in.sin_addr);             /* kernel: host order  */
    return 0;
}

/* Write a sockaddr_in back to user space: *addrlen is in/out, like Linux. */
static int addr_to_user(uint64_t va, uint64_t lenva, const net_sockaddr_t *in)
{
    user_sockaddr_in_t out;
    uint32_t have;

    if (va == 0 || lenva == 0)
        return -EFAULT;
    if (!uok(lenva, sizeof(uint32_t)))
        return -EFAULT;
    memcpy(&have, (const void *)(uintptr_t)lenva, sizeof(have));
    if (have < sizeof(out)) {
        const uint32_t need = (uint32_t)sizeof(out);
        memcpy((void *)(uintptr_t)lenva, &need, sizeof(need));
        return -EINVAL;
    }
    if (!uok(va, sizeof(out)))
        return -EFAULT;

    memset(&out, 0, sizeof(out));
    out.sin_family = AF_INET;
    out.sin_port   = in->sin_port;                    /* network order */
    out.sin_addr   = htonl(in->sin_addr);             /* wire order    */
    memcpy((void *)(uintptr_t)va, &out, sizeof(out));
    have = (uint32_t)sizeof(out);
    memcpy((void *)(uintptr_t)lenva, &have, sizeof(have));
    return 0;
}

/* ================================ socket(41) ================================ */
uint64_t sys_socket(uint64_t domain, uint64_t type, uint64_t protocol,
                    uint64_t a4, uint64_t a5, uint64_t a6)
{
    int t = (int)type & SOCK_TYPE_MASK;
    int s;
    int fd;
    (void)a4; (void)a5; (void)a6;

    if ((int)domain != AF_INET)
        return rt_err(EAFNOSUPPORT);
    if (t != SOCK_STREAM_ && t != SOCK_DGRAM_)
        return rt_err(ESOCKTNOSUPPORT);
    if ((int)protocol != 0 &&
        !(t == SOCK_STREAM_ && (int)protocol == IPPROTO_TCP_) &&
        !(t == SOCK_DGRAM_ && (int)protocol == IPPROTO_UDP_))
        return rt_err(EPROTONOSUPPORT);

    s = net_socket(AF_INET, t, (int)protocol);
    if (s < 0)
        return rt_err(-s);
    fd = fd_install_socket(s);
    if (fd < 0) {
        (void)net_close(s);
        return rt_err(EMFILE);
    }
    return (uint64_t)fd;
}

/* =============================== connect(42) ================================ */
uint64_t sys_connect(uint64_t fd, uint64_t addr, uint64_t addrlen,
                     uint64_t a4, uint64_t a5, uint64_t a6)
{
    net_sockaddr_t sa;
    int s = sock_from_fd(fd);
    int r;
    (void)a4; (void)a5; (void)a6;

    if (s < 0) return rt_err(-s);
    r = addr_from_user(addr, addrlen, &sa);
    if (r < 0) return rt_err(-r);
    r = net_connect(s, &sa, (socklen_t)sizeof(sa));
    return (r < 0) ? rt_err(-r) : 0;
}

/* ================================ accept(43) ================================ */
uint64_t sys_accept(uint64_t fd, uint64_t addr, uint64_t addrlen,
                    uint64_t a4, uint64_t a5, uint64_t a6)
{
    net_sockaddr_t peer;
    socklen_t peerlen = 0;
    int s = sock_from_fd(fd);
    int child, newfd, r;
    (void)a4; (void)a5; (void)a6;

    if (s < 0) return rt_err(-s);
    /* Validate the out-parameters before waiting for a connection. */
    if (addr != 0 && addrlen != 0 &&
        (!uok(addr, sizeof(user_sockaddr_in_t)) || !uok(addrlen, sizeof(uint32_t))))
        return rt_err(EFAULT);

    child = net_accept(s, &peer, &peerlen);
    if (child < 0)
        return rt_err(-child);
    newfd = fd_install_socket(child);
    if (newfd < 0) {
        (void)net_close(child);
        return rt_err(EMFILE);
    }
    if (addr != 0 && addrlen != 0) {
        r = addr_to_user(addr, addrlen, &peer);
        if (r < 0) {
            /* The connection is already accepted; report the descriptor and let
             * the (possibly too small) address buffer keep whatever it had. */
            (void)r;
        }
    }
    return (uint64_t)newfd;
}

/* ================================ sendto(44) ================================ */
uint64_t sys_sendto(uint64_t fd, uint64_t buf, uint64_t len, uint64_t flags,
                    uint64_t addr, uint64_t addrlen)
{
    net_sockaddr_t sa;
    int s = sock_from_fd(fd);
    int r;

    if (s < 0) return rt_err(-s);
    if (len != 0 && !uok(buf, len)) return rt_err(EFAULT);
    if (addr != 0) {
        r = addr_from_user(addr, addrlen, &sa);
        if (r < 0) return rt_err(-r);
    } else {
        memset(&sa, 0, sizeof(sa));
    }
    r = net_sendto(s, (const void *)(uintptr_t)buf, (size_t)len, (int)flags,
                   (addr != 0) ? &sa : NULL, (socklen_t)sizeof(sa));
    return (r < 0) ? rt_err(-r) : (uint64_t)r;
}

/* =============================== recvfrom(45) =============================== */
uint64_t sys_recvfrom(uint64_t fd, uint64_t buf, uint64_t len, uint64_t flags,
                      uint64_t addr, uint64_t addrlen)
{
    net_sockaddr_t from;
    socklen_t fromlen = 0;
    int s = sock_from_fd(fd);
    int r;

    if (s < 0) return rt_err(-s);
    if (len != 0 && !uok(buf, len)) return rt_err(EFAULT);
    if (addr != 0 && addrlen != 0 &&
        (!uok(addr, sizeof(user_sockaddr_in_t)) || !uok(addrlen, sizeof(uint32_t))))
        return rt_err(EFAULT);

    r = net_recvfrom(s, (void *)(uintptr_t)buf, (size_t)len, (int)flags,
                     (addr != 0) ? &from : NULL, (addr != 0) ? &fromlen : NULL);
    if (r < 0)
        return rt_err(-r);
    if (addr != 0 && addrlen != 0)
        (void)addr_to_user(addr, addrlen, &from);
    return (uint64_t)r;
}

/* =============================== sendmsg(46) ================================ */
/* The msghdr is copied field by field (the kernel must not follow user pointers
 * it has not validated). iovecs are copied in a bounded batch. */
#define MAX_IOV 8

uint64_t sys_sendmsg(uint64_t fd, uint64_t msg_va, uint64_t flags,
                     uint64_t a4, uint64_t a5, uint64_t a6)
{
    struct msghdr msg;
    struct iovec iov[MAX_IOV];
    net_sockaddr_t sa;
    int s = sock_from_fd(fd);
    int total = 0;
    uint64_t i;
    (void)a4; (void)a5; (void)a6;

    if (s < 0) return rt_err(-s);
    if (msg_va == 0) return rt_err(EFAULT);
    if (!uok(msg_va, sizeof(msg))) return rt_err(EFAULT);
    memcpy(&msg, (const void *)(uintptr_t)msg_va, sizeof(msg));

    if (msg.msg_iovlen > 0) {
        const size_t iov_bytes = (msg.msg_iovlen > MAX_IOV ? MAX_IOV : msg.msg_iovlen) *
                                 sizeof(struct iovec);
        if (msg.msg_iov == NULL) return rt_err(EFAULT);
        if (!uok((uint64_t)(uintptr_t)msg.msg_iov, iov_bytes)) return rt_err(EFAULT);
        memcpy(iov, msg.msg_iov, iov_bytes);
        for (i = 0; i < msg.msg_iovlen && i < MAX_IOV; i++) {
            int r;
            if (iov[i].iov_len != 0 && !uok((uint64_t)(uintptr_t)iov[i].iov_base,
                                            iov[i].iov_len))
                return rt_err(EFAULT);
            if (msg.msg_name != NULL) {
                const int ar = addr_from_user((uint64_t)(uintptr_t)msg.msg_name,
                                              msg.msg_namelen, &sa);
                if (ar < 0) return rt_err(-ar);
                r = net_sendto(s, iov[i].iov_base, iov[i].iov_len, (int)flags, &sa,
                               (socklen_t)sizeof(sa));
            } else {
                r = net_send(s, iov[i].iov_base, iov[i].iov_len, (int)flags);
            }
            if (r < 0) return rt_err(-r);
            total += r;
        }
    } else {
        total = 0;
    }
    return (uint64_t)total;
}

/* =============================== recvmsg(47) ================================ */
uint64_t sys_recvmsg(uint64_t fd, uint64_t msg_va, uint64_t flags,
                     uint64_t a4, uint64_t a5, uint64_t a6)
{
    struct msghdr msg;
    struct iovec iov[MAX_IOV];
    uint8_t staging[1472];
    net_sockaddr_t from;
    socklen_t fromlen = 0;
    int s = sock_from_fd(fd);
    uint32_t off;
    uint64_t i;
    int r;
    (void)a4; (void)a5; (void)a6;

    if (s < 0) return rt_err(-s);
    if (msg_va == 0) return rt_err(EFAULT);
    if (!uok(msg_va, sizeof(msg))) return rt_err(EFAULT);
    memcpy(&msg, (const void *)(uintptr_t)msg_va, sizeof(msg));

    if (msg.msg_iov == NULL || msg.msg_iovlen == 0)
        return rt_err(EINVAL);
    {
        const size_t n = (msg.msg_iovlen > MAX_IOV) ? MAX_IOV : msg.msg_iovlen;
        if (!uok((uint64_t)(uintptr_t)msg.msg_iov, n * sizeof(struct iovec)))
            return rt_err(EFAULT);
        memcpy(iov, msg.msg_iov, n * sizeof(struct iovec));
        for (i = 0; i < n; i++) {
            if (iov[i].iov_len != 0 && !uok((uint64_t)(uintptr_t)iov[i].iov_base,
                                            iov[i].iov_len))
                return rt_err(EFAULT);
            if (iov[i].iov_len > sizeof(staging))
                return rt_err(EINVAL);
        }
    }

    /* The kernel collects into one staging buffer and scatters below; msg_name is
     * an OUTPUT for recvmsg, so it is written only after the data arrives. */
    r = net_recvfrom(s, staging, sizeof(staging), (int)flags,
                     (msg.msg_name != NULL) ? &from : NULL,
                     (msg.msg_name != NULL) ? &fromlen : NULL);
    if (r < 0)
        return rt_err(-r);

    /* Scatter the received bytes across the caller's iovecs. */
    off = 0;
    for (i = 0; i < msg.msg_iovlen && i < MAX_IOV && off < (uint32_t)r; i++) {
        uint32_t n = (uint32_t)r - off;
        if (n > iov[i].iov_len)
            n = (uint32_t)iov[i].iov_len;
        if (n != 0 && iov[i].iov_base != NULL)
            memcpy(iov[i].iov_base, staging + off, n);
        off += n;
    }

    /* Report the source address (and its real length) back. */
    if (msg.msg_name != NULL) {
        if (msg.msg_namelen >= sizeof(user_sockaddr_in_t) &&
            uok((uint64_t)(uintptr_t)msg.msg_name, sizeof(user_sockaddr_in_t))) {
            user_sockaddr_in_t out;
            memset(&out, 0, sizeof(out));
            out.sin_family = AF_INET;
            out.sin_port   = from.sin_port;      /* already network order */
            out.sin_addr   = htonl(from.sin_addr);
            memcpy(msg.msg_name, &out, sizeof(out));
            msg.msg_namelen = (socklen_t)sizeof(out);
        } else {
            msg.msg_namelen = 0;
        }
    } else {
        msg.msg_namelen = 0;
    }
    msg.msg_flags = 0;
    memcpy((void *)(uintptr_t)msg_va, &msg, sizeof(msg));
    return (uint64_t)((off < (uint32_t)r) ? off : (uint32_t)r);
}

/* ============================== shutdown(48) ================================ */
/* Named sys_sock_shutdown, not sys_shutdown: kernel/gui/gui.c already owns
 * sys_shutdown() for the ACPI power-off sequence, and the GUI is frozen. */
uint64_t sys_sock_shutdown(uint64_t fd, uint64_t how, uint64_t a3,
                           uint64_t a4, uint64_t a5, uint64_t a6)
{
    int s = sock_from_fd(fd);
    int r;
    (void)a3; (void)a4; (void)a5; (void)a6;

    if (s < 0) return rt_err(-s);
    if (how > 2) return rt_err(EINVAL);
    r = net_shutdown(s, (int)how);
    return (r < 0) ? rt_err(-r) : 0;
}

/* ================================ bind(49) ================================== */
uint64_t sys_bind(uint64_t fd, uint64_t addr, uint64_t addrlen,
                  uint64_t a4, uint64_t a5, uint64_t a6)
{
    net_sockaddr_t sa;
    int s = sock_from_fd(fd);
    int r;
    (void)a4; (void)a5; (void)a6;

    if (s < 0) return rt_err(-s);
    r = addr_from_user(addr, addrlen, &sa);
    if (r < 0) return rt_err(-r);
    r = net_bind(s, &sa, (socklen_t)sizeof(sa));
    return (r < 0) ? rt_err(-r) : 0;
}

/* =============================== listen(50) ================================= */
uint64_t sys_listen(uint64_t fd, uint64_t backlog, uint64_t a3,
                    uint64_t a4, uint64_t a5, uint64_t a6)
{
    int s = sock_from_fd(fd);
    int r;
    (void)a3; (void)a4; (void)a5; (void)a6;

    if (s < 0) return rt_err(-s);
    r = net_listen(s, (int)backlog);
    if (r == -ENOSYS)
        return rt_err(EOPNOTSUPP);        /* datagram sockets cannot listen */
    return (r < 0) ? rt_err(-r) : 0;
}

/* ============================ getsockname(51) ============================== */
uint64_t sys_getsockname(uint64_t fd, uint64_t addr, uint64_t addrlen,
                         uint64_t a4, uint64_t a5, uint64_t a6)
{
    net_sockaddr_t sa;
    socklen_t salen = 0;
    int s = sock_from_fd(fd);
    int r;
    (void)a4; (void)a5; (void)a6;

    if (s < 0) return rt_err(-s);
    if (addr == 0 || addrlen == 0) return rt_err(EFAULT);
    r = net_getsockname(s, &sa, &salen);
    if (r < 0) return rt_err(-r);
    r = addr_to_user(addr, addrlen, &sa);
    return (r < 0) ? rt_err(-r) : 0;
}

/* ============================ getpeername(52) ============================== */
uint64_t sys_getpeername(uint64_t fd, uint64_t addr, uint64_t addrlen,
                         uint64_t a4, uint64_t a5, uint64_t a6)
{
    net_sockaddr_t sa;
    socklen_t salen = 0;
    int s = sock_from_fd(fd);
    int r;
    (void)a4; (void)a5; (void)a6;

    if (s < 0) return rt_err(-s);
    if (addr == 0 || addrlen == 0) return rt_err(EFAULT);
    r = net_getpeername(s, &sa, &salen);
    if (r < 0) return rt_err(-r);
    r = addr_to_user(addr, addrlen, &sa);
    return (r < 0) ? rt_err(-r) : 0;
}

/* ============================= setsockopt(54) ============================== */
uint64_t sys_setsockopt(uint64_t fd, uint64_t level, uint64_t optname,
                        uint64_t optval, uint64_t optlen, uint64_t a6)
{
    int s = sock_from_fd(fd);
    (void)a6;

    if (s < 0) return rt_err(-s);
    if (optlen != 0 && optval != 0 && !uok(optval, optlen)) return rt_err(EFAULT);
    if ((int)level != SOL_SOCKET_)
        return rt_err(ENOPROTOOPT);       /* no protocol-level options exist */
    switch ((int)optname) {
    case SO_REUSEADDR_:
    case SO_SNDBUF_:
    case SO_RCVBUF_:
        /* Accepted and remembered by nothing: the stack has one TX/RX buffer per
         * socket and no TIME_WAIT reuse problem to solve yet. Saying 0 here is
         * honest for these three (they only tune behaviour we do not vary), and
         * net_setsockopt() is still the single place that would change. */
        return (uint64_t)(int64_t)net_setsockopt(s, (int)level, (int)optname,
                                                 (const void *)(uintptr_t)optval,
                                                 (socklen_t)optlen);
    default:
        return rt_err(ENOPROTOOPT);
    }
}

/* ============================= getsockopt(55) ============================== */
uint64_t sys_getsockopt(uint64_t fd, uint64_t level, uint64_t optname,
                        uint64_t optval, uint64_t optlen, uint64_t a6)
{
    int s = sock_from_fd(fd);
    int32_t value;
    uint32_t have;
    (void)optval; (void)a6;

    if (s < 0) return rt_err(-s);
    if (optval == 0 || optlen == 0) return rt_err(EFAULT);
    if (!uok(optlen, sizeof(uint32_t))) return rt_err(EFAULT);
    if ((int)level != SOL_SOCKET_)
        return rt_err(ENOPROTOOPT);

    switch ((int)optname) {
    case SO_TYPE_: {
        const int t = net_socket_type(s);
        if (t < 0) return rt_err(-t);
        value = (int32_t)t;
        break;
    }
    case SO_ERROR_:
        value = 0;                        /* no asynchronous socket errors */
        break;
    case SO_SNDBUF_:
        value = (int32_t)NET_TX_BUF_SIZE;
        break;
    case SO_RCVBUF_:
        value = (int32_t)NET_RX_BUF_SIZE;
        break;
    case SO_REUSEADDR_:
        value = 0;
        break;
    default:
        return rt_err(ENOPROTOOPT);
    }

    memcpy(&have, (const void *)(uintptr_t)optlen, sizeof(have));
    if (have < sizeof(value)) {
        const uint32_t need = (uint32_t)sizeof(value);
        memcpy((void *)(uintptr_t)optlen, &need, sizeof(need));
        return rt_err(EINVAL);
    }
    if (!uok(optval, sizeof(value))) return rt_err(EFAULT);
    memcpy((void *)(uintptr_t)optval, &value, sizeof(value));
    have = (uint32_t)sizeof(value);
    memcpy((void *)(uintptr_t)optlen, &have, sizeof(have));
    return 0;
}

/* ===================== silent-on-success acceptance =====================
 * Exercises the whole ABI boundary with real objects but without the wire:
 * socket() returns a descriptor, bind()+getsockname() round-trip a loopback
 * address through the byte-order conversion, getsockopt(SO_TYPE) reports the
 * type that was asked for, an unsupported family is refused with -EAFNOSUPPORT,
 * and close() releases the descriptor. Prints nothing on success. */
int sockets_boot_selftest(void)
{
    static const char *g_fail[8];
    uint32_t nfail = 0;
    user_sockaddr_in_t sin;
    uint32_t slen;
    int32_t opt;
    int fd = -1;
    uint32_t i;
    int want;
    int ok;

    for (i = 0; i < 8; i++) g_fail[i] = NULL;

    /* socket(AF_INET, SOCK_DGRAM) must return an ordinary descriptor. */
    ok = (int)syscall_invoke(SYS_SOCKET, AF_INET, SOCK_DGRAM, 0, 0, 0, 0);
    if (ok < 0) {
        if (nfail < 8) g_fail[nfail] = "socket(AF_INET, SOCK_DGRAM) answered an error";
        nfail++;
    } else {
        fd = ok;
    }

    if (fd >= 0) {
        /* bind + getsockname round trip through the user-space sockaddr_in. */
        memset(&sin, 0, sizeof(sin));
        sin.sin_family = AF_INET;
        sin.sin_port = htons(40001);
        sin.sin_addr = htonl(INADDR_LOOPBACK);
        ok = (int)syscall_invoke(SYS_BIND, (uint64_t)fd, (uint64_t)(uintptr_t)&sin,
                                 (uint64_t)sizeof(sin), 0, 0, 0);
        if (ok != 0) {
            if (nfail < 8) g_fail[nfail] = "bind(127.0.0.1:40001) failed";
            nfail++;
        } else {
            memset(&sin, 0, sizeof(sin));
            slen = (uint32_t)sizeof(sin);
            ok = (int)syscall_invoke(SYS_GETSOCKNAME, (uint64_t)fd,
                                     (uint64_t)(uintptr_t)&sin,
                                     (uint64_t)(uintptr_t)&slen, 0, 0, 0);
            if (ok != 0 || slen != (uint32_t)sizeof(sin) ||
                sin.sin_family != AF_INET || ntohs(sin.sin_port) != 40001 ||
                ntohl(sin.sin_addr) != INADDR_LOOPBACK) {
                if (nfail < 8)
                    g_fail[nfail] = "bind/getsockname round trip mismatch";
                nfail++;
            }
        }

        /* getsockopt(SO_TYPE) must report SOCK_DGRAM. */
        opt = 0;
        want = (int32_t)sizeof(opt);
        ok = (int)syscall_invoke(SYS_GETSOCKOPT, (uint64_t)fd, SOL_SOCKET_,
                                 SO_TYPE_, (uint64_t)(uintptr_t)&opt,
                                 (uint64_t)(uintptr_t)&want, 0);
        if (ok != 0 || opt != SOCK_DGRAM) {
            if (nfail < 8) g_fail[nfail] = "getsockopt(SO_TYPE) wrong";
            nfail++;
        }

        /* An unknown option is refused instead of silently accepted. */
        opt = 0;
        want = (int32_t)sizeof(opt);
        if ((int)syscall_invoke(SYS_GETSOCKOPT, (uint64_t)fd, SOL_SOCKET_, 0x7FFF,
                                (uint64_t)(uintptr_t)&opt,
                                (uint64_t)(uintptr_t)&want, 0) != -ENOPROTOOPT) {
            if (nfail < 8) g_fail[nfail] = "getsockopt(unknown) not -ENOPROTOOPT";
            nfail++;
        }

        /* close() must release the descriptor and the net socket slot. */
        ok = (int)syscall_invoke(SYS_CLOSE, (uint64_t)fd, 0, 0, 0, 0, 0);
        if (ok != 0 || fd_is_open(fd)) {
            if (nfail < 8) g_fail[nfail] = "close() left the socket descriptor open";
            nfail++;
        }
    }

    /* Other address families are refused, never faked. */
    if ((int)syscall_invoke(SYS_SOCKET, 1 /* AF_UNIX */, SOCK_STREAM, 0, 0, 0, 0)
        != -EAFNOSUPPORT) {
        if (nfail < 8) g_fail[nfail] = "socket(AF_UNIX) not -EAFNOSUPPORT";
        nfail++;
    }
    /* Other socket types are refused too. */
    if ((int)syscall_invoke(SYS_SOCKET, AF_INET, 5 /* SOCK_SEQPACKET */, 0, 0, 0, 0)
        != -ESOCKTNOSUPPORT) {
        if (nfail < 8) g_fail[nfail] = "socket(SOCK_SEQPACKET) not -ESOCKTNOSUPPORT";
        nfail++;
    }
    /* A zero pointer on the datagram path must be refused, not dereferenced. */
    if ((int)syscall_invoke(SYS_BIND, 3, 0, (uint64_t)sizeof(sin), 0, 0, 0)
        != -EBADF) {
        if (nfail < 8) g_fail[nfail] = "bind() on a non-socket fd not -EBADF";
        nfail++;
    }

    for (i = 0; i < nfail && i < 8; i++)
        console_printf("[sock] selftest FAIL: %s\n", g_fail[i]);
    return (int)nfail;
}
