/* SutureOS: DHCP client (RFC 2131 DISCOVER/OFFER/REQUEST/ACK, BOOTP fields).
 *
 * On demand only: the shell command `dhcp` is the single caller, so nothing here
 * runs during boot and the frozen boot log keeps its lines. It reuses the UDP
 * socket ABI in kernel/net/net.c (net_socket/net_bind/net_sendto/net_recvfrom),
 * no second stack and no allocator.
 *
 * Wire shape (all multi-byte fields network order):
 *   0   op htype hlen hops            BOOTREQUEST=1, ethernet=1, hlen=6
 *   4   xid                            transaction id
 *   8   secs flags                     flags bit 15 = broadcast reply requested
 *   12  ciaddr yiaddr siaddr giaddr
 *   28  chaddr[16] sname[64] file[128]
 *   236 magic cookie 0x63825363
 *   240 options: 53 message type, 55 parameter request list, 61 client id,
 *       50 requested address (REQUEST), 54 server id (REQUEST), 255 end
 *
 * Honest limits: no lease renewal/T1/T2 timer, no RELEASE/DECLINE, no hostname
 * option, no relay (giaddr) support, a single transaction at a time, and only
 * the four options above are parsed.
 */
#ifndef SUTUREOS_DHCP_H
#define SUTUREOS_DHCP_H

#include <stitch/types.h>

#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68

#define DHCP_MSG_DISCOVER 1
#define DHCP_MSG_OFFER    2
#define DHCP_MSG_REQUEST  3
#define DHCP_MSG_ACK      5
#define DHCP_MSG_NAK      6

#define DHCP_FIXED_LEN    236
#define DHCP_MAGIC_OFFSET 236
#define DHCP_OPTIONS_OFF  240
#define DHCP_MAGIC_COOKIE 0x63825363u

/* What a completed handshake told us (host byte order addresses). */
typedef struct {
    uint32_t ip;            /* yiaddr                              */
    uint32_t netmask;       /* option 1                            */
    uint32_t gateway;       /* option 3                            */
    uint32_t dns;           /* option 6 (first address)            */
    uint32_t server_id;     /* option 54                           */
    uint32_t lease_secs;    /* option 51                           */
    uint8_t  message_type;  /* DHCP_MSG_* of the parsed packet     */
} dhcp_lease_t;

/* Build one BOOTP/DHCP packet into `buf` (>= DHCP_MIN_PACKET bytes). `xid` is the
 * transaction id, `mac` the client hardware address (6 bytes). `requested_ip` and
 * `server_id` are only emitted for DHCP_MSG_REQUEST (0 = omit). Returns the packet
 * length, or -1 when the buffer is too small. Pure: no I/O, so the boot self test
 * can check the byte layout without a network. */
int dhcp_build_packet(uint8_t *buf, uint32_t buflen, uint8_t msg_type, uint32_t xid,
                      const uint8_t mac[6], uint32_t requested_ip, uint32_t server_id);

/* Parse a DHCP reply (OFFER/ACK). Returns 0 and fills *out, or a negative errno:
 * -EPROTO malformed/not a DHCP reply, -ENOENT magic cookie missing. Pure. */
int dhcp_parse_reply(const uint8_t *buf, uint32_t len, uint32_t xid, dhcp_lease_t *out);

/* Run DISCOVER + REQUEST against 255.255.255.255:67 and program the result into
 * the stack (net_configure_ip + net_set_dns_ip). Returns 0 on a lease, or a
 * negative errno: -ENETDOWN no NIC, -ENOSPC no socket slot, -ETIMEDOUT no reply,
 * -EPROTO a NAK or a malformed reply. */
int dhcp_client_acquire(dhcp_lease_t *out, int timeout_ms);

/* Silent-on-success unit test: packet builder byte layout + reply parser against
 * a hand-built ACK. Prints `[dhcp] selftest FAIL ...` only on failure. */
int dhcp_selftest(void);

#endif /* SUTUREOS_DHCP_H */
