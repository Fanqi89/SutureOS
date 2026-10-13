/* Test-only shadow of <stitch/io.h>: port I/O becomes no-ops on the host. */
#ifndef TEST_STITCH_IO_H
#define TEST_STITCH_IO_H

#include <stitch/types.h>

static inline void outb(uint16_t port, uint8_t val) { (void)port; (void)val; }
static inline uint8_t inb(uint16_t port) { (void)port; return 0; }
static inline void outw(uint16_t port, uint16_t val) { (void)port; (void)val; }
static inline uint16_t inw(uint16_t port) { (void)port; return 0; }
static inline void io_wait(void) { }
static inline void cpu_cli(void) { }
static inline void cpu_sti(void) { }
static inline void cpu_hlt(void) { }

#endif /* TEST_STITCH_IO_H */
