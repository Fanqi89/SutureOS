/* StitchOS (缝合怪系统) - x86 port I/O helpers
 * Zero-dependency inline in/out (inspired by VimtuOS kernel/port.h,
 * GPL-3.0 - same license as this project).
 */
#ifndef STITCH_IO_H
#define STITCH_IO_H

#include <stitch/types.h>

static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outw(uint16_t port, uint16_t val)
{
    __asm__ volatile("outw %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint16_t inw(uint16_t port)
{
    uint16_t ret;
    __asm__ volatile("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void io_wait(void)
{
    outb(0x80, 0);
}

static inline void cpu_cli(void) { __asm__ volatile("cli"); }
static inline void cpu_sti(void) { __asm__ volatile("sti"); }
static inline void cpu_hlt(void) { __asm__ volatile("hlt"); }

#endif /* STITCH_IO_H */
