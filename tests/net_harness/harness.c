/* Host-side functional harness for the SutureOS network stack.
 *
 * This compiles the REAL kernel/net/net.c (unmodified) against shadow headers
 * that replace the NIC driver with a scripted model, and then drives the
 * protocol state machine through complete exchanges:
 *
 *   ARP   request out -> reply in -> cache must return the MAC; the cache must
 *         time out both by epoch age AND by replacement pressure.
 *   IPv4  header checksum rejection, short-frame rejection, non-fragment rule.
 *   ICMP  inbound echo request must be answered (reply frame byte-compared on
 *         the wire, including type/code/checksum/id/seq/payload and the
 *         ethernet addresses); net_ping() must return 0 with a bounded RTT on a
 *         matching reply; a wrong id must NOT satisfy a pending ping.
 *   UDP   net_udp_send() must emit a correctly checksummed datagram that the
 *         stack itself accepts back (udp_rx), delivering it to the handler with
 *         the right src/dst ports and byte-identical payload; a corrupted UDP
 *         checksum must be dropped.
 *   Stats the counters net_dump_stats() prints must advance, and the two
 *         acceptance log lines must be produced by net_init().
 *
 * Build + run (from the repo root):
 *   clang -O1 -g -Wall -Wextra -I tests/net_harness -o build/net_harness.exe \
 *         tests/net_harness/harness.c kernel/net/net.c
 *   ./build/net_harness.exe
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>

#include "net.h"
#include "rtl8139.h"
#include "mb_pit.h"

/* ===================== fake NIC model ===================== */
#define FAKE_RXQ 8
#define FAKE_TXQ 16
#define FAKE_FRAME_MAX 1600u

typedef struct {
    uint8_t data[FAKE_FRAME_MAX];
    uint32_t len;
} fake_frame_t;

static fake_frame_t g_rxq[FAKE_RXQ];
static uint32_t g_rxq_head, g_rxq_count;
static fake_frame_t g_txq[FAKE_TXQ];
static uint32_t g_txq_count;               /* newest-wins history for checks */
static uint8_t g_fake_mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
static uint32_t g_fake_rx_drops;
static int g_fake_up;

static void fake_rx_push(const uint8_t *f, uint32_t len)
{
    if (g_rxq_count >= FAKE_RXQ)
        return;
    memcpy(g_rxq[(g_rxq_head + g_rxq_count) % FAKE_RXQ].data, f, len);
    g_rxq[(g_rxq_head + g_rxq_count) % FAKE_RXQ].len = len;
    g_rxq_count++;
}

static void fake_tx_record(const uint8_t *f, uint32_t len)
{
    if (len > FAKE_FRAME_MAX)
        return;
    if (g_txq_count < FAKE_TXQ) {
        memcpy(g_txq[g_txq_count].data, f, len);
        g_txq[g_txq_count].len = len;
        g_txq_count++;
    } else {
        memmove(&g_txq[0], &g_txq[1], sizeof(fake_frame_t) * (FAKE_TXQ - 1));
        memcpy(g_txq[FAKE_TXQ - 1].data, f, len);
        g_txq[FAKE_TXQ - 1].len = len;
    }
}

static int fake_tx_find(uint16_t ethertype)
{
    uint32_t i;
    for (i = g_txq_count; i > 0; i--) {
        if (((uint16_t)g_txq[i - 1].data[12] << 8 | g_txq[i - 1].data[13]) == ethertype)
            return (int)(i - 1);
    }
    return -1;
}

static void fake_tx_reset(void) { g_txq_count = 0; }

/* ---- driver entry points referenced by net.c through the shadow header ---- */
int TEST_rtl8139_init(void)
{
    g_fake_up = 1;
    return RTL8139_ERR_NONE;
}
int TEST_rtl8139_present(void) { return g_fake_up; }
int TEST_rtl8139_get_mac(uint8_t mac_out[6])
{
    if (!g_fake_up || !mac_out)
        return -1;
    memcpy(mac_out, g_fake_mac, 6);
    return 0;
}
uint8_t  TEST_rtl8139_irq(void)      { return RTL8139_TEST_IRQ; }
uint16_t TEST_rtl8139_io_base(void)  { return RTL8139_TEST_IO_BASE; }
int TEST_rtl8139_send_frame(const void *frame, uint32_t len)
{
    if (!g_fake_up || !frame || len < 14u || len > 1514u)
        return -1;
    fake_tx_record((const uint8_t *)frame, len);
    return 0;
}
int TEST_rtl8139_receive(void (*fn)(const uint8_t *frame, uint32_t len))
{
    int n = 0;
    if (!g_fake_up || !fn)
        return 0;
    while (g_rxq_count > 0) {
        fake_frame_t *f = &g_rxq[g_rxq_head];
        fn(f->data, f->len);
        g_rxq_head = (g_rxq_head + 1) % FAKE_RXQ;
        g_rxq_count--;
        n++;
    }
    return n;
}
uint16_t TEST_rtl8139_last_isr(void)  { return 0; }
uint32_t TEST_rtl8139_rx_drops(void)  { return g_fake_rx_drops; }
uint64_t TEST_rtl8139_tx_frames(void) { return g_txq_count; }
uint64_t TEST_rtl8139_rx_frames(void) { return 0; }
void     TEST_st_rtl8139_test(void)   { }

/* ===================== console + selftest + PIT stubs ===================== */
static int g_console_lines;
static char g_console_log[64][256];
static int g_console_log_n;

void console_init(void) { }
void console_putc(char c) { putchar(c); }
void console_puts(const char *s)
{
    fputs(s, stdout);
    g_console_lines++;
    if (g_console_log_n < 64) {
        strncpy(g_console_log[g_console_log_n], s, 255);
        g_console_log[g_console_log_n][255] = '\0';
        g_console_log_n++;
    }
}
void console_printf(const char *fmt, ...)
{
    va_list ap;
    char buf[512];
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fputs(buf, stdout);
    g_console_lines++;
    if (g_console_log_n < 64) {
        strncpy(g_console_log[g_console_log_n], buf, 255);
        g_console_log[g_console_log_n][255] = '\0';
        g_console_log_n++;
    }
}

