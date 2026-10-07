/* StitchOS (缝合怪系统) - basic freestanding types */
#ifndef STITCH_TYPES_H
#define STITCH_TYPES_H

typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long long uint64_t;
typedef signed char        int8_t;
typedef short              int16_t;
typedef int                int32_t;
typedef long long          int64_t;

typedef uint64_t uintptr_t;
typedef int64_t  intptr_t;
typedef uint64_t size_t;
typedef int64_t  ssize_t;
typedef int64_t  ptrdiff_t;

#define NULL ((void *)0)

typedef _Bool bool;
#define true  1
#define false 0

#define UINT64_MAX 0xFFFFFFFFFFFFFFFFULL

#define offsetof(type, member) ((size_t)&(((type *)0)->member))

#endif /* STITCH_TYPES_H */
