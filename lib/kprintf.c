/* StitchOS (缝合怪系统) - minimal kernel printf
 * Uses compiler builtins only (no libc, no headers).
 * Formatting approach kept deliberately small: %s %c %d %u %x %p %%.
 */
#include <stitch/console.h>

typedef __builtin_va_list va_list;
#define va_start(v, l) __builtin_va_start(v, l)
#define va_arg(v, t)   __builtin_va_arg(v, t)
#define va_end(v)      __builtin_va_end(v)

static void print_unsigned(uint64_t v, unsigned base, int width, char pad)
{
    char buf[24];
    int i = 0;
    do {
        unsigned d = (unsigned)(v % base);
        buf[i++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
        v /= base;
    } while (v);
    while (i < width && i < (int)sizeof(buf))
        buf[i++] = pad;
    while (i--)
        console_putc(buf[i]);
}

void console_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);

    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            console_putc(*fmt);
            continue;
        }
        fmt++;
        char pad = ' ';
        int width = 0;
        if (*fmt == '0') { pad = '0'; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }
        /* length modifiers: accept and ignore l / ll (numeric args are passed as 64-bit) */
        while (*fmt == 'l') { fmt++; }

        switch (*fmt) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            while (*s) console_putc(*s++);
            break;
        }
        case 'c':
            console_putc((char)va_arg(ap, int));
            break;
        case 'd': {
            int64_t v = va_arg(ap, int64_t);   /* callers must pass int64_t/uint64_t */
            if (v < 0) { console_putc('-'); v = -v; }
            print_unsigned((uint64_t)v, 10, width, pad);
            break;
        }
        case 'u':
            print_unsigned((uint64_t)va_arg(ap, uint64_t), 10, width, pad);
            break;
        case 'x':
            print_unsigned((uint64_t)va_arg(ap, uint64_t), 16, width, pad);
            break;
        case 'p':
            console_puts("0x");
            print_unsigned((uint64_t)va_arg(ap, uint64_t), 16, 16, '0');
            break;
        case '%':
            console_putc('%');
            break;
        default:
            console_putc('%');
            console_putc(*fmt);
            break;
        }
    }
    va_end(ap);
}