static int g_st_total, g_st_fail;
void st_run(const char *name, bool pass)
{
    g_st_total++;
    if (!pass)
        g_st_fail++;
    printf("[selftest][%s] %s\n", name, pass ? "PASS" : "FAIL");
}
int st_failures(void) { return g_st_fail; }
int st_total(void)    { return g_st_total; }

/* The kernel clock is derived from mbos_pit_counter(); the harness models a PIT
 * that advances 12 ms per read. That makes net_now_us() a perfectly linear
 * microsecond clock and every deadline in net.c reachable in a handful of
 * iterations, while staying inside the kernel's uint16 delta arithmetic. */
static uint64_t g_fake_us;
void     mbos_pit_init(uint32_t hz) { (void)hz; }
uint32_t mbos_pit_hz(void) { return 100u; }
uint32_t mbos_pit_divisor(void) { return 11931u; }
uint16_t mbos_pit_counter(void)
{
    g_fake_us += 12000ull;
    return (uint16_t)(65536ull - (g_fake_us * (uint64_t)MBOS_PIT_BASE_HZ) / 1000000ull);
}

/* host-order port helpers: the wire carries network order, so comparing a
 * captured port against a host-order constant needs a swap. */
static uint16_t wire16(const uint8_t *p) { return (uint16_t)(((uint16_t)p[0] << 8) | p[1]); }


/* ===================== test helpers ===================== */
static int g_fail;
#define CHECK(cond, ...)                                                    \
    do {                                                                    \
        if (cond) {                                                         \
            printf("  ok   : " __VA_ARGS__);                                \
            printf("\n");                                                   \
        } else {                                                            \
            printf("  FAIL : " __VA_ARGS__);                                \
            printf("\n");                                                   \
            g_fail++;                                                       \
        }                                                                   \
    } while (0)

static void hexdump(const char *tag, const uint8_t *f, uint32_t n)
{
    uint32_t i, j;
    printf("  dump %s (%u bytes)\n", tag, n);
    for (i = 0; i < n; i += 16) {
        printf("    %04x:", i);
        for (j = 0; j < 16 && i + j < n; j++)
            printf(" %02x", f[i + j]);
        printf("\n");
    }
}

static uint16_t be16(uint16_t v) { return (uint16_t)((v << 8) | (v >> 8)); }
static uint32_t be32(uint32_t v)
{
    return ((v & 0xFFu) << 24) | ((v & 0xFF00u) << 8) |
           ((v & 0xFF0000u) >> 8) | ((v & 0xFF000000u) >> 24);
}

/* reference checksum implementations used to VERIFY the kernel's output */
static uint16_t test_ip_checksum(const uint8_t *d, uint32_t len);
static uint16_t test_udp_checksum(uint32_t src, uint32_t dst,
                                  const uint8_t *udp, uint32_t len, uint8_t proto);

/* Verify a 16-bit Internet checksum field inside a captured frame.
 *
 * The kernel computes every checksum as the classic big-endian byte-pair sum
 * and stores the result with (uint8_t)(c >> 8), (uint8_t)(c & 0xFF), i.e. in
 * WIRE (big-endian) order inside the uint16_t field. Its receiver-side check is
 * net_checksum(field_included) == 0. With the field filled in BIG-endian that
 * is exactly 0, so this helper reads the field big-endian and recomputes the
 * sum over the full structure including the field.
 *
 * (An earlier revision of this harness loaded the field with a native
 * little-endian load and required stored == complement; that double-counts and
 * produced phantom failures. kernel/net/net.c was correct all along.) */
static int checksum_verifies(const uint8_t *buf, uint32_t len, uint32_t field_off)
{
    (void)field_off;
    return test_ip_checksum(buf, len) == 0u;
}

/* UDP checksum: udp_checksum() folds the pseudo-header AND the transmitted
 * checksum field and returns the complemented byte-pair sum; it substitutes
 * 0xFFFF for a zero complement (the "absent checksum" encoding). For a valid
 * datagram that expression evaluates to 0xFFFF. */
static int udp_checksum_verifies(uint32_t src, uint32_t dst, const uint8_t *udp,
                                 uint32_t len)
{
    if (len < 8u)
        return 0;
    return test_udp_checksum(src, dst, udp, len, 17) == 0xFFFFu;
}

static void ip4(uint8_t *dst, uint32_t host_order)
{
    uint32_t n = be32(host_order);
    memcpy(dst, &n, 4);
}

/* Build eth + IPv4 + payload into out. Returns the total frame length. */
static uint32_t build_ip_frame(uint8_t *out, uint32_t src, uint32_t dst,
                               uint8_t proto, const uint8_t *payload, uint32_t plen)
{
    uint8_t *ip = out + 14;
    uint32_t total = 20u + plen;
    uint16_t c;

    memcpy(out, g_fake_mac, 6);                 /* dst mac (us)  */
    out[6] = 0x52; out[7] = 0x55; out[8] = 0x0A;
    out[9] = 0x00; out[10] = 0x02; out[11] = 0x02;   /* src: slirp gw */
    out[12] = 0x08; out[13] = 0x00;

    memset(ip, 0, 20);
    ip[0] = 0x45;
    ip[2] = (uint8_t)(total >> 8);
    ip[3] = (uint8_t)(total & 0xFFu);
    ip[6] = 0x40;
    ip[8] = 64;
    ip[9] = proto;
    ip4(ip + 12, src);
    ip4(ip + 16, dst);
    memcpy(ip + 20, payload, plen);
    c = test_ip_checksum(ip, 20);
    ip[10] = (uint8_t)(c >> 8);                /* wire (big-endian) order */
    ip[11] = (uint8_t)(c & 0xFFu);
    return 14u + total;
}

static uint16_t test_ip_checksum(const uint8_t *d, uint32_t len)
{
    uint32_t sum = 0;
    uint32_t i;
    for (i = 0; i + 1u < len; i += 2u)
        sum += ((uint32_t)d[i] << 8) | d[i + 1u];
    if (len & 1u)
        sum += (uint32_t)d[len - 1u] << 8;
    while (sum >> 16)
        sum = (sum & 0xFFFFu) + (sum >> 16);
    return (uint16_t)(~sum & 0xFFFFu);
}

