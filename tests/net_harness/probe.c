/* Minimal byte-dump probe for the exact frames net.c produces.
 * Uses net_input() to inject frames directly, bypassing the fake driver. */
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>

#include "net.h"
#include "rtl8139.h"
#include "mb_pit.h"

#define MAXF 16
#define RXQ 8
static uint8_t g_q[MAXF][1600];
static uint32_t g_qlen[MAXF];
static uint32_t g_qn;
static uint8_t g_rxq[RXQ][1600];
static uint32_t g_rxqlen[RXQ];
static uint32_t g_rxq_head, g_rxq_count;
static uint8_t g_mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
static uint8_t g_pit;

static void rx_push(const uint8_t *f, uint32_t n)
{
    if (g_rxq_count >= RXQ)
        return;
    memcpy(g_rxq[(g_rxq_head + g_rxq_count) % RXQ], f, n);
    g_rxqlen[(g_rxq_head + g_rxq_count) % RXQ] = n;
    g_rxq_count++;
}

static void pump(void) { net_poll(); }

static void dump(const char *tag, const uint8_t *f, uint32_t n)
{
    uint32_t i;
    printf("%s (%u bytes)\n", tag, n);
    for (i = 0; i < n; i += 16) {
        uint32_t j;
        printf("  %04x: ", i);
        for (j = 0; j < 16 && i + j < n; j++)
            printf("%02x ", f[i + j]);
        printf("\n");
    }
}

static uint16_t ref_ck(const uint8_t *d, uint32_t len)
{
    uint32_t sum = 0, i;
    for (i = 0; i + 1u < len; i += 2u) sum += ((uint32_t)d[i] << 8) | d[i + 1u];
    if (len & 1u) sum += (uint32_t)d[len - 1u] << 8;
    while (sum >> 16) sum = (sum & 0xFFFFu) + (sum >> 16);
    return (uint16_t)(~sum & 0xFFFFu);
}

int TEST_rtl8139_init(void) { return 0; }
int TEST_rtl8139_present(void) { return 1; }
int TEST_rtl8139_get_mac(uint8_t o[6]) { memcpy(o, g_mac, 6); return 0; }
uint8_t TEST_rtl8139_irq(void) { return 11; }
uint16_t TEST_rtl8139_io_base(void) { return 0xC000; }
int TEST_rtl8139_send_frame(const void *f, uint32_t n)
{
    if (g_qn < MAXF) {
        memcpy(g_q[g_qn], f, n);
        g_qlen[g_qn] = n;
        g_qn++;
    }
    return 0;
}
int TEST_rtl8139_receive(void (*fn)(const uint8_t *, uint32_t))
{
    int n = 0;
    while (g_rxq_count > 0) {
        fn(g_rxq[g_rxq_head], g_rxqlen[g_rxq_head]);
        g_rxq_head = (g_rxq_head + 1) % RXQ;
        g_rxq_count--;
        n++;
    }
    return n;
}
uint16_t TEST_rtl8139_last_isr(void) { return 0; }
uint32_t TEST_rtl8139_rx_drops(void) { return 0; }
uint64_t TEST_rtl8139_tx_frames(void) { return g_qn; }
uint64_t TEST_rtl8139_rx_frames(void) { return 0; }
void TEST_st_rtl8139_test(void) { }

void console_init(void) { }
void console_putc(char c) { (void)c; }
void console_puts(const char *s) { (void)s; }
void console_printf(const char *f, ...) { (void)f; }
void st_run(const char *n, bool p) { (void)n; (void)p; }
int st_failures(void) { return 0; }
int st_total(void) { return 0; }
void mbos_pit_init(uint32_t h) { (void)h; }
uint32_t mbos_pit_hz(void) { return 100; }
uint32_t mbos_pit_divisor(void) { return 11931; }
uint16_t mbos_pit_counter(void)
{
    /* Same scheme as the functional harness: each call advances the model by
     * 12 ms, so net_now_us() sees a perfectly linear clock and every deadline
     * in net.c is reachable within a handful of iterations. */
    static uint64_t us;
    us += 12000ull;
    return (uint16_t)(65536ull - (us * 1193182ull) / 1000000ull);
}

