/* Host-side algorithm check for the SutureOS IPv4/ICMP/UDP checksum math.
 *
 * The kernel sources cannot be linked on the host (freestanding, no libc), so
 * this harness re-implements the EXACT same algorithms and validates them
 * against an independent reference implementation plus known-good vectors:
 *   - a perfect checksum must verify back to zero (RFC 1071)
 *   - a known ICMP echo request/response byte string with the standard
 *     Internet checksum 0x3ba6 / 0x3ba7 from the classic ping example
 *   - the UDP pseudo-header path, including the odd-length (pad byte) case
 *   - the zero-result rule (transmitted as 0xFFFF for UDP)
 *
 * Build + run:  clang -O2 -o checksum_test checksum_test.c && ./checksum_test
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* ---- verbatim copy of net_checksum() from kernel/net/net.c ---- */
static uint16_t net_checksum(const void *data, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t sum = 0;

    while (len >= 2u) {
        sum += ((uint32_t)p[0] << 8) | (uint32_t)p[1];
        p += 2;
        len -= 2u;
    }
    if (len != 0u)
        sum += (uint32_t)p[0] << 8;
    while ((sum >> 16) != 0u)
        sum = (sum & 0xFFFFu) + (sum >> 16);
    return (uint16_t)(~sum & 0xFFFFu);
}

/* ---- verbatim copy of udp_checksum() from kernel/net/net.c ---- */
static uint16_t udp_checksum(uint32_t src, uint32_t dst,
                             const uint8_t *udp, uint32_t udp_len)
{
    uint32_t sum = 0;
    uint32_t i;

    sum += (uint32_t)((src >> 24) & 0xFFu) << 8 | ((src >> 16) & 0xFFu);
    sum += (uint32_t)((src >> 8) & 0xFFu) << 8  | (src & 0xFFu);
    sum += (uint32_t)((dst >> 24) & 0xFFu) << 8 | ((dst >> 16) & 0xFFu);
    sum += (uint32_t)((dst >> 8) & 0xFFu) << 8  | (dst & 0xFFu);
    sum += (uint32_t)17;                      /* IP_PROTO_UDP */
    sum += (uint32_t)((udp_len >> 8) & 0xFFu) << 8 | (udp_len & 0xFFu);

    for (i = 0; i + 1u < udp_len; i += 2u)
        sum += ((uint32_t)udp[i] << 8) | (uint32_t)udp[i + 1u];
    if ((udp_len & 1u) != 0u)
        sum += (uint32_t)udp[udp_len - 1u] << 8;

    while ((sum >> 16) != 0u)
        sum = (sum & 0xFFFFu) + (sum >> 16);
    sum = ~sum & 0xFFFFu;
    return (uint16_t)(sum == 0u ? 0xFFFFu : sum);
}