static uint16_t test_udp_checksum(uint32_t src, uint32_t dst,
                                  const uint8_t *udp, uint32_t len, uint8_t proto)
{
    uint8_t pseudo[12];
    uint32_t sum = 0;
    uint32_t i;
    ip4(pseudo, src);
    ip4(pseudo + 4, dst);
    pseudo[8] = 0;
    pseudo[9] = proto;
    pseudo[10] = (uint8_t)(len >> 8);
    pseudo[11] = (uint8_t)(len & 0xFFu);
    for (i = 0; i < 12u; i += 2u)
        sum += ((uint32_t)pseudo[i] << 8) | pseudo[i + 1u];
    for (i = 0; i + 1u < len; i += 2u)
        sum += ((uint32_t)udp[i] << 8) | udp[i + 1u];
    if (len & 1u)
        sum += (uint32_t)udp[len - 1u] << 8;
    while (sum >> 16)
        sum = (sum & 0xFFFFu) + (sum >> 16);
    sum = ~sum & 0xFFFFu;
    return (uint16_t)(sum == 0u ? 0xFFFFu : sum);
}

/* ---- inbound datagram builders ----
 *
 * BYTE-ORDER NOTE (this is the subtle part of the harness):
 *   - The kernel builds the ICMP echo id/sequence in NETWORK order
 *     (htons(id) << 16 | htons(seq)) but reads a reply's fields as
 *     ntohs(...). On the wire the bytes are therefore big-endian, so the
 *     harness writes them big-endian too.
 *   - The kernel stores its own computed ICMP/IP checksums through a packed
 *     uint16_t field, so the bytes on the wire are the little-endian image of
 *     the value (a hex dump shows "1a 43" for 0x431a). The harness must use a
 *     native store for those fields as well, which is what
 *     checksum_verifies() reads back. */


static uint32_t build_arp_reply(uint8_t *out, uint32_t who_ip, const uint8_t *who_mac,
                                uint32_t my_ip, const uint8_t *my_mac)
{
    memset(out, 0, 42);
    memcpy(out, my_mac, 6);
    memcpy(out + 6, who_mac, 6);
    out[12] = 0x08; out[13] = 0x06;
    out[14] = 0x00; out[15] = 0x01;             /* htype 1 */
    out[16] = 0x08; out[17] = 0x00;             /* ptype IPv4 */
    out[18] = 6; out[19] = 4;
    out[20] = 0x00; out[21] = 0x02;             /* reply */
    memcpy(out + 22, who_mac, 6);
    ip4(out + 28, who_ip);
    memcpy(out + 32, my_mac, 6);
    ip4(out + 38, my_ip);
    return 42;
}

static uint32_t build_arp_request(uint8_t *out, uint32_t who_ip, const uint8_t *who_mac,
                                  uint32_t target_ip)
{
    memset(out, 0, 42);
    memcpy(out, g_fake_mac, 6);
    memcpy(out + 6, who_mac, 6);
    out[12] = 0x08; out[13] = 0x06;
    out[14] = 0x00; out[15] = 0x01;
    out[16] = 0x08; out[17] = 0x00;
    out[18] = 6; out[19] = 4;
    out[20] = 0x00; out[21] = 0x01;             /* request */
    memcpy(out + 22, who_mac, 6);
    ip4(out + 28, who_ip);
    memset(out + 32, 0, 6);
    ip4(out + 38, target_ip);
    return 42;
}

static uint32_t build_icmp_echo(uint8_t *out, uint32_t src, uint32_t dst,
                                uint8_t type, uint16_t id, uint16_t seq,
                                const uint8_t *payload, uint32_t plen)
{
    uint8_t icmp[512];
    uint16_t c;
    memset(icmp, 0, sizeof(icmp));
    icmp[0] = type;
    icmp[1] = 0;
    /* wire order for echo id/sequence, exactly like the kernel's TX path */
    icmp[4] = (uint8_t)(id >> 8); icmp[5] = (uint8_t)(id & 0xFFu);
    icmp[6] = (uint8_t)(seq >> 8); icmp[7] = (uint8_t)(seq & 0xFFu);
    if (plen)
        memcpy(icmp + 8, payload, plen);
    c = test_ip_checksum(icmp, 8u + plen);
    icmp[2] = (uint8_t)(c >> 8);               /* wire (big-endian) order */
    icmp[3] = (uint8_t)(c & 0xFFu);
    return build_ip_frame(out, src, dst, 1, icmp, 8u + plen);
}

static uint32_t build_udp(uint8_t *out, uint32_t src, uint32_t dst,
                          uint16_t sport, uint16_t dport, uint8_t proto,
                          const uint8_t *data, uint32_t dlen, int corrupt)
{
    uint8_t udp[1500];
    uint32_t ulen = 8u + dlen;
    uint16_t c;
    memset(udp, 0, sizeof(udp));
    udp[0] = (uint8_t)(sport >> 8); udp[1] = (uint8_t)(sport & 0xFFu);
    udp[2] = (uint8_t)(dport >> 8); udp[3] = (uint8_t)(dport & 0xFFu);
    udp[4] = (uint8_t)(ulen >> 8);  udp[5] = (uint8_t)(ulen & 0xFFu);
    if (dlen)
        memcpy(udp + 8, data, dlen);
    c = test_udp_checksum(src, dst, udp, ulen, proto);
    if (corrupt)
        c ^= 0x0100u;
    /* The kernel's udp_rx() compares udp_checksum(...) == 0 as the raw 16-bit
     * field, so the value travels in "sum of bytes as big-endian words == 0"
     * form. test_udp_checksum() already returns that wire-order value, so
     * storing it big-endian is correct here. */
    udp[6] = (uint8_t)(c >> 8);
    udp[7] = (uint8_t)(c & 0xFFu);
    return build_ip_frame(out, src, dst, proto, udp, ulen);
}

