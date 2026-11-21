/* SutureOS: memfd + MAP_SHARED implementation. See memfd.h. */
#include "memfd.h"

#include <stitch/console.h>
#include <stitch/string.h>
#include "mmu.h"          /* mmu_walk, loader_alloc_page, PTE flags, read_cr3 */
#include "loader.h"       /* loader_free_page: recycle frames of an unref'd object */

#define EINVAL_ 22
#define ENOMEM_ 12
#define EAGAIN_ 11

typedef struct {
    int    in_use;
    int    refs;       /* v0.62: references (creator + every SCM_RIGHTS receiver) */
    int    mapped;     /* v0.62: 1 once a mapping exists (pages must stay alive)  */
    int    pages;
    void  *page[MEMFD_MAX_PAGES];
} memfd_t;

static memfd_t g_fds[MEMFD_MAX_OBJECTS];

static int memfd_decode(int fd, int *slot)
{
    const int rel = fd - MEMFD_FD_BASE;
    if (rel < 0 || rel >= MEMFD_MAX_OBJECTS) return -1;
    if (!g_fds[rel].in_use) return -1;
    *slot = rel;
    return 0;
}

int memfd_is_fd(int fd) { int s; return memfd_decode(fd, &s) == 0; }

/* Size of the reference-counting surface: just enough for the SCM_RIGHTS path. */
int memfd_live_count(void)
{
    int i, n = 0;

    for (i = 0; i < MEMFD_MAX_OBJECTS; i++)
        if (g_fds[i].in_use) n++;
    return n;
}

int memfd_ref(int fd)
{
    int slot;

    if (memfd_decode(fd, &slot) != 0) return -EINVAL_;
    g_fds[slot].refs++;
    return g_fds[slot].refs;
}

int memfd_unref(int fd)
{
    int slot, i;

    if (memfd_decode(fd, &slot) != 0) return -EINVAL_;
    if (g_fds[slot].refs > 1) return --g_fds[slot].refs;
    /* Last reference: release the slot.  Frames are recycled only when nothing
     * maps the object any more (a live mapping would keep touching them). */
    if (!g_fds[slot].mapped) {
        for (i = 0; i < g_fds[slot].pages; i++)
            loader_free_page(g_fds[slot].page[i]);
    }
    memset(&g_fds[slot], 0, sizeof(g_fds[slot]));
    return 0;
}

int memfd_create_obj(void)
{
    int slot;
    for (slot = 0; slot < MEMFD_MAX_OBJECTS; slot++) {
        if (!g_fds[slot].in_use) break;
    }
    if (slot >= MEMFD_MAX_OBJECTS) return -EAGAIN_;
    memset(&g_fds[slot], 0, sizeof(g_fds[slot]));
    g_fds[slot].in_use = 1;
    g_fds[slot].refs = 1;
    return MEMFD_FD_BASE + slot;
}

int memfd_resize(int fd, uint64_t bytes)
{
    int slot;
    int want;
    if (memfd_decode(fd, &slot) != 0) return -EINVAL_;
    want = (int)((bytes + 4095) / 4096);
    if (want < 0 || want > MEMFD_MAX_PAGES) return -EINVAL_;
    while (g_fds[slot].pages < want) {
        void *p = loader_alloc_page();
        if (!p) return -ENOMEM_;
        memset(p, 0, 4096);
        g_fds[slot].page[g_fds[slot].pages++] = p;
    }
    return 0;
}

int memfd_size(int fd)
{
    int slot;
    if (memfd_decode(fd, &slot) != 0) return -EINVAL_;
    return g_fds[slot].pages * 4096;
}

void *memfd_page(int fd, int idx)
{
    int slot;
    if (memfd_decode(fd, &slot) != 0) return NULL;
    if (idx < 0 || idx >= g_fds[slot].pages) return NULL;
    return g_fds[slot].page[idx];
}

static void memfd_flush_tlb(void)
{
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
}

int memfd_map(uint64_t vaddr, int fd, uint64_t len, int writable)
{
    int slot;
    uint64_t n, i;
    uint64_t *cr3 = (uint64_t *)(uintptr_t)read_cr3();
    /* PTE_MEMFD (software bit 9): marks this PTE as aliasing a memfd-owned page
     * so munmap clears the mapping without recycling the frame into the process
     * frame pool (the memfd still hands it to other mappings). */
    uint64_t flags = PTE_P | PTE_U | (writable ? PTE_W : 0) | (1ull << 9);

    if (memfd_decode(fd, &slot) != 0) return -EINVAL_;
    if ((vaddr & 0xFFF) != 0 || len == 0) return -EINVAL_;
    n = (len + 4095) / 4096;
    if ((int)n > g_fds[slot].pages) return -EINVAL_;
    for (i = 0; i < n; i++) {
        uint64_t va = vaddr + i * 4096;
        uint64_t *pte = mmu_walk(cr3, 0, va, 1);
        if (!pte) return -ENOMEM_;
        *pte = (uint64_t)(uintptr_t)g_fds[slot].page[i] | flags;
    }
    g_fds[slot].mapped = 1;          /* keep the frames alive across a close */
    memfd_flush_tlb();
    return 0;
}

