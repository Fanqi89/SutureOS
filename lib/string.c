/* StitchOS (缝合怪系统) - freestanding string/memory routines
 * Original glue code for the stitched kernel (GPL-3.0).
 * Required by ported modules (slab/heap/vfs expect memset/memcpy).
 */
#include <stitch/string.h>
#include <stitch/stdarg.h>

void *memset(void *dst, int c, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    while (n--)
        *d++ = (unsigned char)c;
    return dst;
}

void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n--)
        *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    if (d < s) {
        while (n--)
            *d++ = *s++;
    } else if (d > s) {
        d += n;
        s += n;
        while (n--)
            *--d = *--s;
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = (const unsigned char *)a;
    const unsigned char *y = (const unsigned char *)b;
    while (n--) {
        if (*x != *y)
            return (int)*x - (int)*y;
        x++;
        y++;
    }
    return 0;
}

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

char *strcpy(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++) != '\0')
        ;
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n)
{
    char *d = dst;
    while (n && (*d = *src) != '\0') {
        d++;
        src++;
        n--;
    }
    while (n--) {
        *d++ = '\0';
    }
    return dst;
}

char *strrchr(const char *s, int c)
{
    const char *last = NULL;
    while (*s) {
        if (*s == (char)c)
            last = s;
        s++;
    }
    if (c == '\0')
        return (char *)s;
    return (char *)last;
}

int strncmp(const char *a, const char *b, size_t n)
{
    while (n && *a && *a == *b) {
        a++;
        b++;
        n--;
    }
    if (n == 0) return 0;
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

char *strcat(char *dst, const char *src)
{
    char *d = dst;
    while (*d) d++;
    while ((*d++ = *src++) != '\0')
        ;
    return dst;
}

char *strncat(char *dst, const char *src, size_t n)
{
    char *d = dst;
    while (*d) d++;
    while (n && (*d = *src) != '\0') {
        d++;
        src++;
        n--;
    }
    *d = '\0';
    return dst;
}

char *strchr(const char *s, int c)
{
    while (*s) {
        if (*s == (char)c)
            return (char *)s;
        s++;
    }
    if (c == '\0')
        return (char *)s;
    return NULL;
}

/* Simple strtok implementation using static buffer */
static char *g_strtok_save = NULL;

char *strtok(char *str, const char *delim)
{
    if (str == NULL)
        str = g_strtok_save;
    
    if (str == NULL)
        return NULL;
    
    /* Skip leading delimiters */
    while (*str && strchr(delim, *str))
        str++;
    
    if (*str == '\0') {
        g_strtok_save = NULL;
        return NULL;
    }
    
    char *token = str;
    
    /* Find end of token */
    while (*str && !strchr(delim, *str))
        str++;
    
    if (*str) {
        *str++ = '\0';
        g_strtok_save = str;
    } else {
        g_strtok_save = NULL;
    }
    
    return token;
}

/* ==================== printf family ==================== */

static int printf_putc(void *buf, char c) {
    char **p = (char **)buf;
    *(*p)++ = c;
    return 1;
}

static int printf_puts(void *buf, const char *s, size_t len) {
    char **p = (char **)buf;
    for (size_t i = 0; i < len; i++) {
        *(*p)++ = s[i];
    }
    return len;
}

int vsnprintf(char *str, size_t size, const char *fmt, va_list args) {
    if (size == 0) return 0;
    
    char *end = str + size - 1;
    char *p = str;
    const char *f = fmt;
    
    while (*f && p < end) {
        if (*f != '%') {
            *p++ = *f++;
            continue;
        }
        f++;
        if (!*f) break;

        /* flags / width / length: supports [-][width][l|ll|z]<conv> */
        bool left = false;
        int width = 0;
        int longs = 0;
        if (*f == '-') { left = true; f++; }
        while (*f >= '0' && *f <= '9') { width = width * 10 + (*f - '0'); f++; }
        while (*f == 'l') { longs++; f++; }
        if (*f == 'z') { longs = 1; f++; }

        char spec = *f;
        char out[24];
        int len = 0;
        unsigned long uval = 0;
        bool neg = false;

        switch (spec) {
            case 's': {
                const char *s = va_arg(args, const char *);
                if (!s) s = "(null)";
                int slen = (int)strlen(s);
                int pad = width - slen;
                if (!left) { while (pad-- > 0 && p < end) *p++ = ' '; }
                while (*s && p < end) *p++ = *s++;
                if (left) { while (pad-- > 0 && p < end) *p++ = ' '; }
                f++;
                continue;
            }
            case 'c': {
                out[len++] = (char)va_arg(args, int);
                break;
            }
            case 'd': case 'i': {
                long sval = longs ? va_arg(args, long) : (long)va_arg(args, int);
                if (sval < 0) { neg = true; uval = (unsigned long)(-sval); }
                else uval = (unsigned long)sval;
                char tb[20]; int ti = 0;
                if (uval == 0) tb[ti++] = '0';
                while (uval) { tb[ti++] = (char)('0' + uval % 10); uval /= 10; }
                while (ti-- > 0 && len < (int)sizeof(out)) out[len++] = tb[ti];
                break;
            }
            case 'u': {
                uval = longs ? va_arg(args, unsigned long) : (unsigned long)va_arg(args, unsigned int);
                char tb[20]; int ti = 0;
                if (uval == 0) tb[ti++] = '0';
                while (uval) { tb[ti++] = (char)('0' + uval % 10); uval /= 10; }
                while (ti-- > 0 && len < (int)sizeof(out)) out[len++] = tb[ti];
                break;
            }
            case 'x': case 'p': {
                uval = (longs || spec == 'p') ? va_arg(args, unsigned long)
                                              : (unsigned long)va_arg(args, unsigned int);
                char tb[20]; int ti = 0;
                if (uval == 0) tb[ti++] = '0';
                while (uval) { int d = (int)(uval & 0xF);
                    tb[ti++] = (char)((d < 10) ? '0' + d : 'a' + d - 10);
                    uval >>= 4; }
                while (ti-- > 0 && len < (int)sizeof(out)) out[len++] = tb[ti];
                break;
            }
            case '%': {
                *p++ = '%';
                f++;
                continue;
            }
            default: {
                *p++ = '%';
                *p++ = spec;
                f++;
                continue;
            }
        }

        /* numeric padding: sign/digits/0x prefix count toward width */
        int prefix = neg ? 1 : (spec == 'p' ? 2 : 0);
        int pad = width - len - prefix;
        if (!left) { while (pad-- > 0 && p < end) *p++ = ' '; }
        if (neg && p < end) *p++ = '-';
        if (spec == 'p' && p + 1 < end) { *p++ = '0'; *p++ = 'x'; }
        for (int i = 0; i < len && p < end; i++) *p++ = out[i];
        if (left) { while (pad-- > 0 && p < end) *p++ = ' '; }
        f++;
    }
    *p = '\0';
    return p - str;
}

int snprintf(char *str, size_t size, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int ret = vsnprintf(str, size, fmt, args);
    va_end(args);
    return ret;
}

int sprintf(char *str, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    /* Note: no size limit - caller must ensure buffer is large enough */
    int ret = vsnprintf(str, 0x7FFFFFFF, fmt, args);
    va_end(args);
    return ret;
}