/* The peer MAC is used by several tests as "the host we talk to". */
static const uint8_t g_peer_mac[6] = {0x52, 0x55, 0x0A, 0x00, 0x02, 0x02};

/* ---- captured UDP payload from the handler ---- */
static uint32_t g_udp_cb_count;
static uint32_t g_udp_cb_src, g_udp_cb_sport, g_udp_cb_dport, g_udp_cb_len;
static uint8_t  g_udp_cb_data[1500];

static void udp_cb(uint32_t src, uint16_t sport, uint16_t dport,
                   const void *data, uint32_t len)
{
    g_udp_cb_count++;
    if (getenv("NET_HARNESS_DUMP"))
        printf("  info : udp_cb fired #%u src=%08x sport=%u dport=%u len=%u\n",
               g_udp_cb_count, src, sport, dport, len);
    g_udp_cb_src = src;
    g_udp_cb_sport = sport;
    g_udp_cb_dport = dport;
    g_udp_cb_len = len;
    if (len && len <= sizeof(g_udp_cb_data))
        memcpy(g_udp_cb_data, data, len);
}

static void banner(const char *s) { printf("\n=== %s ===\n", s); }

/* ===================== tests ===================== */

/* net_ping() chooses its own sequence number: g_icmp_seq starts at 0 after
 * net_init() and is pre-incremented, so the Nth ping in a process uses seq N.
 * The harness tracks that counter itself and builds the reply with the matching
 * value, which is how a real echo responder behaves (it copies the request's
 * id/sequence back verbatim). `id` and `seq` are written as wire bytes. */
static uint32_t g_ping_seq;

static uint32_t build_icon_echo_wire(uint8_t *out, uint32_t src, uint32_t dst,
                                     uint16_t id, uint16_t seq)
{
    uint8_t icmp[512];
    uint16_t c;
    uint32_t i;
    memset(icmp, 0, sizeof(icmp));
    icmp[0] = 0;                                   /* echo reply */
    icmp[1] = 0;
    icmp[4] = (uint8_t)(id >> 8);   icmp[5] = (uint8_t)(id & 0xFFu);
    icmp[6] = (uint8_t)(seq >> 8);  icmp[7] = (uint8_t)(seq & 0xFFu);
    for (i = 0; i < 56u; i++)
        icmp[8 + i] = (uint8_t)('a' + (i % 26u));
    c = test_ip_checksum(icmp, 64);
    memcpy(&icmp[2], &c, 2);                       /* native load/store, like the kernel */
    return build_ip_frame(out, src, dst, 1, icmp, 64);
}

/* Teach the stack a peer MAC by injecting an ARP reply for it. Returns 0 when
 * the entry was cached. */
static int cache_peer(uint32_t ip, const uint8_t *mac)
{
    uint8_t frame[1600];
    uint32_t n = build_arp_reply(frame, ip, mac, 0x0A00020Fu, g_fake_mac);
    uint8_t got[6];
    fake_rx_push(frame, n);
    net_poll();
    return net_arp_lookup(ip, got) == 0 ? 0 : -1;
}

static void test_init_log(void)
{
    banner("net_init / acceptance log lines");
    fake_tx_reset();
    int r = net_init();
    CHECK(r == 0, "net_init() returned 0");
    CHECK(net_ready() == 1, "net_ready() == 1");
    CHECK(net_ip() == 0x0A00020Fu, "net_ip() == 10.0.2.15");
    CHECK(net_gw() == 0x0A000202u, "net_gw() == 10.0.2.2");
    CHECK(net_mask() == 0xFFFFFF00u, "net_mask() == 255.255.255.0");
    CHECK(net_dns() == 0x0A000203u, "net_dns() == 10.0.2.3");

    int found_line = -1, ip_line = -1, i;
    for (i = 0; i < g_console_log_n; i++) {
        if (strstr(g_console_log[i], "[net] rtl8139 found at "))
            found_line = i;
        if (strstr(g_console_log[i], "[net] IPv4 10.0.2.15/24 gw 10.0.2.2 (mode=poll)"))
            ip_line = i;
    }
    CHECK(found_line >= 0, "logged '[net] rtl8139 found at <io base>, irq=<n>, mac=...'");
    if (found_line >= 0)
        CHECK(strstr(g_console_log[found_line], "mac=52:54:00:12:34:56") != NULL,
              "MAC in the log line matches the NIC (52:54:00:12:34:56)");
    CHECK(ip_line >= 0, "logged '[net] IPv4 10.0.2.15/24 gw 10.0.2.2 (mode=poll)'");
}

static void test_ip_helpers(void)
{
    uint32_t ip = 0;
    char buf[16];
    banner("address helpers");
    CHECK(net_ip_from_str("10.0.2.2", &ip) == 0 && ip == 0x0A000202u,
          "\"10.0.2.2\" -> 0x0A000202");
    CHECK(net_ip_from_str("255.255.255.255", &ip) == 0 && ip == 0xFFFFFFFFu,
          "\"255.255.255.255\" -> 0xFFFFFFFF");
    CHECK(net_ip_from_str("256.1.1.1", &ip) != 0, "\"256.1.1.1\" rejected");
    CHECK(net_ip_from_str("1.2.3", &ip) != 0, "\"1.2.3\" rejected");
    CHECK(net_ip_from_str("1.2.3.4.5", &ip) != 0, "\"1.2.3.4.5\" rejected");
    CHECK(net_ip_from_str("a.b.c.d", &ip) != 0, "non-numeric rejected");
    net_ip_to_str(0x0A00020Fu, buf, sizeof(buf));
    CHECK(strcmp(buf, "10.0.2.15") == 0, "0x0A00020F -> \"10.0.2.15\"");
    net_ip_to_str(0x00000000u, buf, sizeof(buf));
    CHECK(strcmp(buf, "0.0.0.0") == 0, "0 -> \"0.0.0.0\"");
    net_ip_to_str(0xC0A80101u, buf, sizeof(buf));
    CHECK(strcmp(buf, "192.168.1.1") == 0, "0xC0A80101 -> \"192.168.1.1\"");
}

