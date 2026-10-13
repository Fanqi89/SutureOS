/* Independent check: run a byte-for-byte copy of net.c's net_checksum() over
 * the literal frames captured from the kernel, and print the value the field
 * MUST hold for the checksum to verify. */
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

/* The exact 98-byte ICMP echo reply the kernel produced for an inbound echo
 * request (payload 0xaa x56, id 0x1234, seq 7), from the probe hex dump. */
static uint8_t mk_reply(uint8_t *f)
{
    uint32_t i;
    memset(f, 0xaa, 98);
    /* ethernet */
    f[0]=0x52; f[1]=0x55; f[2]=0x0a; f[3]=0x00; f[4]=0x02; f[5]=0x02;
    f[6]=0x52; f[7]=0x54; f[8]=0x00; f[9]=0x12; f[10]=0x34; f[11]=0x56;
    f[12]=0x08; f[13]=0x00;
    /* ipv4 */
    f[14]=0x45; f[15]=0x00; f[16]=0x00; f[17]=0x54;
    f[18]=0x4f; f[19]=0x01; f[20]=0x40; f[21]=0x00;
    f[22]=0x40; f[23]=0x01;
    f[24]=0x8a; f[25]=0xd3;                 /* the checksum under test */
    f[26]=0x0a; f[27]=0x00; f[28]=0x02; f[29]=0x0f;
    f[30]=0x0a; f[31]=0x00; f[32]=0x02; f[33]=0x0f;
    /* icmp */
    f[34]=0x00; f[35]=0x00;
    f[36]=0x1a; f[37]=0x43;                 /* the checksum under test */
    f[38]=0x12; f[39]=0x34;
    f[40]=0x00; f[41]=0x07;
    for (i = 0; i < 56u; i++)
        f[42 + i] = 0xaa;
    return 0;
}

int main(void)
{
    uint8_t f[98];
    uint8_t tmp[98];
    uint16_t ip_field, icmp_field, ip_want, icmp_want;

    mk_reply(f);
    ip_field = (uint16_t)((f[24] << 8) | f[25]);
    icmp_field = (uint16_t)((f[36] << 8) | f[37]);

    printf("ip   field on the wire = 0x%04x\n", ip_field);
    printf("ip   verify            = 0x%04x  (0 == consistent)\n", net_checksum(f + 14, 20));
    memcpy(tmp, f, 98);
    tmp[24] = 0; tmp[25] = 0;
    ip_want = net_checksum(tmp + 14, 20);
    printf("ip   required value    = 0x%04x\n", ip_want);
    printf("ip   consistent?       = %s\n", (ip_field == ip_want) ? "YES" : "NO");

    printf("icmp field on the wire = 0x%04x\n", icmp_field);
    printf("icmp verify            = 0x%04x  (0 == consistent)\n", net_checksum(f + 34, 64));
    memcpy(tmp, f, 98);
    tmp[36] = 0; tmp[37] = 0;
    icmp_want = net_checksum(tmp + 34, 64);
    printf("icmp required value    = 0x%04x\n", icmp_want);
    printf("icmp consistent?       = %s\n", (icmp_field == icmp_want) ? "YES" : "NO");
    return 0;
}
