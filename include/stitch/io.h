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

/* 32-bit port I/O. These MUST be real dword accesses, never two 16-bit halves:
 * the RTL8139 (and other MMIO/register-file devices such as the PCI config
 * address port 0xCF8, the AHCI/NVMe BARs and the 8139 transmit doorbell) only
 * implement 32-bit register semantics, so a pair of outw() calls is silently
 * dropped or lands in the wrong register. Missing these two helpers is exactly
 * why the RTL8139 transmit doorbell never fired and every NIC frame was lost. */
static inline void outl(uint16_t port, uint32_t val)
{
    __asm__ volatile("outl %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint32_t inl(uint16_t port)
{
    uint32_t ret;
    __asm__ volatile("inl %1, %0" : "=a"(ret) : "Nd"(port));
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