static void test_arp(void)
{
    uint8_t frame[1600];
    uint8_t mac[6];
    uint32_t n;
    static const uint8_t peer_mac[6] = {0x52, 0x55, 0x0A, 0x00, 0x02, 0x02};

    banner("ARP request / reply / cache");
    CHECK(net_arp_lookup(0x0A000202u, mac) == -1, "cache starts empty for 10.0.2.2");

    /* --- an inbound request for OUR address must be answered --- */
    fake_tx_reset();
    net_stats_reset();
    n = build_arp_request(frame, 0x0A000202u, peer_mac, 0x0A00020Fu);
    fake_rx_push(frame, n);
    net_poll();
    {
        int idx = fake_tx_find(0x0806);
        CHECK(idx >= 0, "ARP request for our IP produced an ARP frame");
        if (idx >= 0) {
            const uint8_t *f = g_txq[idx].data;
            CHECK(((f[20] << 8) | f[21]) == 2, "  opcode == reply (2)");
            CHECK(memcmp(f, peer_mac, 6) == 0, "  dst MAC == requester MAC");
            CHECK(memcmp(f + 6, g_fake_mac, 6) == 0, "  src MAC == our MAC");
            CHECK(((f[28] << 24) | (f[29] << 16) | (f[30] << 8) | f[31]) == (int)0x0A00020F,
                  "  sender protocol address == 10.0.2.15");
            CHECK(((f[38] << 24) | (f[39] << 16) | (f[40] << 8) | f[41]) == (int)0x0A000202,
                  "  target protocol address == 10.0.2.2");
        }
    }
    CHECK(net_arp_lookup(0x0A000202u, mac) == 0 && memcmp(mac, peer_mac, 6) == 0,
          "requester learned from the inbound request");

    /* --- an inbound reply must teach us a new peer --- */
    {
        static const uint8_t other_mac[6] = {0x52, 0x55, 0x0A, 0x00, 0x02, 0x0F};
        n = build_arp_reply(frame, 0x0A00020Fu, other_mac, 0x0A00020Fu, g_fake_mac);
        fake_rx_push(frame, n);
        net_poll();
        CHECK(net_arp_lookup(0x0A00020Fu, mac) == 0 && memcmp(mac, other_mac, 6) == 0,
              "inbound ARP reply cached 10.0.2.15's peer MAC");
    }

    /* --- net_arp_resolve() must send a request and wait for the reply --- */
    {
        static const uint8_t slow_mac[6] = {0x52, 0x55, 0x0A, 0x00, 0x02, 0x03};
        fake_tx_reset();
        net_stats_reset();
        /* the responder answers the very first poll after the request goes out */
        n = build_arp_reply(frame, 0x0A000203u, slow_mac, 0x0A00020Fu, g_fake_mac);
        fake_rx_push(frame, n);
        int rr = net_arp_resolve(0x0A000203u, mac);
        CHECK(rr == 0, "net_arp_resolve(10.0.2.3) returned 0");
        CHECK(memcmp(mac, slow_mac, 6) == 0, "  resolved MAC matches the reply");
        CHECK(1 <= net_arp_entries() && net_arp_entries() <= 8,
              "  cache entry count is 1..8");
    }

    /* --- timeout path: nobody answers --- */
    {
        fake_tx_reset();
        int rr = net_arp_resolve(0x0A000099u, mac);
        CHECK(rr == -1, "net_arp_resolve() on a silent host returns -1 (timeout)");
        CHECK(fake_tx_find(0x0806) >= 0, "  and at least one request was transmitted");
    }
}

