#ifndef STITCH_LOADER_H
#define STITCH_LOADER_H

#include <stitch/types.h>

int elf_load_user(const void *buf, size_t len,
                  uint64_t *out_entry, uint64_t *out_base, uint64_t *out_end);

int syscall_execve(uint64_t path_ptr, uint64_t argv_ptr, uint64_t envp_ptr,
                   uint64_t a4, uint64_t a5, uint64_t a6);

/* User frame pool (256 x 4 KiB). loader_free_page() takes a frame returned by
 * loader_alloc_page(); loader_pool_free_pages() reports capacity for the
 * self-test and for OOM diagnostics. */
void *loader_alloc_page(void);
void loader_free_page(void *page);
uint32_t loader_pool_free_pages(void);
void st_loader_pool_test(void);

#endif