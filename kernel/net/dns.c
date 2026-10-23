/* SutureOS: minimal DNS resolver. See dns.h for scope and limits.
 *
 * Wire format (RFC 1035):
 *   header  : id, flags, qdcount, ancount, nscount, arcount   (12 bytes)
 *   question: QNAME (length-prefixed labels, 0 terminator), QTYPE, QCLASS
 *   answer  : NAME, TYPE, CLASS, TTL, RDLENGTH, RDATA
 *
 * A record: TYPE=1, CLASS=1, RDLENGTH=4, RDATA = the IPv4 address in network
 * byte order. Names in an answer may be a compression pointer (top two bits set)
 * into the packet, which is why the name skipper resolves pointers instead of
 * assuming literal labels.
 */
#include "dns.h"
#include "net.h"

#include <stitch/console.h>
#include <stitch/string.h>

#define DNS_TYPE_A      1
#define DNS_CLASS_IN    1
#define DNS_FLAG_RD     0x0100u     /* recursion desired                        */
#define DNS_FLAG_QR     0x8000u
#define DNS_RCODE_MASK  0x000Fu
#define DNS_MAX_ATTEMPTS 2
#define DNS_TIMEOUT_MS   1000

static uint16_t dns_rd16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static void dns_wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFFu);
}

int dns_build_query(const char *name, uint16_t id, uint8_t *buf, uint32_t buflen)
{
    uint32_t pos = DNS_HEADER_LEN;
    uint32_t name_len = 0;

    if (buf == NULL || name == NULL || buflen < DNS_HEADER_LEN + 5u)
        return -1;
    if (*name == '\0')
        return -1;

    /* Header: standard query, recursion desired, one question. */
    dns_wr16(buf + 0, id);
    dns_wr16(buf + 2, DNS_FLAG_RD);
    dns_wr16(buf + 4, 1);                    /* qdcount */
    dns_wr16(buf + 6, 0);                    /* ancount */
    dns_wr16(buf + 8, 0);                    /* nscount */
    dns_wr16(buf + 10, 0);                   /* arcount */

    /* QNAME: one length-prefixed label per dot-separated component. */
    for (;;) {
        uint32_t label_len = 0;
        while (name[label_len] != '\0' && name[label_len] != '.')
            label_len++;
        if (label_len == 0u)
            return -1;                       /* empty label ("a..b" / leading .) */
        if (label_len > 63u)
            return -1;
        name_len += label_len + 1u;
        if (name_len > DNS_MAX_NAME)
            return -1;
        if (pos + 1u + label_len + 5u > buflen)   /* label + qtype/qclass + 0 */
            return -1;
        buf[pos++] = (uint8_t)label_len;
        memcpy(buf + pos, name, label_len);
        pos += label_len;
        name += label_len;
        if (*name == '.') {
            name++;
            continue;                        /* a trailing dot ends the name */
        }
        break;
    }
    if (pos + 5u > buflen)
        return -1;
    buf[pos++] = 0u;                         /* root label */
    dns_wr16(buf + pos, DNS_TYPE_A);  pos += 2;
    dns_wr16(buf + pos, DNS_CLASS_IN); pos += 2;
    return (int)pos;
}

/* Skip one (possibly compressed) name. Returns the offset just past it, or 0
 * when the packet is malformed / a pointer loops. */
static uint32_t dns_skip_name(const uint8_t *buf, uint32_t len, uint32_t pos)
{
    uint32_t guard = 0;

    while (pos < len) {
        const uint8_t c = buf[pos];
        if ((c & 0xC0u) == 0xC0u) {          /* compression pointer: 2 bytes */
            if (pos + 1u >= len)
                return 0;
            return pos + 2u;
        }
        if (c == 0u)
            return pos + 1u;
        if ((c & 0xC0u) != 0u)
            return 0;                        /* 0x40/0x80 are reserved */
        pos += 1u + (uint32_t)c;
        if (++guard > 128u)
            return 0;
    }
    return 0;
}

