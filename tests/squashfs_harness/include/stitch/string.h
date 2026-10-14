/* Host-harness shadow of include/stitch/string.h (see types.h next to it). */
#ifndef STITCH_STRING_H
#define STITCH_STRING_H

#include <stitch/types.h>
#include <string.h>

int vsnprintf(char *str, size_t size, const char *fmt, va_list args);
int snprintf(char *str, size_t size, const char *fmt, ...);
int sprintf(char *str, const char *fmt, ...);

#endif /* STITCH_STRING_H */