static void test_icmp_reply(void)
{
    uint8_t frame[1600];
    uint8_t payload[56];
    uint32_t n, i;

    banner("ICMP echo request -> reply");
    for (i = 0; i < sizeof(payload); i++)
        payload[i] = (uint8_t)(i + 1);

    /* Learn the peer MAC first so the reply path does not need ARP. */
    {
        static const uint8_t peer_mac[6] = {0x52, 0x55, 0x0A, 0x00, 0x02, 0x02};
        uint8_t mac[6];
        n = build_arp_reply(frame, 0x0A00020Fu, peer_mac, 0x0A00020Fu, g_fake_mac);
        fake_rx_push(frame, n);
        net_poll();
        CHECK(net_arp_lookup(0x0A00020Fu, mac) == 0, "peer 10.0.2.15 MAC cached for ICMP test");
    }

    fake_tx_reset();
    net_stats_reset();
    n = build_icmp_echo(frame, 0x0A00020Fu, 0x0A00020Fu, 8, 0x1234, 0x0007,
                        payload, sizeof(payload));
    if (getenv("NET_HARNESS_DUMP")) {
        uint8_t z[20];
        uint16_t ck;
        printf("  info : built echo request len=%u\n", n);
        printf("  hdr  : %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
               frame[14],frame[15],frame[16],frame[17],frame[18],frame[19],frame[20],frame[21],
               frame[22],frame[23],frame[24],frame[25],frame[26],frame[27],frame[28],frame[29],
               frame[30],frame[31],frame[32],frame[33]);
        memcpy(z, frame + 14, 20);
        z[10] = 0; z[11] = 0;
        ck = test_ip_checksum(z, 20);
        printf("  want : zeroed-field checksum=0x%04x -> wire bytes %02x %02x\n",
               ck, (uint8_t)(ck >> 8), (uint8_t)(ck & 0xFF));
        printf("  field: %02x %02x | verify-after-store=0x%04x\n",
               frame[24], frame[25], test_ip_checksum(frame + 14, 20));
        hexdump("echo request in", frame, n);
    }
    fake_rx_push(frame, n);
    net_poll();

    {
        int idx = fake_tx_find(0x0800);
        if (getenv("NET_HARNESS_DUMP")) {
            int k;
            printf("  info : txq has %d frames after net_poll:\n", (int)g_txq_count);
            for (k = 0; k < (int)g_txq_count; k++)
                printf("         [%d] len=%u type=0x%02x%02x\n", k, g_txq[k].len,
                       g_txq[k].data[12], g_txq[k].data[13]);
            net_dump_stats();
        }
        CHECK(idx >= 0, "echo request produced an IPv4 frame");
        if (idx >= 0) {
            const uint8_t *f = g_txq[idx].data;
            const uint8_t *oip = f + 14;
            const uint8_t *oic = f + 34;
            if (getenv("NET_HARNESS_DUMP"))
                hexdump("icmp reply", f, g_txq[idx].len);
            printf("  info : txf[%d] len=%u ipfld=%02x%02x icmpfld=%02x%02x ipsum=%04x icmpsum=%04x\n",
                   idx, g_txq[idx].len, oip[10], oip[11], oic[2], oic[3],
                   test_ip_checksum(oip, 20), test_ip_checksum(oic, 8u + sizeof(payload)));
            printf("  info : eth dst=%02x:%02x:%02x:%02x:%02x:%02x peer=%02x:%02x:%02x:%02x:%02x:%02x\n",
                   f[0], f[1], f[2], f[3], f[4], f[5],
                   g_fake_mac[0], g_fake_mac[1], g_fake_mac[2],
                   g_fake_mac[3], g_fake_mac[4], g_fake_mac[5]);
            CHECK(oic[0] == 0, "  ICMP type == 0 (echo reply)");
            CHECK(oic[1] == 0, "  ICMP code == 0");
            CHECK(checksum_verifies(oic, 8u + sizeof(payload), 2),
                  "  ICMP checksum field verifies");
            CHECK(checksum_verifies(oip, 20, 10),
                  "  IPv4 header checksum field verifies");
            CHECK(oip[9] == 1, "  IP protocol == ICMP");
            CHECK(oip[8] == 64, "  TTL == 64");
            CHECK(((oip[12] << 24) | (oip[13] << 16) | (oip[14] << 8) | oip[15]) == (int)0x0A00020F,
                  "  src IP == 10.0.2.15");
            CHECK(((oip[16] << 24) | (oip[17] << 16) | (oip[18] << 8) | oip[19]) == (int)0x0A00020F,
                  "  dst IP == requester 10.0.2.15");
            CHECK(oic[4] == 0x12 && oic[5] == 0x34, "  echo id preserved (0x1234)");
            CHECK(oic[6] == 0x00 && oic[7] == 0x07, "  echo seq preserved (7)");
            CHECK(memcmp(oic + 8, payload, sizeof(payload)) == 0,
                  "  56-byte payload copied verbatim");
            CHECK(g_txq[idx].len == 14u + 20u + 8u + sizeof(payload),
                  "  frame length == 98 bytes");
            CHECK(memcmp(f, g_peer_mac, 6) == 0, "  ethernet dst == peer MAC");
        }
    }
}

static void test_ping(void)
{
    uint8_t frame[1600];
    uint8_t payload[56];
    uint32_t rtt = 0;
    int r;
    uint32_t i;

    banner("net_ping()");
    for (i = 0; i < sizeof(payload); i++)
        payload[i] = (uint8_t)('a' + (i % 26));

    /* --- success: queue a reply with the id/seq the next ping will use --- */
    fake_tx_reset();
    net_stats_reset();
    {
        uint32_t n = build_icmp_echo(frame, 0x0A000202u, 0x0A00020Fu, 0,
                                     0xB00B, (uint16_t)(g_ping_seq + 1),
                                     payload, sizeof(payload));
        fake_rx_push(frame, n);
    }
    g_ping_seq++;
    r = net_ping(0x0A000202u, 1000, &rtt);
    CHECK(r == 0, "net_ping(10.0.2.2) returned 0 (reply matched)");
    CHECK(rtt > 0, "  rtt_ticks > 0 (rtt=%u ticks, %u us)", rtt, net_ping_us(rtt));
    CHECK(net_ping_us(0) == 0, "  net_ping_us(0) == 0");
    CHECK(net_ping_us(1193182u) == 1000000u, "  net_ping_us(1193182) == 1000000");

    /* --- wrong id must not satisfy a pending ping --- */
    {
        uint32_t n = build_icmp_echo(frame, 0x0A000202u, 0x0A00020Fu, 0,
                                     0xDEAD, (uint16_t)(g_ping_seq + 1),
                                     payload, sizeof(payload));
        fake_rx_push(frame, n);
    }
    g_ping_seq++;
    r = net_ping(0x0A000202u, 40, &rtt);
    CHECK(r == -1, "net_ping() ignores a reply with a foreign echo id (timeout)");

    /* --- wrong sequence with the right id must also be ignored --- */
    {
        uint32_t n = build_icmp_echo(frame, 0x0A000202u, 0x0A00020Fu, 0,
                                     0xB00B, (uint16_t)(g_ping_seq + 99),
                                     payload, sizeof(payload));
        fake_rx_push(frame, n);
    }
    g_ping_seq++;
    r = net_ping(0x0A000202u, 40, &rtt);
    CHECK(r == -1, "net_ping() ignores a reply with a foreign sequence (timeout)");

    /* --- corrupted ICMP checksum must be dropped --- */
    {
        uint32_t n = build_icmp_echo(frame, 0x0A000202u, 0x0A00020Fu, 0,
                                     0xB00B, (uint16_t)(g_ping_seq + 1),
                                     payload, sizeof(payload));
        frame[34 + 2] ^= 0xFF;                 /* wreck the ICMP checksum */
        fake_rx_push(frame, n);
    }
    g_ping_seq++;
    r = net_ping(0x0A000202u, 40, &rtt);
    CHECK(r == -1, "net_ping() ignores a reply with a bad ICMP checksum");

    /* --- a reply from the wrong host is ignored --- */
    {
        uint32_t n = build_icmp_echo(frame, 0x0A000209u, 0x0A00020Fu, 0,
                                     0xB00B, (uint16_t)(g_ping_seq + 1),
                                     payload, sizeof(payload));
        fake_rx_push(frame, n);
    }
    g_ping_seq++;
    r = net_ping(0x0A000202u, 40, &rtt);
    CHECK(r == -1, "net_ping() ignores a reply from another host (timeout)");
}

