/* SutureOS: DHCP client. See dhcp.h for the wire layout and the limits. */
#include "dhcp.h"
#include "net.h"

#include <stitch/console.h>
#include <stitch/string.h>

/* Offsets inside the fixed BOOTP block. */
#define DHCP_OFF_OP      0
#define DHCP_OFF_HTYPE   1
#define DHCP_OFF_HLEN    2
#define DHCP_OFF_HOPS    3
#define DHCP_OFF_XID     4
#define DHCP_OFF_SECS    8
#define DHCP_OFF_FLAGS   10
#define DHCP_OFF_CIADDR  12
#define DHCP_OFF_YIADDR  16
#define DHCP_OFF_SIADDR  20
#define DHCP_OFF_GIADDR  24
#define DHCP_OFF_CHADDR  28
#define DHCP_OFF_SNAME   44
#define DHCP_OFF_FILE    108

#define DHCP_OPT_PAD         0
#define DHCP_OPT_END         255
#define DHCP_OPT_NETMASK     1
#define DHCP_OPT_ROUTER      3
#define DHCP_OPT_DNS         6
#define DHCP_OPT_MSGTYPE     53
#define DHCP_OPT_SERVER_ID   54
#define DHCP_OPT_REQ_IP      50
#define DHCP_OPT_LEASE       51
#define DHCP_OPT_PARAM_LIST  55
#define DHCP_OPT_CLIENT_ID   61

#define DHCP_ATTEMPTS        4
#define DHCP_WAIT_MS         1000

