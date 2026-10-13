/* Debug: run the kernel's own net_checksum() over the exact frame layout the
 * ICMP reply path uses, printing every intermediate step. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

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

int main(void)
{
    uint8_t iph[20] = {0x45,0x00,0x00,0x54,0x4f,0x01,0x40,0x00,0x40,0x01,0x8a,0xd3,
                       0x0a,0x00,0x02,0x0f,0x0a,0x00,0x02,0x0f};
    uint8_t icmp[64];
    uint16_t c;
    int i;

    printf("verify ip header as-is   = 0x%04x\n", net_checksum(iph, 20));
    {
        uint8_t z[20];
        memcpy(z, iph, 20);
        z[10] = 0; z[11] = 0;
        c = net_checksum(z, 20);
        printf("recompute ip checksum    = 0x%04x  -> stored bytes %02x %02x\n",
               c, (uint8_t)(c >> 8), (uint8_t)(c & 0xFF));
        z[10] = (uint8_t)(c >> 8); z[11] = (uint8_t)(c & 0xFF);
        printf("verify recomputed header = 0x%04x\n", net_checksum(z, 20));
    }

    /* ICMP echo reply exactly as the kernel builds it: header + 1..56 payload */
    memset(icmp, 0, sizeof(icmp));
    icmp[0] = 0; icmp[1] = 0;
    icmp[4] = 0x12; icmp[5] = 0x34;
    icmp[6] = 0x00; icmp[7] = 0x07;
    for (i = 0; i < 56; i++)
        icmp[8 + i] = (uint8_t)(i + 1);
    c = net_checksum(icmp, 64);
    printf("recompute icmp checksum  = 0x%04x  -> stored bytes %02x %02x\n",
           c, (uint8_t)(c >> 8), (uint8_t)(c & 0xFF));
    icmp[2] = (uint8_t)(c >> 8);
    icmp[3] = (uint8_t)(c & 0xFF);
    printf("verify recomputed icmp   = 0x%04x\n", net_checksum(icmp, 64));
    return 0;
}
