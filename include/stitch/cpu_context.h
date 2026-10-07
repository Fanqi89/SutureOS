/* SutureOS module: CPU context structure (cinux_ prefix)
 * Ported from: Cinux (C:\Users\fanqi\Desktop\自研操作系统\Cinux\kernel\arch\x86_64\include\context.h)
 * Original license: GPL-3.0
 * Changes: layout locked by _Static_assert; no rdi field (asm trampoline
 *          reads tasks[current].arg to set rdi); 11 fields, 96 bytes total.
 */
#ifndef STITCH_CPU_CONTEXT_H
#define STITCH_CPU_CONTEXT_H

#include <stitch/types.h>

typedef struct cinux_cpu_context {
    uint64_t r15;
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t rbp;
    uint64_t rbx;
    uint64_t rsp;
    uint64_t rip;
    uint64_t rflags;
    uint64_t kgs_base;
    uint64_t fs_base;
    uint64_t _pad;  /* padding to reach 96 bytes for 16-byte alignment */
} cinux_cpu_context_t;

_Static_assert(offsetof(cinux_cpu_context_t, r15)      == 0,  "r15 offset");
_Static_assert(offsetof(cinux_cpu_context_t, r14)      == 8,  "r14 offset");
_Static_assert(offsetof(cinux_cpu_context_t, r13)      == 16, "r13 offset");
_Static_assert(offsetof(cinux_cpu_context_t, r12)      == 24, "r12 offset");
_Static_assert(offsetof(cinux_cpu_context_t, rbp)      == 32, "rbp offset");
_Static_assert(offsetof(cinux_cpu_context_t, rbx)      == 40, "rbx offset");
_Static_assert(offsetof(cinux_cpu_context_t, rsp)      == 48, "rsp offset");
_Static_assert(offsetof(cinux_cpu_context_t, rip)      == 56, "rip offset");
_Static_assert(offsetof(cinux_cpu_context_t, rflags)   == 64, "rflags offset");
_Static_assert(offsetof(cinux_cpu_context_t, kgs_base) == 72, "kgs_base offset");
_Static_assert(offsetof(cinux_cpu_context_t, fs_base)  == 80, "fs_base offset");
_Static_assert(sizeof(cinux_cpu_context_t) == 96, "context size");

/* Context switch function (implemented in context_switch.S). */
void cinux_context_switch(cinux_cpu_context_t *from, cinux_cpu_context_t *to);

#endif /* STITCH_CPU_CONTEXT_H */
