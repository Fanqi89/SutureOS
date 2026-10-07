/* StitchOS (缝合怪系统) - freestanding string/memory interface */
#ifndef STITCH_STRING_H
#define STITCH_STRING_H

#include <stitch/types.h>

void *memset(void *dst, int c, size_t n);
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
int   memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
char  *strcpy(char *dst, const char *src);
char  *strncpy(char *dst, const char *src, size_t n);
char  *strrchr(const char *s, int c);

#endif /* STITCH_STRING_H */