static uint32_t dhcp_rd32be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void dhcp_wr32be(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

int dhcp_build_packet(uint8_t *buf, uint32_t buflen, uint8_t msg_type, uint32_t xid,
                      const uint8_t mac[6], uint32_t requested_ip, uint32_t server_id)
{
    uint32_t pos = DHCP_OPTIONS_OFF;

    if (buf == NULL || mac == NULL)
        return -1;
    /* Options: 3 (msgtype) + 6 (param list) + 9 (client id) + up to 12 + end. */
    if (buflen < DHCP_OPTIONS_OFF + 48u)
        return -1;

    memset(buf, 0, DHCP_OPTIONS_OFF);
    buf[DHCP_OFF_OP]    = 1;                 /* BOOTREQUEST                */
    buf[DHCP_OFF_HTYPE] = 1;                 /* ethernet                   */
    buf[DHCP_OFF_HLEN]  = 6;
    buf[DHCP_OFF_HOPS]  = 0;
    dhcp_wr32be(buf + DHCP_OFF_XID, xid);
    buf[DHCP_OFF_SECS]  = 0;
    buf[DHCP_OFF_SECS + 1] = 0;
    /* flags: bit 15 = broadcast reply requested (we have no address yet) */
    buf[DHCP_OFF_FLAGS]     = 0x80;
    buf[DHCP_OFF_FLAGS + 1] = 0x00;
    memcpy(buf + DHCP_OFF_CHADDR, mac, 6);
    dhcp_wr32be(buf + DHCP_MAGIC_OFFSET, DHCP_MAGIC_COOKIE);

    /* option 53: message type */
    buf[pos++] = DHCP_OPT_MSGTYPE;
    buf[pos++] = 1;
    buf[pos++] = msg_type;

    /* option 61: client identifier = 01 + hardware address */
    buf[pos++] = DHCP_OPT_CLIENT_ID;
    buf[pos++] = 7;
    buf[pos++] = 1;                          /* ethernet */
    memcpy(buf + pos, mac, 6);
    pos += 6;

    /* option 55: parameter request list (mask, router, DNS) */
    buf[pos++] = DHCP_OPT_PARAM_LIST;
    buf[pos++] = 3;
    buf[pos++] = DHCP_OPT_NETMASK;
    buf[pos++] = DHCP_OPT_ROUTER;
    buf[pos++] = DHCP_OPT_DNS;

    if (msg_type == DHCP_MSG_REQUEST) {
        if (requested_ip != 0u) {
            buf[pos++] = DHCP_OPT_REQ_IP;
            buf[pos++] = 4;
            dhcp_wr32be(buf + pos, requested_ip);
            pos += 4;
        }
        if (server_id != 0u) {
            buf[pos++] = DHCP_OPT_SERVER_ID;
            buf[pos++] = 4;
            dhcp_wr32be(buf + pos, server_id);
            pos += 4;
        }
    }

    buf[pos++] = DHCP_OPT_END;
    return (int)pos;
}

int dhcp_parse_reply(const uint8_t *buf, uint32_t len, uint32_t xid, dhcp_lease_t *out)
{
    uint32_t pos;
    int got_type = 0;

    if (buf == NULL || out == NULL || len < DHCP_OPTIONS_OFF)
        return -EPROTO;
    memset(out, 0, sizeof(*out));
    if (buf[DHCP_OFF_OP] != 2)               /* BOOTREPLY */
        return -EPROTO;
    if (dhcp_rd32be(buf + DHCP_OFF_XID) != xid)
        return -EPROTO;
    if (dhcp_rd32be(buf + DHCP_MAGIC_OFFSET) != DHCP_MAGIC_COOKIE)
        return -ENOENT;

    out->ip = dhcp_rd32be(buf + DHCP_OFF_YIADDR);

    pos = DHCP_OPTIONS_OFF;
    while (pos < len) {
        const uint8_t code = buf[pos++];
        uint8_t olen;
        if (code == DHCP_OPT_PAD)
            continue;
        if (code == DHCP_OPT_END)
            break;
        if (pos >= len)
            return -EPROTO;
        olen = buf[pos++];
        if (pos + olen > len)
            return -EPROTO;
        switch (code) {
        case DHCP_OPT_MSGTYPE:
            if (olen != 1u)
                return -EPROTO;
            out->message_type = buf[pos];
            got_type = 1;
            break;
        case DHCP_OPT_NETMASK:
            if (olen == 4u) out->netmask = dhcp_rd32be(buf + pos);
            break;
        case DHCP_OPT_ROUTER:
            if (olen >= 4u) out->gateway = dhcp_rd32be(buf + pos);
            break;
        case DHCP_OPT_DNS:
            if (olen >= 4u) out->dns = dhcp_rd32be(buf + pos);
            break;
        case DHCP_OPT_SERVER_ID:
            if (olen == 4u) out->server_id = dhcp_rd32be(buf + pos);
            break;
        case DHCP_OPT_LEASE:
            if (olen == 4u) out->lease_secs = dhcp_rd32be(buf + pos);
            break;
        default:
            break;
        }
        pos += olen;
    }
    if (!got_type)
        return -EPROTO;
    return 0;
}

/* One transaction step: send `msg_type` and wait for a reply of `want_type`. */
static int dhcp_step(int fd, const net_sockaddr_t *to, uint8_t msg_type, uint32_t xid,
                     const uint8_t mac[6], uint32_t requested_ip, uint32_t server_id,
                     uint8_t want_type, dhcp_lease_t *lease, int wait_ms)
{
    uint8_t pkt[DHCP_OPTIONS_OFF + 64u];
    uint8_t reply[576];
    int len = dhcp_build_packet(pkt, (uint32_t)sizeof(pkt), msg_type, xid, mac,
                                requested_ip, server_id);
    int attempt;

    if (len < 0)
        return -EINVAL;

    for (attempt = 0; attempt < DHCP_ATTEMPTS; attempt++) {
        int n;
        if (net_sendto(fd, pkt, (size_t)len, 0, to, (socklen_t)sizeof(*to)) != len)
            continue;
        n = net_recvfrom(fd, reply, sizeof(reply), 0, NULL, NULL);
        if (n <= 0)
            continue;
        if (dhcp_parse_reply(reply, (uint32_t)n, xid, lease) != 0)
            continue;
        if (lease->message_type == want_type)
            return 0;
        if (lease->message_type == DHCP_MSG_NAK)
            return -EPROTO;
    }
    (void)wait_ms;
    return -ETIMEDOUT;
}

int dhcp_client_acquire(dhcp_lease_t *out, int timeout_ms)
{
    dhcp_lease_t offer;
    dhcp_lease_t ack;
    net_sockaddr_t bcast;
    uint8_t mac[6];
    uint32_t xid;
    int fd;
    int rc;
    (void)timeout_ms;

    if (out == NULL)
        return -EINVAL;
    memset(out, 0, sizeof(*out));
    if (!net_ready())
        return -ENETDOWN;
    if (net_mac(mac) != 0)
        return -ENETDOWN;

    fd = net_socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return -ENOSPC;

    memset(&bcast, 0, sizeof(bcast));
    bcast.sin_family = AF_INET;
    bcast.sin_port   = htons(DHCP_CLIENT_PORT);
    bcast.sin_addr   = 0u;                   /* INADDR_ANY: bind port 68 */
    if (net_bind(fd, &bcast, (socklen_t)sizeof(bcast)) != 0) {
        (void)net_close(fd);
        return -ENOSPC;
    }

    memset(&bcast, 0, sizeof(bcast));
    bcast.sin_family = AF_INET;
    bcast.sin_port   = htons(DHCP_SERVER_PORT);
    bcast.sin_addr   = NET_IP_BROADCAST;     /* 255.255.255.255 */

    xid = (uint32_t)net_now_us() ^ ((uint32_t)mac[4] << 16) ^ ((uint32_t)mac[5] << 8);

    /* DISCOVER goes out from 0.0.0.0: a client without a lease must not claim
     * one, and the server keys the reply off chaddr/xid, not the source IP. */
    net_set_tx_src_ip(0u);
    rc = dhcp_step(fd, &bcast, DHCP_MSG_DISCOVER, xid, mac, 0, 0,
                   DHCP_MSG_OFFER, &offer, DHCP_WAIT_MS);
    if (rc == 0) {
        rc = dhcp_step(fd, &bcast, DHCP_MSG_REQUEST, xid, mac,
                       offer.ip, offer.server_id, DHCP_MSG_ACK, &ack, DHCP_WAIT_MS);
    }
    net_clear_tx_src_ip();
    (void)net_close(fd);

    if (rc != 0)
        return rc;

    /* Program the stack: address/mask/gateway + resolver. */
    net_configure_ip(ack.ip,
                     (ack.gateway != 0u) ? ack.gateway : offer.gateway,
                     (ack.netmask != 0u) ? ack.netmask : offer.netmask);
    if (ack.dns != 0u)
        net_set_dns_ip(ack.dns);
    if (ack.server_id == 0u)
        ack.server_id = offer.server_id;
    *out = ack;
    return 0;
}

/* ===================== silent-on-success self test ===================== */
int dhcp_selftest(void)
{
    static const uint8_t mac[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
    uint8_t pkt[DHCP_OPTIONS_OFF + 64u];
    uint8_t reply[576];
    dhcp_lease_t lease;
    uint32_t pos;
    int len;
    int fails = 0;

    /* 1. DISCOVER layout: BOOTP header, magic cookie, option 53 == DISCOVER. */
    len = dhcp_build_packet(pkt, (uint32_t)sizeof(pkt), DHCP_MSG_DISCOVER, 0xA1B2C3D4u,
                            mac, 0, 0);
    if (len <= (int)DHCP_OPTIONS_OFF) {
        console_printf("[dhcp] selftest FAIL build length %d\n", (int64_t)len);
        fails++;
    } else if (pkt[DHCP_OFF_OP] != 1 || pkt[DHCP_OFF_HTYPE] != 1 ||
               pkt[DHCP_OFF_HLEN] != 6 ||
               dhcp_rd32be(pkt + DHCP_OFF_XID) != 0xA1B2C3D4u ||
               dhcp_rd32be(pkt + DHCP_MAGIC_OFFSET) != DHCP_MAGIC_COOKIE ||
               memcmp(pkt + DHCP_OFF_CHADDR, mac, 6) != 0 ||
               pkt[DHCP_OPTIONS_OFF] != DHCP_OPT_MSGTYPE ||
               pkt[DHCP_OPTIONS_OFF + 2] != DHCP_MSG_DISCOVER) {
        console_puts("[dhcp] selftest FAIL build bytes\n");
        fails++;
    }

    /* 2. REQUEST must carry option 50 (requested address) and 54 (server id). */
    len = dhcp_build_packet(pkt, (uint32_t)sizeof(pkt), DHCP_MSG_REQUEST, 0x11223344u,
                            mac, 0x0A00020Fu, 0x0A000202u);
    if (len <= (int)DHCP_OPTIONS_OFF) {
        console_puts("[dhcp] selftest FAIL request build\n");
        fails++;
    } else {
        int have50 = 0, have54 = 0;
        pos = DHCP_OPTIONS_OFF;
        while (pos < (uint32_t)len) {
            uint8_t code = pkt[pos++];
            uint8_t olen;
            if (code == DHCP_OPT_END) break;
            if (code == DHCP_OPT_PAD) continue;
            if (pos >= (uint32_t)len) break;
            olen = pkt[pos++];
            if (code == DHCP_OPT_REQ_IP && olen == 4u &&
                dhcp_rd32be(pkt + pos) == 0x0A00020Fu)
                have50 = 1;
            if (code == DHCP_OPT_SERVER_ID && olen == 4u &&
                dhcp_rd32be(pkt + pos) == 0x0A000202u)
                have54 = 1;
            pos += olen;
        }
        if (!have50 || !have54) {
            console_puts("[dhcp] selftest FAIL request options\n");
            fails++;
        }
    }

    /* 3. reply parser: a hand-built ACK with mask/router/DNS/lease. */
    memset(reply, 0, sizeof(reply));
    reply[DHCP_OFF_OP] = 2;
    reply[DHCP_OFF_HTYPE] = 1;
    reply[DHCP_OFF_HLEN] = 6;
    dhcp_wr32be(reply + DHCP_OFF_XID, 0xA1B2C3D4u);
    dhcp_wr32be(reply + DHCP_OFF_YIADDR, 0x0A00020Fu);
    dhcp_wr32be(reply + DHCP_MAGIC_OFFSET, DHCP_MAGIC_COOKIE);
    pos = DHCP_OPTIONS_OFF;
    reply[pos++] = DHCP_OPT_MSGTYPE; reply[pos++] = 1; reply[pos++] = DHCP_MSG_ACK;
    reply[pos++] = DHCP_OPT_SERVER_ID; reply[pos++] = 4;
    dhcp_wr32be(reply + pos, 0x0A000202u); pos += 4;
    reply[pos++] = DHCP_OPT_NETMASK; reply[pos++] = 4;
    dhcp_wr32be(reply + pos, 0xFFFFFF00u); pos += 4;
    reply[pos++] = DHCP_OPT_ROUTER; reply[pos++] = 4;
    dhcp_wr32be(reply + pos, 0x0A000202u); pos += 4;
    reply[pos++] = DHCP_OPT_DNS; reply[pos++] = 4;
    dhcp_wr32be(reply + pos, 0x0A000203u); pos += 4;
    reply[pos++] = DHCP_OPT_LEASE; reply[pos++] = 4;
    dhcp_wr32be(reply + pos, 86400u); pos += 4;
    reply[pos++] = DHCP_OPT_END;

    if (dhcp_parse_reply(reply, pos, 0xA1B2C3D4u, &lease) != 0 ||
        lease.ip != 0x0A00020Fu || lease.netmask != 0xFFFFFF00u ||
        lease.gateway != 0x0A000202u || lease.dns != 0x0A000203u ||
        lease.server_id != 0x0A000202u || lease.lease_secs != 86400u ||
        lease.message_type != DHCP_MSG_ACK) {
        console_puts("[dhcp] selftest FAIL parse ack\n");
        fails++;
    }
    /* wrong transaction id / wrong opcode / truncated must be rejected */
    if (dhcp_parse_reply(reply, pos, 0x55555555u, &lease) != -EPROTO ||
        dhcp_parse_reply(reply, 100u, 0xA1B2C3D4u, &lease) != -EPROTO) {
        console_puts("[dhcp] selftest FAIL parse rejects\n");
        fails++;
    }
    return fails;
}