int dns_parse_response(const uint8_t *buf, uint32_t len, uint16_t id, uint32_t *out_ip)
{
    uint32_t pos;
    uint16_t flags, ancount, i;

    if (buf == NULL || out_ip == NULL || len < DNS_HEADER_LEN)
        return -EPROTO;
    if (dns_rd16(buf) != id)
        return -EPROTO;                      /* not our transaction */
    flags = dns_rd16(buf + 2);
    if ((flags & DNS_FLAG_QR) == 0u)
        return -EPROTO;                      /* a query, not a response */
    if ((flags & DNS_RCODE_MASK) != 0u)
        return -EPROTO;                      /* NXDOMAIN / SERVFAIL / ... */
    /* qdcount is not used: this resolver always sends exactly one question and
     * only ever parses its own answers, so question 1 is the only one present. */
    ancount = dns_rd16(buf + 6);

    pos = dns_skip_name(buf, len, DNS_HEADER_LEN);
    if (pos == 0u || pos + 4u > len)
        return -EPROTO;
    pos += 4u;                               /* QTYPE + QCLASS of question 1 */

    for (i = 0; i < ancount; i++) {
        uint16_t type, class_, rdlen;
        pos = dns_skip_name(buf, len, pos);
        if (pos == 0u || pos + 10u > len)
            return -EPROTO;
        type  = dns_rd16(buf + pos);
        class_ = dns_rd16(buf + pos + 2);
        rdlen = dns_rd16(buf + pos + 8);
        pos += 10u;
        if (pos + rdlen > len)
            return -EPROTO;
        if (type == DNS_TYPE_A && class_ == DNS_CLASS_IN && rdlen == 4u) {
            *out_ip = ((uint32_t)buf[pos] << 24) | ((uint32_t)buf[pos + 1] << 16) |
                      ((uint32_t)buf[pos + 2] << 8) | (uint32_t)buf[pos + 3];
            return 0;
        }
        pos += rdlen;
    }
    return -ENOENT;
}

int dns_resolve(const char *name, uint32_t *out_ip, int timeout_ms)
{
    uint8_t  query[512];
    uint8_t  answer[512];
    uint16_t id;
    int      qlen;
    int      fd;
    int      attempt;
    net_sockaddr_t server;

    if (name == NULL || out_ip == NULL || *name == '\0')
        return -EINVAL;
    if (!net_ready() || net_dns_ip == 0u)
        return -ENETDOWN;
    if (timeout_ms <= 0)
        timeout_ms = DNS_TIMEOUT_MS;

    qlen = dns_build_query(name, (uint16_t)(net_now_us() & 0xFFFFu), query,
                           (uint32_t)sizeof(query));
    if (qlen < 0)
        return -EINVAL;
    id = dns_rd16(query);

    fd = net_socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return -ENOSPC;
    /* Ephemeral source port: bind() with port 0 picks one. */
    memset(&server, 0, sizeof(server));
    server.sin_family = AF_INET;
    if (net_bind(fd, &server, (socklen_t)sizeof(server)) != 0) {
        (void)net_close(fd);
        return -ENOSPC;
    }

    memset(&server, 0, sizeof(server));
    server.sin_family = AF_INET;
    server.sin_port   = htons(DNS_PORT);
    server.sin_addr   = net_dns_ip;

    for (attempt = 0; attempt < DNS_MAX_ATTEMPTS; attempt++) {
        int n;
        if (net_sendto(fd, query, (size_t)qlen, 0, &server,
                       (socklen_t)sizeof(server)) != qlen)
            continue;
        n = net_recvfrom(fd, answer, sizeof(answer), 0, NULL, NULL);
        if (n > 0) {
            const int r = dns_parse_response(answer, (uint32_t)n, id, out_ip);
            if (r == 0) {
                (void)net_close(fd);
                return 0;
            }
            if (r == -EPROTO || r == -ENOENT) {
                (void)net_close(fd);
                return r;                    /* a real answer, just not an A */
            }
        }
    }
    (void)net_close(fd);
    return -EAGAIN;
}

