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

/* Screen enable switch. The GUI turns this off while it owns the framebuffer so
 * that kernel log output (which still goes to COM1) cannot paint over the
 * desktop; the terminal is suspended rather than the logging being disabled. */
static int g_screen_enabled = 1;
void console_screen_enable(int on) { g_screen_enabled = on ? 1 : 0; }
int  console_screen_enabled(void) { return g_screen_enabled; }

/* SutureOS console sink.
 *
 * Every character goes to COM1 (so automated boots always have a complete
 * log) AND to the screen: the framebuffer terminal (flanterm, black background
 * with white text) when it is available, otherwise the simple fbcon text
 * renderer. This is what makes the boot log and the shell visible on screen
 * like a Linux console while still being capturable over the serial line. */
void console_putc(char c)
{
    /* Keep the log first: capture before touching any hardware so a broken or
     * slow screen/UART never loses a line. (Captured before the CR inserted
     * below, otherwise every logged line would end with a stray '\r'.) */
    extern void log_ring_putc(char c);
    log_ring_putc(c);

    /* serial next: never lose a line because of a screen problem */
    if (c == '\n') {
        while (!(inb(REG_LSR) & LSR_THRE))
            ;
        outb(REG_DATA, '\r');
    }
    while (!(inb(REG_LSR) & LSR_THRE))
        ;
    outb(REG_DATA, (uint8_t)c);

    /* screen second (skipped entirely while the GUI owns the framebuffer) */
    if (!g_screen_enabled)
        return;

    /* flanterm is fed character by character but flushed once per line (it
     * batches internally and per-character flushes mislay text). */
    extern int term_available(void);
    extern void term_putc(char c);
    extern void term_refresh(void);
    extern void fbcon_putc(char c);
    if (term_available()) {
        term_putc(c);
        if (c == '\n')
            term_refresh();
    } else {
        fbcon_putc(c);
    }
}

void console_puts(const char *s)
{
    while (*s)
        console_putc(*s++);
}
