/* StitchOS (缝合怪系统) - COM1 16550 UART early console
 * Initialization sequence per the classic 16550 bring-up
 * (divisor latch, 8N1, FIFO on) as seen in several of the
 * source kernels (Phobos kernel/dev/chr/ser.cpp, CoolPotOS
 * arch driver ns16550.c, VimtuOS dbg64 serial path).
 */
#include <stitch/io.h>
#include <stitch/console.h>

#define COM1 0x3F8

#define REG_DATA    (COM1 + 0)
#define REG_IER     (COM1 + 1)
#define REG_FCR     (COM1 + 2)
#define REG_LCR     (COM1 + 3)
#define REG_MCR     (COM1 + 4)
#define REG_LSR     (COM1 + 5)

#define LCR_DLAB    0x80
#define LCR_8N1     0x03
#define FCR_ENABLE  0x01
#define FCR_CLEAR   0x06
#define MCR_DTR_RTS 0x03
#define LSR_THRE    0x20

void console_init(void)
{
    outb(REG_IER, 0x00);                /* no interrupts yet */
    outb(REG_LCR, LCR_DLAB);            /* unlock divisor    */
    outb(REG_DATA, 0x01);               /* divisor 1 -> 115200 */
    outb(REG_IER, 0x00);
    outb(REG_LCR, LCR_8N1);             /* 8 bits, no parity, 1 stop */
    outb(REG_FCR, FCR_ENABLE | FCR_CLEAR);
    outb(REG_MCR, MCR_DTR_RTS);
}

void console_putc(char c)
{
    if (c == '\n')
        console_putc('\r');
    while (!(inb(REG_LSR) & LSR_THRE))
        ;
    outb(REG_DATA, (uint8_t)c);
}

void console_puts(const char *s)
{
    while (*s)
        console_putc(*s++);
}
