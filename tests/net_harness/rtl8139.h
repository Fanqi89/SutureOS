/* Test-only shadow of "rtl8139.h" (kernel/drivers/rtl8139.h).
 *
 * It mirrors the real header's diagnostics constants and API, but every driver
 * entry point is renamed to TEST_rtl8139_* so the harness can substitute a
 * scripted NIC. net.c is compiled unmodified against this header, which means
 * the protocol code under test is the shipped kernel/net/net.c.
 */
#ifndef TEST_RTL8139_H
#define TEST_RTL8139_H

#include <stitch/types.h>

#define RTL8139_ERR_NONE         0
#define RTL8139_ERR_NOT_FOUND   -1
#define RTL8139_ERR_BAR          2
#define RTL8139_ERR_RESET        3
#define RTL8139_ERR_IO_BASE      4
#define RTL8139_ERR_NO_BUFFERS   5
#define RTL8139_ERR_CMD          6

int      TEST_rtl8139_init(void);
int      TEST_rtl8139_present(void);
int      TEST_rtl8139_get_mac(uint8_t mac_out[6]);
uint8_t  TEST_rtl8139_irq(void);
uint16_t TEST_rtl8139_io_base(void);
int      TEST_rtl8139_send_frame(const void *frame, uint32_t len);
int      TEST_rtl8139_receive(void (*fn)(const uint8_t *frame, uint32_t len));
uint16_t TEST_rtl8139_last_isr(void);
uint32_t TEST_rtl8139_rx_drops(void);
uint64_t TEST_rtl8139_tx_frames(void);
uint64_t TEST_rtl8139_rx_frames(void);
void     TEST_st_rtl8139_test(void);

#define rtl8139_init        TEST_rtl8139_init
#define rtl8139_present     TEST_rtl8139_present
#define rtl8139_get_mac     TEST_rtl8139_get_mac
#define rtl8139_irq         TEST_rtl8139_irq
#define rtl8139_io_base     TEST_rtl8139_io_base
#define rtl8139_send_frame  TEST_rtl8139_send_frame
#define rtl8139_receive     TEST_rtl8139_receive
#define rtl8139_last_isr    TEST_rtl8139_last_isr
#define rtl8139_rx_drops    TEST_rtl8139_rx_drops
#define rtl8139_tx_frames   TEST_rtl8139_tx_frames
#define rtl8139_rx_frames   TEST_rtl8139_rx_frames
#define st_rtl8139_test     TEST_st_rtl8139_test

#define RTL8139_TEST_IO_BASE 0xC000u
#define RTL8139_TEST_IRQ     11u

#endif /* TEST_RTL8139_H */
