#ifndef STITCH_LOADER_H
#define STITCH_LOADER_H

#include <stitch/types.h>

int elf_load_user(const void *buf, size_t len,
                  uint64_t *out_entry, uint64_t *out_base, uint64_t *out_end);

int syscall_execve(uint64_t path_ptr, uint64_t argv_ptr, uint64_t envp_ptr,
                   uint64_t a4, uint64_t a5, uint64_t a6);

#endif