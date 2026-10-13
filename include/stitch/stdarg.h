/* StitchOS (缝合怪系统) - freestanding stdarg interface */
#ifndef STITCH_STDARG_H
#define STITCH_STDARG_H

/* Minimal stdarg implementation for x86_64 */
typedef __builtin_va_list va_list;

#define va_start(v, l) __builtin_va_start(v, l)
#define va_end(v) __builtin_va_end(v)
#define va_arg(v, l) __builtin_va_arg(v, l)
#define va_copy(d, s) __builtin_va_copy(d, s)

#endif /* STITCH_STDARG_H */