int memfd_unmap(uint64_t vaddr, uint64_t len)
{
    uint64_t n = (len + 4095) / 4096;
    uint64_t *cr3 = (uint64_t *)(uintptr_t)read_cr3();
    for (uint64_t i = 0; i < n; i++) {
        uint64_t *pte = mmu_walk(cr3, 0, vaddr + i * 4096, 1);
        if (pte) *pte = 0;
    }
    memfd_flush_tlb();
    return 0;
}

int memfd_write(int fd, uint64_t off, const void *src, uint64_t len)
{
    const uint8_t *s = (const uint8_t *)src;
    uint64_t done = 0;
    int size = memfd_size(fd);
    if (size < 0) return size;
    while (done < len && (int)(off + done) < size) {
        const uint64_t pos = off + done;
        uint8_t *pg = (uint8_t *)memfd_page(fd, (int)(pos / 4096));
        const uint64_t in_pg = pos % 4096;
        uint64_t chunk = 4096 - in_pg;
        if (chunk > len - done) chunk = len - done;
        memcpy(pg + in_pg, s + done, chunk);
        done += chunk;
    }
    return (int)done;
}

int memfd_read(int fd, uint64_t off, void *dst, uint64_t len)
{
    uint8_t *d = (uint8_t *)dst;
    uint64_t done = 0;
    int size = memfd_size(fd);
    if (size < 0) return size;
    while (done < len && (int)(off + done) < size) {
        const uint64_t pos = off + done;
        const uint8_t *pg = (const uint8_t *)memfd_page(fd, (int)(pos / 4096));
        const uint64_t in_pg = pos % 4096;
        uint64_t chunk = 4096 - in_pg;
        if (chunk > len - done) chunk = len - done;
        memcpy(d + done, pg + in_pg, chunk);
        done += chunk;
    }
    return (int)done;
}

/* ------------------------------------------------------------------ selftest */

static int g_ok, g_fail;

static void shm_check(int cond, const char *what)
{
    if (cond) { g_ok++; console_printf("[shm]   ok   %s\n", what); }
    else      { g_fail++; console_printf("[shm]   FAIL %s\n", what); }
}

#define SHM_VA_A 0x50000000ull
#define SHM_VA_B 0x50800000ull
#define SHM_LEN  12288ull    /* 3 pages */

int memfd_selftest(void)
{
    int fd;
    uint8_t pat[SHM_LEN];
    volatile uint8_t *a;
    volatile uint8_t *b;
    int diff = 0;

    g_ok = g_fail = 0;
    console_printf("[shm] memfd + MAP_SHARED selftest\n");

    fd = memfd_create_obj();
    shm_check(fd >= MEMFD_FD_BASE, "memfd_create_obj() returns an fd");
    if (fd < 0) { console_printf("[shm] selftest: %d ok, %d FAIL\n", g_ok, g_fail); return -1; }
    shm_check(memfd_is_fd(fd), "the fd is recognised as a memfd");

    shm_check(memfd_resize(fd, SHM_LEN) == 0, "ftruncate to 3 pages succeeds");
    shm_check(memfd_size(fd) == (int)SHM_LEN, "the object is 12288 bytes");
    shm_check(memfd_page(fd, 0) != NULL && memfd_page(fd, 2) != NULL,
              "all three pages are backed");
    shm_check(memfd_page(fd, 3) == NULL, "page 3 is not backed (no overrun)");

    shm_check(memfd_map(SHM_VA_A, fd, SHM_LEN, 1) == 0, "first MAP_SHARED succeeds");
    shm_check(memfd_map(SHM_VA_B, fd, SHM_LEN, 1) == 0, "second MAP_SHARED succeeds");

    for (uint64_t i = 0; i < SHM_LEN; i++) pat[i] = (uint8_t)(i * 7u + 3u);

    /* write through mapping A, read back through mapping B: same frames */
    a = (volatile uint8_t *)(uintptr_t)SHM_VA_A;
    b = (volatile uint8_t *)(uintptr_t)SHM_VA_B;
    for (uint64_t i = 0; i < SHM_LEN; i++) a[i] = pat[i];
    for (uint64_t i = 0; i < SHM_LEN; i++) {
        if (b[i] != pat[i]) diff++;
    }
    shm_check(diff == 0, "writes through map A are visible through map B (shared frames)");

    /* and the reverse direction */
    b[100] = 0xA5;
    shm_check(a[100] == 0xA5, "writes through map B are visible through map A");

    /* the object's own page view agrees with the mappings */
    shm_check(((const uint8_t *)memfd_page(fd, 0))[100] == 0xA5,
              "the kernel-side page holds the same byte");

    /* offset-based helpers agree too */
    {
        uint8_t probe = 0;
        (void)memfd_read(fd, 100, &probe, 1);
        shm_check(probe == 0xA5, "memfd_read() sees the mapped write");
    }

    shm_check(memfd_unmap(SHM_VA_A, SHM_LEN) == 0, "unmap A succeeds");
    shm_check(memfd_unmap(SHM_VA_B, SHM_LEN) == 0, "unmap B succeeds");

    console_printf("[shm] memfd selftest: %d ok, %d FAIL\n", g_ok, g_fail);
    return g_fail == 0 ? 0 : -1;
}
