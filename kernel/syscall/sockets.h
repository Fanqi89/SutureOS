/* SutureOS: the Linux socket syscalls. See sockets.c for the design notes.
 *
 * Numbers (identical to Linux x86-64, and to the SYS_* names in syscall.h):
 *   41 socket   42 connect  43 accept   44 sendto    45 recvfrom  46 sendmsg
 *   47 recvmsg  48 shutdown 49 bind     50 listen    51 getsockname
 *   52 getpeername          54 setsockopt           55 getsockopt
 * close(3) also closes a socket descriptor (the fd table has a socket kind).
 */
#ifndef SUTUREOS_SOCKETS_H
#define SUTUREOS_SOCKETS_H

#include <stitch/types.h>

uint64_t sys_socket(uint64_t domain, uint64_t type, uint64_t protocol,
                    uint64_t a4, uint64_t a5, uint64_t a6);
uint64_t sys_connect(uint64_t fd, uint64_t addr, uint64_t addrlen,
                     uint64_t a4, uint64_t a5, uint64_t a6);
uint64_t sys_accept(uint64_t fd, uint64_t addr, uint64_t addrlen,
                    uint64_t a4, uint64_t a5, uint64_t a6);
uint64_t sys_sendto(uint64_t fd, uint64_t buf, uint64_t len, uint64_t flags,
                    uint64_t addr, uint64_t addrlen);
uint64_t sys_recvfrom(uint64_t fd, uint64_t buf, uint64_t len, uint64_t flags,
                      uint64_t addr, uint64_t addrlen);
uint64_t sys_sendmsg(uint64_t fd, uint64_t msg_va, uint64_t flags,
                     uint64_t a4, uint64_t a5, uint64_t a6);
uint64_t sys_recvmsg(uint64_t fd, uint64_t msg_va, uint64_t flags,
                     uint64_t a4, uint64_t a5, uint64_t a6);
uint64_t sys_sock_shutdown(uint64_t fd, uint64_t how, uint64_t a3,
                           uint64_t a4, uint64_t a5, uint64_t a6);
uint64_t sys_bind(uint64_t fd, uint64_t addr, uint64_t addrlen,
                  uint64_t a4, uint64_t a5, uint64_t a6);
uint64_t sys_listen(uint64_t fd, uint64_t backlog, uint64_t a3,
                    uint64_t a4, uint64_t a5, uint64_t a6);
uint64_t sys_getsockname(uint64_t fd, uint64_t addr, uint64_t addrlen,
                         uint64_t a4, uint64_t a5, uint64_t a6);
uint64_t sys_getpeername(uint64_t fd, uint64_t addr, uint64_t addrlen,
                         uint64_t a4, uint64_t a5, uint64_t a6);
uint64_t sys_setsockopt(uint64_t fd, uint64_t level, uint64_t optname,
                        uint64_t optval, uint64_t optlen, uint64_t a6);
uint64_t sys_getsockopt(uint64_t fd, uint64_t level, uint64_t optname,
                        uint64_t optval, uint64_t optlen, uint64_t a6);

/* Boot-time acceptance for the ABI boundary (descriptor kind, bind/getsockname
 * round trip, getsockopt(SO_TYPE), family/type refusals, close()). Silent when
 * everything works; prints `[sock] selftest FAIL: ...` and returns the number of
 * failures otherwise. No network traffic, so it cannot perturb the boot log. */
int sockets_boot_selftest(void);

#endif /* SUTUREOS_SOCKETS_H */