int main(void)
{
    uint8_t frame[1600];
    uint8_t payload[56];
    uint32_t n, i;

    setbuf(stdout, NULL);
    fprintf(stderr, "probe: start\n");
    memset(payload, 0xAA, sizeof(payload));
    net_init();
    fprintf(stderr, "probe: net_init done\n");

    /* ---- 1. inbound ARP reply so the peer MAC is cached ---- */
    {
        static const uint8_t peer[6] = {0x52, 0x55, 0x0A, 0x00, 0x02, 0x02};
        uint8_t a[42];
        memset(a, 0, sizeof(a));
        memcpy(a, g_mac, 6);
        memcpy(a + 6, peer, 6);
        a[12] = 0x08; a[13] = 0x06;
        a[14] = 0; a[15] = 1; a[16] = 0x08; a[17] = 0;
        a[18] = 6; a[19] = 4; a[20] = 0; a[21] = 2;
        memcpy(a + 22, peer, 6);
        a[28] = 10; a[29] = 0; a[30] = 2; a[31] = 15;
        memcpy(a + 32, g_mac, 6);
        a[38] = 10; a[39] = 0; a[40] = 2; a[41] = 15;
        rx_push(a, sizeof(a));
        pump();
        fprintf(stderr, "probe: arp cached=%d`n", net_arp_entries());
    }

    /* ---- 2. inbound ICMP echo request ---- */
    {
        uint8_t icmp[64];
        uint32_t total;
        uint16_t c;
        uint8_t *ip;
        memset(icmp, 0, sizeof(icmp));
        icmp[0] = 8;
        icmp[4] = 0x12; icmp[5] = 0x34;
        icmp[6] = 0x00; icmp[7] = 0x07;
        memcpy(icmp + 8, payload, 56);
        c = ref_ck(icmp, 64);
        printf("harness icmp checksum = 0x%04x\n", c);
        icmp[2] = (uint8_t)(c >> 8); icmp[3] = (uint8_t)(c & 0xFF);

        total = 20 + 64;
        memcpy(frame, g_mac, 6);
        frame[6] = 0x52; frame[7] = 0x55; frame[8] = 0x0A;
        frame[9] = 0; frame[10] = 0x02; frame[11] = 0x02;
        frame[12] = 0x08; frame[13] = 0x00;
        ip = frame + 14;
        memset(ip, 0, 20);
        ip[0] = 0x45;
        ip[2] = (uint8_t)(total >> 8); ip[3] = (uint8_t)(total & 0xFF);
        ip[6] = 0x40; ip[8] = 64; ip[9] = 1;
        ip[12] = 10; ip[13] = 0; ip[14] = 2; ip[15] = 15;
        ip[16] = 10; ip[17] = 0; ip[18] = 2; ip[19] = 15;
        memcpy(ip + 20, icmp, 64);
        c = ref_ck(ip, 20);
        ip[10] = (uint8_t)(c >> 8); ip[11] = (uint8_t)(c & 0xFF);
        n = 14 + total;
        rx_push(frame, n);
        pump();
        fprintf(stderr, "probe: echo handled, txq=%u`n", g_qn);
    }

    printf("\ntx queue after the echo request: %u frame(s)\n", g_qn);
    for (i = 0; i < g_qn; i++) {
        char tag[64];
        snprintf(tag, sizeof(tag), "tx[%u]", i);
        dump(tag, g_q[i], g_qlen[i]);
    }

    /* ---- 3. outbound UDP ---- */
    g_qn = 0;
    {
        uint8_t data[32];
        for (i = 0; i < sizeof(data); i++) data[i] = (uint8_t)(0xC0 + i);
        fprintf(stderr, "probe: calling net_udp_send`n");
        net_udp_send(0x0A000202u, 5555, 53, data, 32);
        fprintf(stderr, "probe: net_udp_send returned`n");
        printf("\ntx queue after net_udp_send: %u frame(s)\n", g_qn);
        for (i = 0; i < g_qn; i++) {
            char tag[64];
            snprintf(tag, sizeof(tag), "tx[%u]", i);
            dump(tag, g_q[i], g_qlen[i]);
        }
    }
    return 0;
}