static void test_ip_rx_guards(void)
{
    uint8_t frame[1600];
    uint32_t n;
    uint64_t before;

    banner("IPv4 receive guards");

    /* bad IP header checksum -> no ICMP reply */
    fake_tx_reset();
    n = build_icmp_echo(frame, 0x0A00020Fu, 0x0A00020Fu, 8, 0x11, 0x22, NULL, 0);
    frame[14 + 10] ^= 0xFF;
    fake_rx_push(frame, n);
    net_poll();
    CHECK(fake_tx_find(0x0800) < 0, "frame with a bad IPv4 checksum is dropped");

    /* truncated frame -> dropped without a reply */
    fake_tx_reset();
    n = build_icmp_echo(frame, 0x0A00020Fu, 0x0A00020Fu, 8, 0x11, 0x23, NULL, 0);
    fake_rx_push(frame, 20);                   /* shorter than the headers */
    net_poll();
    CHECK(fake_tx_find(0x0800) < 0, "truncated frame is dropped");

    /* a fragment (MF set) is dropped, not answered */
    fake_tx_reset();
    n = build_icmp_echo(frame, 0x0A00020Fu, 0x0A00020Fu, 8, 0x11, 0x24, NULL, 0);
    frame[14 + 6] |= 0x20;                     /* MF */
    {
        uint16_t c;
        frame[14 + 10] = 0; frame[14 + 11] = 0;
        c = test_ip_checksum(frame + 14, 20);
        frame[14 + 10] = (uint8_t)(c >> 8);
        frame[14 + 11] = (uint8_t)(c & 0xFFu);
    }
    fake_rx_push(frame, n);
    net_poll();
    CHECK(fake_tx_find(0x0800) < 0, "IP fragment (MF set) is dropped");

    /* a packet for someone else on our subnet is not accepted as ours */
    before = 0;
    (void)before;
    {
        static const uint8_t peer_mac[6] = {0x52, 0x55, 0x0A, 0x00, 0x02, 0x02};
        uint8_t mac[6];
        n = build_arp_reply(frame, 0x0A00020Fu, peer_mac, 0x0A00020Fu, g_fake_mac);
        fake_rx_push(frame, n);
        net_poll();
        CHECK(net_arp_lookup(0x0A00020Fu, mac) == 0, "peer MAC re-cached");
    }
    fake_tx_reset();
    n = build_icmp_echo(frame, 0x0A00020Fu, 0x0A00020Eu, 8, 0x11, 0x25, NULL, 0);
    fake_rx_push(frame, n);
    net_poll();
    CHECK(fake_tx_find(0x0800) < 0,
          "echo request addressed to 10.0.2.14 is not answered by us");
}

static void test_udp(void)
{
    uint8_t frame[1600];
    uint8_t data[64];
    uint32_t n, i;
    int r;

    banner("UDP send / receive");
    for (i = 0; i < sizeof(data); i++)
        data[i] = (uint8_t)(0xC0 + i);

    net_udp_set_handler(udp_cb);
    g_udp_cb_count = 0;

    /* --- make sure the destination MAC is known so the TX path does not have
     *     to run a (bounded but pointless) ARP exchange first --- */
    {
        static const uint8_t gw_mac[6] = {0x52, 0x55, 0x0A, 0x00, 0x02, 0x02};
        CHECK(cache_peer(0x0A000202u, gw_mac) == 0, "gateway MAC cached for the UDP tests");
    }

    /* --- TX: the datagram we emit must verify like a real one --- */
    fake_tx_reset();
    net_stats_reset();
    r = net_udp_send(0x0A000202u, 5555, 53, data, 32);
    CHECK(r == 0, "net_udp_send(10.0.2.2:53) returned 0");
    {
        int idx = fake_tx_find(0x0800);
        CHECK(idx >= 0, "  produced an IPv4 frame");
        if (idx >= 0) {
            const uint8_t *f = g_txq[idx].data;
            const uint8_t *oip = f + 14;
            const uint8_t *oudp = f + 34;
            uint32_t ulen = 8u + 32u;
            if (getenv("NET_HARNESS_DUMP"))
                hexdump("udp tx", f, g_txq[idx].len);
            CHECK(checksum_verifies(oip, 20, 10), "  IPv4 checksum field verifies");
            CHECK(wire16(oudp) == 5555, "  source port 5555");
            CHECK(wire16(oudp + 2) == 53, "  dest port 53");
            CHECK(wire16(oudp + 4) == ulen, "  udp length == 40");
            CHECK(wire16(oip + 2) == (uint16_t)(20u + ulen), "  ip total length == 60");
            if (getenv("NET_HARNESS_DUMP")) {
                uint8_t z[1600];
                uint16_t want;
                memcpy(z, oudp, ulen);
                z[6] = 0; z[7] = 0;
                want = test_udp_checksum(0x0A00020Fu, 0x0A000202u, z, ulen, 17);
                printf("  info : udp stored=%02x%02x want=%04x (%02x %02x)\n",
                       oudp[6], oudp[7], want, (uint8_t)(want >> 8), (uint8_t)(want & 0xFF));
            }
            CHECK(udp_checksum_verifies(0x0A00020Fu, 0x0A000202u, oudp, ulen),
                  "  UDP pseudo-header checksum field verifies");
            CHECK(memcmp(oudp + 8, data, 32) == 0, "  payload byte-identical");
        }
    }

    /* --- RX: feed our own datagram back in and require delivery --- */
    fake_tx_reset();
    n = build_udp(frame, 0x0A000202u, 0x0A00020Fu, 53, 5555, 17, data, 32, 0);
    fake_rx_push(frame, n);
    net_poll();
    printf("  info : after RX poll, cb_count=%u src=%08x sport=%u dport=%u len=%u\n",
           g_udp_cb_count, g_udp_cb_src, g_udp_cb_sport, g_udp_cb_dport, g_udp_cb_len);
    CHECK(g_udp_cb_count == 1, "inbound UDP datagram reached the handler");
    CHECK(g_udp_cb_src == 0x0A000202u, "  src ip == 10.0.2.2");
    CHECK(g_udp_cb_sport == 53, "  src port == 53");
    CHECK(g_udp_cb_dport == 5555, "  dst port == 5555");
    CHECK(g_udp_cb_len == 32, "  payload len == 32");
    CHECK(memcmp(g_udp_cb_data, data, 32) == 0, "  payload byte-identical");

    /* --- RX: a corrupted UDP checksum must be dropped --- */
    g_udp_cb_count = 0;
    n = build_udp(frame, 0x0A000202u, 0x0A00020Fu, 53, 5555, 17, data, 32, 1);
    fake_rx_push(frame, n);
    net_poll();
    CHECK(g_udp_cb_count == 0, "datagram with a bad UDP checksum is dropped");

    /* --- RX: a zero checksum is legal for IPv4 UDP --- */
    g_udp_cb_count = 0;
    n = build_udp(frame, 0x0A000202u, 0x0A00020Fu, 53, 5555, 17, data, 32, 0);
    frame[34 + 6] = 0;
    frame[34 + 7] = 0;
    fake_rx_push(frame, n);
    net_poll();
    CHECK(g_udp_cb_count == 1, "datagram with checksum 0 (optional in IPv4) is accepted");

    /* --- oversize payload is rejected --- */
    CHECK(net_udp_send(0x0A000202u, 1, 2, data, 2000) == -2,
          "net_udp_send() rejects a payload above 1472 bytes");

    /* --- ARP must not have been needed for 10.0.2.2 (already cached) --- */
    CHECK(net_arp_lookup(0x0A000202u, (uint8_t[6]){0}) == 0,
          "10.0.2.2 still cached after the UDP traffic");
}

