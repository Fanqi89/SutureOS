/* SutureOS: minimal DNS resolver (A records only, one query at a time).
 *
 * It rides on the socket ABI that already exists in kernel/net/net.c
 * (net_socket/net_bind/net_sendto/net_recvfrom) - no second UDP stack, no
 * allocator, all buffers static. Nothing here runs at boot: the shell's
 * `nslookup <name>` command is the only caller, which is what keeps DHCP/DNS off
 * the frozen boot path.
 *
 * Honest limits: A records only (no AAAA/CNAME-chasing/MX), no EDNS0, no TCP
 * fallback, no cache, a single outstanding query, and the wire format is IPv4.
 */
#ifndef SUTUREOS_DNS_H
#define SUTUREOS_DNS_H

#include <stitch/types.h>

#define DNS_PORT            53
#define DNS_MAX_NAME        253
#define DNS_HEADER_LEN      12

/* Build an A/IN query for `name` into `buf`. Returns the packet length, or -1
 * when the name is not a valid dotted name or the buffer is too small. Pure: no
 * I/O, so the boot self test can check it without touching the network. */
int dns_build_query(const char *name, uint16_t id, uint8_t *buf, uint32_t buflen);

/* Parse a response for query `id`; on success stores the first A record's
 * address (host byte order) in *out_ip and returns 0. Returns a negative errno
 * (-EPROTO for a malformed/lame answer, -ENOENT when there is no A record).
 * Pure. */
int dns_parse_response(const uint8_t *buf, uint32_t len, uint16_t id, uint32_t *out_ip);

/* Resolve `name` against the configured DNS server (net_dns_ip). Bounded: at most
 * DNS_MAX_ATTEMPTS datagrams, each waiting DNS_TIMEOUT_MS. Returns 0 and stores
 * the address, or a negative errno:
 *   -EINVAL   bad name
 *   -ENETDOWN no NIC / no DNS server configured
 *   -ENOSPC   no free socket slot
 *   -EAGAIN   the server never answered
 *   -EPROTO / -ENOENT from dns_parse_response()
 * `out_ip` may not be NULL. */
int dns_resolve(const char *name, uint32_t *out_ip, int timeout_ms);

/* Silent-on-success unit test: query builder byte layout + response parser
 * against a hand-built answer. Prints `[dns] selftest FAIL ...` only on failure
 * and returns the number of failures. */
int dns_selftest(void);

#endif /* SUTUREOS_DNS_H */