/* ===================== silent-on-success self test ===================== */
int dns_selftest(void)
{
    static const uint8_t qname[] = { 7, 'e','x','a','m','p','l','e', 3, 'c','o','m', 0 };
    uint8_t buf[128];
    uint8_t resp[128];
    uint32_t ip = 0;
    uint32_t pos;
    int len;
    int fails = 0;

    /* 1. builder: header, label encoding and the A/IN question. */
    len = dns_build_query("example.com", 0x1234u, buf, (uint32_t)sizeof(buf));
    if (len != (int)(DNS_HEADER_LEN + sizeof(qname) + 4u)) {
        console_printf("[dns] selftest FAIL builder length %d\n", (int64_t)len);
        fails++;
    } else {
        int ok = dns_rd16(buf) == 0x1234u && dns_rd16(buf + 2) == DNS_FLAG_RD &&
                 dns_rd16(buf + 4) == 1u && dns_rd16(buf + 6) == 0u &&
                 memcmp(buf + DNS_HEADER_LEN, qname, sizeof(qname)) == 0 &&
                 dns_rd16(buf + DNS_HEADER_LEN + sizeof(qname)) == DNS_TYPE_A &&
                 dns_rd16(buf + DNS_HEADER_LEN + sizeof(qname) + 2u) == DNS_CLASS_IN;
        if (!ok) {
            console_puts("[dns] selftest FAIL builder bytes\n");
            fails++;
        }
    }

    /* 2. builder rejects malformed names instead of emitting a broken packet. */
    if (dns_build_query("", 1u, buf, (uint32_t)sizeof(buf)) >= 0 ||
        dns_build_query("a..b", 1u, buf, (uint32_t)sizeof(buf)) >= 0 ||
        dns_build_query(".", 1u, buf, (uint32_t)sizeof(buf)) >= 0) {
        console_puts("[dns] selftest FAIL bad names accepted\n");
        fails++;
    }

    /* 3. parser: a hand-built answer with one A record 10.0.2.3. */
    memset(resp, 0, sizeof(resp));
    dns_wr16(resp + 0, 0x1234u);             /* id                          */
    dns_wr16(resp + 2, 0x8180u);             /* QR + RD + RA, rcode 0       */
    dns_wr16(resp + 4, 1);                   /* qdcount                     */
    dns_wr16(resp + 6, 1);                   /* ancount                     */
    pos = DNS_HEADER_LEN;
    memcpy(resp + pos, qname, sizeof(qname)); pos += sizeof(qname);
    dns_wr16(resp + pos, DNS_TYPE_A); pos += 2;
    dns_wr16(resp + pos, DNS_CLASS_IN); pos += 2;
    resp[pos++] = 0xC0; resp[pos++] = 0x0C;  /* name: pointer to the question */
    dns_wr16(resp + pos, DNS_TYPE_A); pos += 2;
    dns_wr16(resp + pos, DNS_CLASS_IN); pos += 2;
    resp[pos++] = 0; resp[pos++] = 0;        /* ttl 0.. */
    resp[pos++] = 0; resp[pos++] = 60;
    dns_wr16(resp + pos, 4); pos += 2;       /* rdlength */
    resp[pos++] = 10; resp[pos++] = 0; resp[pos++] = 2; resp[pos++] = 3;

    if (dns_parse_response(resp, pos, 0x1234u, &ip) != 0 || ip != 0x0A000203u) {
        console_puts("[dns] selftest FAIL parser answer\n");
        fails++;
    }
    /* a mismatched transaction id must be rejected, not answered */
    if (dns_parse_response(resp, pos, 0x9999u, &ip) != -EPROTO) {
        console_puts("[dns] selftest FAIL parser id check\n");
        fails++;
    }
    /* truncated answer must be rejected, not read out of bounds */
    if (dns_parse_response(resp, 20u, 0x1234u, &ip) != -EPROTO) {
        console_puts("[dns] selftest FAIL parser truncation\n");
        fails++;
    }
    return fails;
}