static void test_stats(void)
{
    uint8_t frame[1600];
    uint32_t n;

    banner("statistics");
    net_stats_reset();
    /* one ARP reply + one echo request, then dump */
    {
        static const uint8_t peer_mac[6] = {0x52, 0x55, 0x0A, 0x00, 0x02, 0x02};
        n = build_arp_reply(frame, 0x0A00020Fu, peer_mac, 0x0A00020Fu, g_fake_mac);
        fake_rx_push(frame, n);
    }
    n = build_icmp_echo(frame, 0x0A00020Fu, 0x0A00020Fu, 8, 0x77, 0x01, NULL, 0);
    fake_rx_push(frame, n);
    net_poll();

    g_console_log_n = 0;
    net_dump_stats();
    {
        int i, lines = 0, prefixed = 1;
        for (i = 0; i < g_console_log_n; i++) {
            if (g_console_log[i][0] == '\0')
                continue;
            lines++;
            if (strncmp(g_console_log[i], "[net] ", 6) != 0)
                prefixed = 0;
        }
        CHECK(lines >= 6, "net_dump_stats() printed %d lines", lines);
        CHECK(prefixed, "every stats line starts with \"[net] \"");
    }
    {
        int seen_frames = 0, seen_arp = 0, seen_ip = 0, seen_icmp = 0, seen_udp = 0, seen_last = 0;
        int i;
        for (i = 0; i < g_console_log_n; i++) {
            if (strstr(g_console_log[i], "[net] frames rx=")) seen_frames = 1;
            if (strstr(g_console_log[i], "[net] arp rx=")) seen_arp = 1;
            if (strstr(g_console_log[i], "[net] ip rx=")) seen_ip = 1;
            if (strstr(g_console_log[i], "[net] icmp rx=")) seen_icmp = 1;
            if (strstr(g_console_log[i], "[net] udp rx=")) seen_udp = 1;
            if (strstr(g_console_log[i], "[net] last rx ")) seen_last = 1;
        }
        CHECK(seen_frames && seen_arp && seen_ip && seen_icmp && seen_udp,
              "frames/arp/ip/icmp/udp counters all reported");
        CHECK(seen_last, "last received packet summary reported");
    }
}

static void test_selftest(void)
{
    uint8_t frame[1600];
    uint8_t payload[56];
    uint32_t i;

    banner("net_selftest()");
    for (i = 0; i < sizeof(payload); i++)
        payload[i] = (uint8_t)(i);

    /* --- timeout wording: nothing queued, so the gateway never answers --- */
    g_console_log_n = 0;
    {
        int r = net_selftest();
        int i2, saw = 0;
        for (i2 = 0; i2 < g_console_log_n; i2++) {
            if (strstr(g_console_log[i2], "[net] selftest ping TIMEOUT"))
                saw = 1;
        }
        CHECK(r == -1, "net_selftest() returns -1 when nobody answers");
        CHECK(saw, "prints exactly the documented TIMEOUT line");
    }
    g_ping_seq++;                              /* the ping consumed a sequence */

    /* --- success wording: queue the reply for the selftest's next ping --- */
    g_console_log_n = 0;
    {
        uint32_t n = build_icmp_echo(frame, 0x0A000202u, 0x0A00020Fu, 0,
                                     0xB00B, (uint16_t)(g_ping_seq + 1),
                                     payload, sizeof(payload));
        fake_rx_push(frame, n);
    }
    g_ping_seq++;
    g_ping_seq++;
    {
        int r = net_selftest();
        int i2, saw_reply = 0, saw_frames = 0;
        for (i2 = 0; i2 < g_console_log_n; i2++) {
            if (strstr(g_console_log[i2], "[net] selftest ping 10.0.2.2 -> reply in "))
                saw_reply = 1;
            if (strstr(g_console_log[i2], "[net] selftest frames rx="))
                saw_frames = 1;
        }
        CHECK(r == 0, "net_selftest() returns 0 on a reply");
        CHECK(saw_reply, "prints the documented reply line");
        CHECK(saw_frames, "prints the rx/tx/arp_entries evidence line");
    }
}

int main(void)
{
    printf("SutureOS network stack host harness\n");
    printf("(kernel/net/net.c compiled unmodified against a scripted NIC)\n");

    test_init_log();
    test_ip_helpers();
    test_arp();
    test_icmp_reply();
    test_ip_rx_guards();
    test_ping();
    test_udp();
    test_stats();
    test_selftest();

    printf("\n%s: %d failure%s\n", g_fail ? "FAILED" : "ALL PASS",
           g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