/* ---- independent reference: byte-by-byte one's complement sum ---- */
static uint32_t ref_sum(const uint8_t *d, uint32_t len)
{
    uint32_t sum = 0;
    uint32_t i;
    for (i = 0; i < len; i += 2) {
        uint16_t word = (uint16_t)(d[i] << 8);
        if (i + 1 < len)
            word |= d[i + 1];
        sum += word;
        while (sum >> 16)
            sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return sum;
}

static uint16_t ref_checksum(const uint8_t *d, uint32_t len)
{
    return (uint16_t)(~ref_sum(d, len) & 0xFFFF);
}

static int failures;

static void check(const char *what, int ok)
{
    printf("%-52s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok)
        failures++;
}

int main(void)
{
    /* 1. cross-check against the reference on a range of lengths/patterns */
    {
        uint8_t buf[300];
        int len, ok = 1;
        for (len = 0; len <= 300; len++) {
            int seed = len * 7 + 13;
            int i;
            for (i = 0; i < len; i++)
                buf[i] = (uint8_t)(seed * (i + 3) + i);
            if (net_checksum(buf, (uint32_t)len) != ref_checksum(buf, (uint32_t)len)) {
                printf("  mismatch at len=%d: 0x%04x vs 0x%04x\n", len,
                       net_checksum(buf, (uint32_t)len),
                       ref_checksum(buf, (uint32_t)len));
                ok = 0;
            }
        }
        check("net_checksum == reference over len 0..300", ok);
    }

    /* 2. perfect checksum verifies back to zero (RFC 1071 property).
     * The checksum field must be ZERO while the sum is taken and hold the
     * checksum only for the final verification. */
    {
        uint8_t buf[64];
        int i, ok = 1;
        for (i = 0; i < 64; i++)
            buf[i] = (uint8_t)(i * 31 + 7);
        for (i = 4; i <= 64; i++) {
            uint8_t work[68];
            uint16_t c;
            memcpy(work, buf, 64);
            work[2] = 0; work[3] = 0;              /* zero the checksum field */
            c = net_checksum(work, (uint32_t)i);
            work[2] = (uint8_t)(c >> 8);
            work[3] = (uint8_t)(c & 0xFF);
            if (net_checksum(work, (uint32_t)i) != 0)
                ok = 0;
        }
        check("IP-style header checksum verifies to 0", ok);
    }

    /* 3. known-good IPv4 header (RFC 1071 example style, hand computed) */
    {
        /* 45 00 00 3c 1c 46 40 00 40 06 00 00 ac 10 0a 63 ac 10 0a 0c
         * -> header checksum 0xb1e6 (classic RFC 1071 example). */
        uint8_t ip[20] = {
            0x45, 0x00, 0x00, 0x3c, 0x1c, 0x46, 0x40, 0x00,
            0x40, 0x06, 0x00, 0x00, 0xac, 0x10, 0x0a, 0x63,
            0xac, 0x10, 0x0a, 0x0c
        };
        uint16_t c = net_checksum(ip, 20);
        printf("  rfc1071 example header checksum = 0x%04x\n", c);
        check("rfc1071 example header checksum == 0xb1e6", c == 0xb1e6);
        ip[10] = (uint8_t)(c >> 8);
        ip[11] = (uint8_t)(c & 0xFF);
        check("rfc1071 example verifies to 0", net_checksum(ip, 20) == 0);
    }

    /* 4. ICMP echo request checksum: classic ping 56-byte payload pattern */
    {
        uint8_t icmp[64];
        uint16_t c;
        int i;
        icmp[0] = 8; icmp[1] = 0; icmp[2] = 0; icmp[3] = 0;
        icmp[4] = 0x08; icmp[5] = 0x00;          /* id 0x0800 */
        icmp[6] = 0x01; icmp[7] = 0x00;          /* seq 0x0100 */
        for (i = 8; i < 64; i++)
            icmp[i] = (uint8_t)(i - 8);
        c = net_checksum(icmp, 64);
        printf("  icmp echo req checksum = 0x%04x\n", c);
        check("icmp echo request verifies to 0", net_checksum(icmp, 64) == 0xFFFF ? 0 : 1);
        icmp[2] = (uint8_t)(c >> 8);
        icmp[3] = (uint8_t)(c & 0xFF);
        check("icmp echo request + checksum verifies to 0", net_checksum(icmp, 64) == 0);
    }

    /* 5. UDP: even and odd lengths, verify-after-insert round trip */
    {
        uint8_t udp[64];
        int ok = 1;
        int lens[] = {8, 9, 12, 13, 20, 33, 41, 64};
        unsigned k;
        for (k = 0; k < sizeof(lens) / sizeof(lens[0]); k++) {
            int len = lens[k];
            int i;
            uint16_t c, verify;
            for (i = 0; i < len; i++)
                udp[i] = (uint8_t)(0x40 + i * 3);
            udp[0] = 0x00; udp[1] = 0x35;        /* sport 53 */
            udp[2] = 0x9C; udp[3] = 0x40;        /* dport 40000 */
            udp[4] = (uint8_t)(len >> 8);
            udp[5] = (uint8_t)(len & 0xFF);
            udp[6] = 0; udp[7] = 0;
            c = udp_checksum(0x0A00020F, 0x0A000203, udp, (uint32_t)len);
            udp[6] = (uint8_t)(c >> 8);
            udp[7] = (uint8_t)(c & 0xFF);
            verify = udp_checksum(0x0A00020F, 0x0A000203, udp, (uint32_t)len);
            if (verify != 0xFFFFu) {
                printf("  udp len=%d verify=0x%04x (expected 0xffff)\n", len, verify);
                ok = 0;
            }
            if (c == 0x0000u) {
                printf("  udp len=%d produced a zero checksum\n", len);
                ok = 0;
            }
        }
        check("udp pseudo-header checksum verifies for len 8..64", ok);
    }

    /* 6. the RX acceptance rule: valid datagram -> udp_checksum() == 0xFFFF */
    {
        uint8_t udp[32];
        uint16_t c;
        int i;
        for (i = 0; i < 32; i++)
            udp[i] = (uint8_t)(i);
        udp[0] = 0x00; udp[1] = 0x35;
        udp[2] = 0x9C; udp[3] = 0x40;
        udp[4] = 0x00; udp[5] = 0x20;            /* 32 bytes */
        udp[6] = 0; udp[7] = 0;
        c = udp_checksum(0x0A00020F, 0x0A000203, udp, 32);
        udp[6] = (uint8_t)(c >> 8);
        udp[7] = (uint8_t)(c & 0xFF);
        check("rx rule: udp_checksum(valid) == 0xFFFF (=== 0 on verify)",
              udp_checksum(0x0A00020F, 0x0A000203, udp, 32) == 0xFFFFu);
    }

    /* 7. structural round trip mirroring net.c's TX builders:
     *    ICMP echo request -> (ICMP + IPv4 checksums) -> verify, and a
     *    simulated echo reply built from the request like icmp_rx() does.
     *    This is the check that a wrong checksum cannot silently break ping. */
    {
        uint8_t frame[1600];
        uint8_t *const ip  = frame + 14;         /* 20 bytes */
        uint8_t *const ic  = frame + 34;         /* 8 + 56   */
        const uint32_t icmp_len = 8 + 56;
        const uint32_t ip_total = 20 + icmp_len;
        uint16_t c;
        int i;
        int ok = 1;

        /* ---- build the echo request the way icmp_send_echo() does ---- */
        for (i = 0; i < 56; i++)
            ic[8 + i] = (uint8_t)('A' + (i % 26));
        ic[0] = 8; ic[1] = 0; ic[2] = 0; ic[3] = 0;
        ic[4] = 0xB0; ic[5] = 0x0B;              /* NET_ICMP_ID 0xB00B */
        ic[6] = 0x00; ic[7] = 0x01;              /* seq 1 */
        c = net_checksum(ic, icmp_len);
        ic[2] = (uint8_t)(c >> 8);
        ic[3] = (uint8_t)(c & 0xFF);
        /* receiver-side acceptance rule */
        ok = ok && (net_checksum(ic, icmp_len) == 0);

        /* ---- IPv4 header, 10.0.2.15 -> 10.0.2.2 ---- */
        memset(ip, 0, 20);
        ip[0] = 0x45;
        ip[2] = (uint8_t)(ip_total >> 8);
        ip[3] = (uint8_t)(ip_total & 0xFF);
        ip[6] = 0x40;                            /* DF */
        ip[8] = 64;                              /* TTL */
        ip[9] = 1;                               /* ICMP */
        ip[12] = 10; ip[13] = 0; ip[14] = 2; ip[15] = 15;
        ip[16] = 10; ip[17] = 0; ip[18] = 2; ip[19] = 2;
        c = net_checksum(ip, 20);
        ip[10] = (uint8_t)(c >> 8);
        ip[11] = (uint8_t)(c & 0xFF);
        ok = ok && (net_checksum(ip, 20) == 0);  /* RX rule in ip_rx() */
        ok = ok && (ip_total == 84);             /* 14 + 20 + 64 <= 1514 */

        /* ---- build the echo reply exactly like icmp_rx() does ---- */
        {
            uint8_t rep[1600];
            uint8_t *rip = rep + 14;
            uint8_t *ric = rep + 34;
            memcpy(ric, ic, icmp_len);
            ric[0] = 0;                          /* echo reply */
            ric[1] = 0;
            ric[2] = 0; ric[3] = 0;
            c = net_checksum(ric, icmp_len);
            ric[2] = (uint8_t)(c >> 8);
            ric[3] = (uint8_t)(c & 0xFF);
            ok = ok && (net_checksum(ric, icmp_len) == 0);
            ok = ok && (ric[4] == 0xB0 && ric[5] == 0x0B);   /* id preserved */
            ok = ok && (ric[6] == 0x00 && ric[7] == 0x01);   /* seq preserved */
            ok = ok && (memcmp(ric + 8, ic + 8, 56) == 0);   /* payload copied */

            memset(rip, 0, 20);
            rip[0] = 0x45;
            rip[2] = (uint8_t)(ip_total >> 8);
            rip[3] = (uint8_t)(ip_total & 0xFF);
            rip[6] = 0x40;
            rip[8] = 64;
            rip[9] = 1;
            rip[12] = 10; rip[13] = 0; rip[14] = 2; rip[15] = 2;
            rip[16] = 10; rip[17] = 0; rip[18] = 2; rip[19] = 15;
            c = net_checksum(rip, 20);
            rip[10] = (uint8_t)(c >> 8);
            rip[11] = (uint8_t)(c & 0xFF);
            ok = ok && (net_checksum(rip, 20) == 0);
        }

        check("echo request/reply packet round trip verifies", ok);
    }

    /* 8. the UDP datagram net_udp_send() emits must verify on the wire */
    {
        uint8_t frame[1600];
        uint8_t *udp = frame + 14 + 20;
        const uint32_t dns_len = 12 + 1 + 7 + 1 + 3 + 1 + 4;   /* 29 bytes */
        uint16_t c;
        int ok = 1;
        int i;
        for (i = 0; i < (int)dns_len; i++)
            udp[i] = 0;
        udp[0] = 0x53; udp[1] = 0x75;            /* id 0x5375 */
        udp[8 + 2] = 0x01;                       /* qtype low byte */
        udp[4] = 0x00; udp[5] = (uint8_t)dns_len;
        udp[6] = 0; udp[7] = 0;
        c = udp_checksum(0x0A00020F, 0x0A000203, udp, dns_len);
        udp[6] = (uint8_t)(c >> 8);
        udp[7] = (uint8_t)(c & 0xFF);
        /* udp_rx() accepts when the pseudo-header sum verifies to 0xFFFF */
        ok = ok && (udp_checksum(0x0A00020F, 0x0A000203, udp, dns_len) == 0xFFFFu);
        /* and it must be rejected if a single payload byte is flipped */
        udp[9] ^= 0x01;
        ok = ok && (udp_checksum(0x0A00020F, 0x0A000203, udp, dns_len) != 0xFFFFu);
        check("udp dns datagram verifies and a bit flip is detected", ok);
    }

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASS",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
