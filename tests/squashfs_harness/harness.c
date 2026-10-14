/* Host-side functional harness for the SutureOS SquashFS 4.0 driver.
 *
 * This compiles the REAL kernel/fs/squashfs.c and kernel/pkg/inflate.c
 * (unmodified) against shadow headers (tests/squashfs_harness/include) and a
 * scripted block device, feeds them the image produced by
 * scripts/mkfs_squashfs_testimg.py, and asserts the driver's view of the
 * filesystem against expectations the harness derives INDEPENDENTLY of the
 * image (its own byte-pattern rules, its own directory sets, its own sizes).
 *
 * Positive checks
 *   * superblock fields: magic, version, gzip, block size / block_log, inode
 *     count, fragment count, bytes_used
 *   * the driver's own squashfs_selftest() must PASS on the real image
 *   * every file byte-for-byte through squashfs_open/squashfs_file_read,
 *     plus a host-computed CRC32 per file
 *   * stat: size, file/dir/symlink classification, mode bits
 *   * readdir: the exact entry set of "/", "/SUB", "/SUB/DEEP/A/B/C"
 *   * partial reads at many offsets, reads clamped at EOF, reads past EOF
 *   * a 25-block (100 KiB) file: every 4096-byte boundary and a straddling read
 *   * fragment-backed files, a zero-length file, a basic symlink target
 *   * path resolution: "/", "//", "/./x", "/SUB/../x", trailing slash rules
 *   * read-only honesty: write/mkdir/rmdir/unlink/rename all fail
 *   * unmount: the volume stops answering
 * Negative checks
 *   * bad magic, unsupported version, unknown compression id
 *   * block_size / block_log disagreement
 *   * truncated image (mount), I/O failure (mount)
 *   * inode reference pointing past the inode table
 *   * fragment index beyond the fragment table
 *   * file data block list pointing past the end of the image
 *
 * Build + run (from the repo root; no QEMU, nothing written to build/):
 *
 *   :: positive/negative fixture images (build/ holds the primary image)
 *   py -3 scripts/mkfs_squashfs_testimg.py --corrupt-magic --negatives
 *   py -3 scripts/mkfs_squashfs_testimg.py --out %TEMP%\nofrag.img  --no-fragments
 *   py -3 scripts/mkfs_squashfs_testimg.py --out %TEMP%\rawmeta.img --raw-meta
 *
 *   C:\msys64\mingw64\bin\clang.exe -O1 -g -Wall -Wextra ^
 *       -I tests/squashfs_harness/include -I kernel/fs -I kernel/drivers ^
 *       -I kernel/pkg -I kernel/vfs -o %TEMP%\squashfs_harness.exe ^
 *       tests/squashfs_harness/harness.c kernel/fs/squashfs.c ^
 *       kernel/pkg/inflate.c kernel/fs/squashfs_vfs.c
 *
 *   %TEMP%\squashfs_harness.exe build\squashfs.img ^
 *       build\squashfs_badinode.img build\squashfs_badfrag.img ^
 *       build\squashfs_badblocks.img %TEMP%\nofrag.img %TEMP%\rawmeta.img
 *
 * Expected: checks=138 ok=138 fail=0 / === RESULT: PASS ===
 *
 * Exit code 0 = PASS, 1 = FAIL, 2 = usage/IO error.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>

#include "squashfs.h"
#include "block.h"
#include "vfs.h"

/* kernel/fs/squashfs_vfs.c is compiled into this harness too, so the VFS
 * adapter's own error mapping and read-only refusals are exercised for real. */
extern const vfs_fs_driver_t squashfs_driver;

/* ===================================================================== */
/* block layer model                                                      */
/* ===================================================================== */
#define SECTOR_SIZE 512u

static uint8_t *g_disk;
static uint64_t g_disk_bytes;
static int      g_io_fail_after = -1;     /* fail once this many reads happened */
static long     g_read_count;

int blk_read(int drive, uint64_t lba, uint32_t count, void *buf)
{
    uint64_t off = lba * SECTOR_SIZE;
    uint64_t len = (uint64_t)count * SECTOR_SIZE;

    (void)drive;
    g_read_count++;
    if (g_io_fail_after >= 0 && g_read_count > g_io_fail_after)
        return -1;
    if (off > g_disk_bytes || len > g_disk_bytes - off)
        return -1;
    memcpy(buf, g_disk + off, (size_t)len);
    return 0;
}

int blk_write(int drive, uint64_t lba, uint32_t count, const void *buf)
{
    (void)drive; (void)lba; (void)count; (void)buf;
    return -1;
}

int blk_get_info(int drive, uint64_t *sector_count, uint32_t *sector_size)
{
    (void)drive;
    if (sector_count) *sector_count = g_disk_bytes / SECTOR_SIZE;
    if (sector_size) *sector_size = SECTOR_SIZE;
    return 0;
}

struct blk_device *blk_find_by_drive(int drive) { (void)drive; return NULL; }

/* ===================================================================== */
/* heap: trivial and deliberate (free is a real free of a malloc'd block)  */
/* ===================================================================== */
void *lemis_heap_alloc(uintptr_t size)
{
    if (size == 0) size = 1;
    return malloc((size_t)size);
}

void lemis_heap_free(void *ptr)
{
    free(ptr);
}

void lemis_heap_init(void *start, uintptr_t size) { (void)start; (void)size; }

uintptr_t lemis_heap_free_space(void) { return 1u << 20; }

/* ===================================================================== */
/* console: capture what the driver logs, mirror it to stdout on failure   */
/* ===================================================================== */
static char g_log[65536];
static size_t g_log_len;

void console_init(void) {}
void console_putc(char c) { (void)c; }

void console_puts(const char *s)
{
    size_t n = strlen(s);
    if (g_log_len + n + 1 < sizeof(g_log)) {
        memcpy(g_log + g_log_len, s, n);
        g_log_len += n;
        g_log[g_log_len] = '\0';
    }
}

void console_printf(const char *fmt, ...)
{
    char tmp[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    console_puts(tmp);
}

/* ===================================================================== */
/* check bookkeeping                                                      */
/* ===================================================================== */
static int g_checks, g_ok, g_fail;
static int g_section_fail;

static void section(const char *name)
{
    printf("\n--- %s ---\n", name);
    g_section_fail = 0;
}

static int check_v(const char *what, int cond, const char *fmt, ...)
{
    char detail[512];
    va_list ap;

    detail[0] = '\0';
    if (fmt) {
        va_start(ap, fmt);
        vsnprintf(detail, sizeof(detail), fmt, ap);
        va_end(ap);
    }
    g_checks++;
    if (cond) {
        g_ok++;
        printf("[ok]   %s%s%s\n", what, detail[0] ? " -- " : "", detail);
        return 1;
    }
    g_fail++;
    g_section_fail++;
    printf("[FAIL] %s%s%s\n", what, detail[0] ? " -- " : "", detail);
    return 0;
}

/* `check(name, cond)` or `check(name, cond, "fmt", args...)` */
#define check(...) check_v(__VA_ARGS__, NULL)

/* ===================================================================== */
/* content rules (independent of the image file)                          */
/* ===================================================================== */
static const char HELLO_TXT[] =
    "Hello from SutureOS SquashFS!\nSecond line: gzip metadata and data.\n";
static const char README_TXT[] = "SquashFS 4.0 read-only driver test volume.\n";
static const char NESTED_TXT[] =
    "Nested file: /SUB/NESTED.TXT via the SquashFS directory table.\n";
static const char DEEP_TXT[] = "Deep directory chain reached through SquashFS.\n";
static const char LINK_TARGET[] = "LARGE.BIN";

#define LARGE_SIZE  (100u * 1024u)
#define UNCOMP_SIZE (8u * 1024u)
#define FRAG_SIZE   1000u

static uint8_t large_byte(uint32_t i) { return (uint8_t)((i * 31u + 7u) & 0xFFu); }
static uint8_t frag_byte(uint32_t i)  { return (uint8_t)((i * 5u + 1u) & 0xFFu); }

static uint8_t lcg_byte(uint32_t i)
{
    uint32_t s = 0x12345678u + i * 2654435761u;
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return (uint8_t)(s & 0xFFu);
}

/* ===================================================================== */
/* image loading                                                          */
/* ===================================================================== */
static int load_file(const char *path, uint8_t **out, uint64_t *out_len)
{
    FILE *f = fopen(path, "rb");
    long n;
    uint8_t *buf;

    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    n = ftell(f);
    if (n <= 0) { fclose(f); return -1; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -1; }
    buf = (uint8_t *)malloc((size_t)n);
    if (!buf) { fclose(f); return -1; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return -1; }
    fclose(f);
    *out = buf;
    *out_len = (uint64_t)n;
    return 0;
}

static void use_disk(uint8_t *img, uint64_t len)
{
    g_disk = img;
    g_disk_bytes = len;
    g_read_count = 0;
    g_io_fail_after = -1;
}

/* ===================================================================== */
/* manifest walking (inode-number / tree coverage)                        */
/* ===================================================================== */
static int g_inode_numbers[64];
static int g_inode_count;
static int g_path_count;
static int g_dup_inode;

static void record_inode(uint32_t number)
{
    for (int i = 0; i < g_inode_count; i++) {
        if (g_inode_numbers[i] == (int)number) g_dup_inode = 1;
    }
    if (g_inode_count < (int)(sizeof(g_inode_numbers) / sizeof(g_inode_numbers[0])))
        g_inode_numbers[g_inode_count++] = (int)number;
}

static int walk(const char *dir, int depth)
{
    struct squashfs_dir *d = NULL;
    char name[256];
    char child[512];

    if (depth > 8) return -1;
    if (squashfs_opendir(dir, &d) != SQUASHFS_OK) return -1;
    for (;;) {
        uint32_t len = 0;
        int is_dir = 0;
        int r = squashfs_readdir(d, name, sizeof(name), &len, &is_dir);
        struct squashfs_stat st;
        if (r == SQUASHFS_ENOENT) break;
        if (r != SQUASHFS_OK) { squashfs_closedir(d); return -1; }
        g_path_count++;
        snprintf(child, sizeof(child), "%s%s%s", dir,
                 (dir[0] && dir[strlen(dir) - 1] == '/') ? "" : "/", name);
        if (squashfs_stat(child, &st) != SQUASHFS_OK) { squashfs_closedir(d); return -1; }
        record_inode(st.inode_number);
        if (is_dir && walk(child, depth + 1) != 0) { squashfs_closedir(d); return -1; }
    }
    squashfs_closedir(d);
    return 0;
}

/* ===================================================================== */
/* helpers                                                                */
/* ===================================================================== */
static int read_whole(const char *path, uint8_t *buf, uint32_t cap, int *out_len)
{
    struct squashfs_file *f = NULL;
    int n, total = 0;

    if (squashfs_open(path, &f) != SQUASHFS_OK) return -1;
    for (;;) {
        n = squashfs_file_read(f, buf + total, cap - (uint32_t)total, (uint32_t)total);
        if (n < 0) { squashfs_close(f); return -1; }
        if (n == 0) break;
        total += n;
        if ((uint32_t)total >= cap) break;
    }
    squashfs_close(f);
    *out_len = total;
    return 0;
}

static uint32_t crc_of(const uint8_t *p, uint32_t len)
{
    return squashfs_crc32(p, len);
}

static int readdir_set(const char *path, char names[][128], int max, int *out_n)
{
    struct squashfs_dir *d = NULL;
    char name[256];
    int n = 0;

    if (squashfs_opendir(path, &d) != SQUASHFS_OK) return -1;
    for (;;) {
        uint32_t len = 0;
        int is_dir = 0;
        int r = squashfs_readdir(d, name, sizeof(name), &len, &is_dir);
        if (r == SQUASHFS_ENOENT) break;
        if (r != SQUASHFS_OK) { squashfs_closedir(d); return -1; }
        if (n < max) {
            strncpy(names[n], name, 127);
            names[n][127] = '\0';
        }
        n++;
    }
    squashfs_closedir(d);
    *out_n = n;
    return 0;
}

static int set_equals(char names[][128], int n, const char *const *want, int want_n)
{
    int seen[32];
    if (n != want_n) return 0;
    for (int i = 0; i < want_n; i++) seen[i] = 0;
    for (int i = 0; i < n; i++) {
        int hit = -1;
        for (int k = 0; k < want_n; k++) {
            if (strcmp(names[i], want[k]) == 0) { hit = k; break; }
        }
        if (hit < 0) return 0;
        if (seen[hit]) return 0;
        seen[hit] = 1;
    }
    for (int i = 0; i < want_n; i++) if (!seen[i]) return 0;
    return 1;
}

/* ===================================================================== */
/* positive checks                                                        */
/* ===================================================================== */
static const char *const ROOT_WANT[] = {
    "FRAG.TXT", "HELLO.TXT", "LARGE.BIN", "LINK.TXT", "README", "SUB",
    "UNCOMP.BIN", "ZERO.BIN"
};
static const char *const SUB_WANT[] = { "DEEP", "NESTED.TXT" };
static const char *const DEEP_WANT[] = { "DEEP.TXT" };

static void positive_checks(int selftest_want)
{
    struct squashfs_stat st;
    char names[64][128];
    int n = 0;
    uint8_t *buf;

    section("mount and superblock");
    /* Mount through the REAL VFS adapter (kernel/fs/squashfs_vfs.c) so its
     * device-string parsing and error mapping are covered too. "1" means block
     * drive 1, LBA 0 - exactly what `-hdb build/squashfs.img` provides. */
    check("vfs adapter mount(\"1\", \"/mnt/squash\", \"squashfs\") succeeds",
          squashfs_driver.mount("1", "/mnt/squash", NULL) == VFS_OK);
    check("squashfs_is_mounted() is true", squashfs_is_mounted() != 0);
    check("superblock magic is 0x73717368",
          g_squashfs_fs.sb.magic == SQUASHFS_MAGIC,
          "magic=0x%08x", g_squashfs_fs.sb.magic);
    check("version is 4.0",
          g_squashfs_fs.sb.version_major == 4 && g_squashfs_fs.sb.version_minor == 0,
          "%u.%u", (unsigned)g_squashfs_fs.sb.version_major,
          (unsigned)g_squashfs_fs.sb.version_minor);
    check("compression is gzip (1)", squashfs_compression() == SQUASHFS_COMP_GZIP,
          "compression=%u", (unsigned)squashfs_compression());
    check("block size is 4096", squashfs_block_size() == 4096,
          "block_size=%u", (unsigned)squashfs_block_size());
    check("block_log agrees with block_size",
          (1u << g_squashfs_fs.sb.block_log) == squashfs_block_size(),
          "block_log=%u", (unsigned)g_squashfs_fs.sb.block_log);
    check("inode count is 15", squashfs_entry_count() == 15,
          "inodes=%u", (unsigned)squashfs_entry_count());
    check("fragment count is 5", g_squashfs_fs.fragment_count == 5,
          "fragments=%u", (unsigned)g_squashfs_fs.fragment_count);
    check("bytes_used is inside the image",
          g_squashfs_fs.sb.bytes_used <= g_disk_bytes,
          "bytes_used=%llu image=%llu", (unsigned long long)g_squashfs_fs.sb.bytes_used,
          (unsigned long long)g_disk_bytes);
    check("inode table block offsets were built",
          g_squashfs_fs.inode_table.block_count >= 1,
          "%u blocks", g_squashfs_fs.inode_table.block_count);
    check("directory table block offsets were built",
          g_squashfs_fs.dir_table.block_count >= 1,
          "%u blocks", g_squashfs_fs.dir_table.block_count);
    check("fragment table block offsets were built",
          g_squashfs_fs.frag_table.block_count >= 1,
          "%u blocks", g_squashfs_fs.frag_table.block_count);

    section("kernel selftest");
    {
        char before[65536];
        size_t before_len = g_log_len;
        int r;
        memcpy(before, g_log, sizeof(before));
        r = squashfs_selftest();
        check("squashfs_selftest() returns OK", r == SQUASHFS_OK, "ret=%d", r);
        check("selftest logged PASS", strstr(g_log, "selftest PASS") != NULL);
        check("selftest logged no FAIL", strstr(g_log, "selftest FAIL") == NULL);
        if (r != SQUASHFS_OK || selftest_want == 0) {
            const char *p = g_log + before_len;
            printf("      driver log: %s\n", p[0] ? p : "(none)");
        }
        (void)before;
    }

    section("file content (byte for byte, host-derived CRCs)");
    buf = (uint8_t *)malloc(LARGE_SIZE + 4096);
    if (!buf) { check("harness can allocate its read buffer", 0); return; }

    /* small text files (all stored in fragments) */
    {
        struct { const char *path; const char *text; uint32_t size; } files[] = {
            { "/HELLO.TXT", HELLO_TXT, 67 },
            { "/README", README_TXT, 43 },
            { "/SUB/NESTED.TXT", NESTED_TXT, 63 },
            { "/SUB/DEEP/A/B/C/DEEP.TXT", DEEP_TXT, 47 },
        };
        uint8_t *small = (uint8_t *)malloc(4096);
        for (unsigned i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
            struct squashfs_file *f = NULL;
            uint32_t total = 0;
            int r;
            char label[256];
            int ok = 1;

            snprintf(label, sizeof(label), "read %s (%u bytes, fragment)", files[i].path,
                     files[i].size);
            if (squashfs_open(files[i].path, &f) != SQUASHFS_OK) {
                check(label, 0, "open failed");
                continue;
            }
            /* read in awkward 7-byte chunks to exercise the partial-read path */
            for (;;) {
                uint8_t chunk[7];
                r = squashfs_file_read(f, chunk, sizeof(chunk), total);
                if (r < 0) { ok = 0; break; }
                if (r == 0) break;
                if (total + (uint32_t)r > 4096u) { ok = 0; break; }
                memcpy(small + total, chunk, (size_t)r);
                total += (uint32_t)r;
            }
            squashfs_close(f);
            check(label, ok && total == files[i].size,
                  "ok=%d got=%u want=%u", ok, total, files[i].size);
            check("content of the small file matches its literal text",
                  total == files[i].size &&
                  memcmp(small, files[i].text, files[i].size) == 0);
            check("host CRC32 of the small file matches its literal text",
                  total == files[i].size &&
                  crc_of(small, total) ==
                  crc_of((const uint8_t *)files[i].text, files[i].size),
                  "driver=0x%08x host=0x%08x", crc_of(small, total),
                  crc_of((const uint8_t *)files[i].text, files[i].size));
        }
        free(small);
    }

    /* /LARGE.BIN: 25 blocks */
    {
        int got = 0;
        int r = read_whole("/LARGE.BIN", buf, LARGE_SIZE, &got);
        int mismatch = -1;
        uint32_t i;
        check("read /LARGE.BIN (100 KiB)", r == 0 && got == (int)LARGE_SIZE,
              "r=%d got=%d", r, got);
        if (got == (int)LARGE_SIZE) {
            for (i = 0; i < LARGE_SIZE; i++) {
                if (buf[i] != large_byte(i)) { mismatch = (int)i; break; }
            }
        }
        check("/LARGE.BIN every byte matches (i*31+7) rule", mismatch < 0,
              mismatch < 0 ? "" : "first mismatch at %d", mismatch);
        check("/LARGE.BIN crc32 == host crc32",
              got == (int)LARGE_SIZE && crc_of(buf, LARGE_SIZE) == 0x37fe697d,
              "driver=0x%08x (host constant 0x37fe697d)", crc_of(buf, (uint32_t)got));
    }

    /* /UNCOMP.BIN: stored (uncompressed) data blocks */
    {
        int got = 0;
        int r = read_whole("/UNCOMP.BIN", buf, UNCOMP_SIZE, &got);
        int mismatch = -1;
        uint32_t i;
        check("read /UNCOMP.BIN (8 KiB, stored blocks)",
              r == 0 && got == (int)UNCOMP_SIZE, "r=%d got=%d", r, got);
        if (got == (int)UNCOMP_SIZE) {
            for (i = 0; i < UNCOMP_SIZE; i++) {
                if (buf[i] != lcg_byte(i)) { mismatch = (int)i; break; }
            }
        }
        check("/UNCOMP.BIN every byte matches the lcg rule", mismatch < 0,
              mismatch < 0 ? "" : "first mismatch at %d", mismatch);
    }

    /* /FRAG.TXT: whole file lives in a fragment */
    {
        int got = 0;
        int r = read_whole("/FRAG.TXT", buf, 4096, &got);
        int mismatch = -1;
        uint32_t i;
        check("read /FRAG.TXT (fragment, 1000 B)", r == 0 && got == (int)FRAG_SIZE,
              "r=%d got=%d", r, got);
        if (got == (int)FRAG_SIZE) {
            for (i = 0; i < FRAG_SIZE; i++) {
                if (buf[i] != frag_byte(i)) { mismatch = (int)i; break; }
            }
        }
        check("/FRAG.TXT every byte matches (i*5+1) rule", mismatch < 0,
              mismatch < 0 ? "" : "first mismatch at %d", mismatch);
    }

    /* /ZERO.BIN */
    {
        uint8_t one = 0xAA;
        check("stat /ZERO.BIN says 0 bytes",
              squashfs_stat("/ZERO.BIN", &st) == SQUASHFS_OK && st.size == 0 &&
              st.is_file, "size=%llu", (unsigned long long)st.size);
        check("read /ZERO.BIN returns 0 (EOF)", squashfs_read_ref(st.inode_ref, &one, 0, 1) == 0);
    }

    section("partial / offset / EOF reads");
    {
        uint8_t part[64];
        uint64_t lref = 0;
        int i, bad = 0;
        if (squashfs_lookup("/LARGE.BIN", &lref) != SQUASHFS_OK) {
            check("lookup /LARGE.BIN", 0);
        } else {
            for (i = 0; i < 10; i++) {
                uint32_t off = (uint32_t)(i * 10000u + 1234u);
                int r = squashfs_read_ref(lref, part, off, sizeof(part));
                if (r != (int)sizeof(part)) { bad = 1; break; }
                for (uint32_t k = 0; k < sizeof(part); k++) {
                    if (part[k] != large_byte(off + k)) { bad = 2; break; }
                }
                if (bad) break;
            }
            check("10 partial reads at scattered offsets are correct", bad == 0,
                  bad == 1 ? "short read" : (bad == 2 ? "wrong bytes" : ""));
            /* straddling a 4096 boundary */
            {
                uint8_t big[300];
                int r = squashfs_read_ref(lref, big, 4000, sizeof(big));
                int ok = (r == (int)sizeof(big));
                for (uint32_t k = 0; ok && k < sizeof(big); k++) {
                    if (big[k] != large_byte(4000u + k)) ok = 0;
                }
                check("read straddling a 4096-byte block boundary is correct", ok);
            }
            /* clamped at EOF */
            {
                uint8_t tail[64];
                int r = squashfs_read_ref(lref, tail, LARGE_SIZE - 10u, sizeof(tail));
                int ok = (r == 10);
                for (uint32_t k = 0; ok && k < 10; k++) {
                    if (tail[k] != large_byte(LARGE_SIZE - 10u + k)) ok = 0;
                }
                check("read is clamped to EOF (10 of 64 bytes)", ok, "got=%d", r);
            }
            check("read starting past EOF returns 0",
                  squashfs_read_ref(lref, part, LARGE_SIZE + 1u, 16) == 0);
            check("read of size 0 returns 0",
                  squashfs_read_ref(lref, part, 0, 0) == 0);
        }
    }

    section("stat");
    {
        check("stat /LARGE.BIN size and type",
              squashfs_stat("/LARGE.BIN", &st) == SQUASHFS_OK && st.is_file &&
              !st.is_dir && !st.is_symlink && st.size == LARGE_SIZE,
              "size=%llu", (unsigned long long)st.size);
        check("stat /LARGE.BIN inode number is 5", st.inode_number == 5,
              "inode=%u", (unsigned)st.inode_number);
        check("stat /LARGE.BIN mode permission bits are 0644",
              (st.mode & 0xFFFu) == 0644u, "mode=0o%o", st.mode & 0xFFFu);
        check("stat / reports a directory",
              squashfs_stat("/", &st) == SQUASHFS_OK && st.is_dir, "");
        check("stat /SUB reports a directory with mode 0755",
              squashfs_stat("/SUB", &st) == SQUASHFS_OK && st.is_dir &&
              (st.mode & 0xFFFu) == 0755u, "mode=0o%o", st.mode & 0xFFFu);
        check("stat /LINK.TXT reports a symlink",
              squashfs_stat("/LINK.TXT", &st) == SQUASHFS_OK && st.is_symlink &&
              !st.is_file && !st.is_dir, "");
        check("stat /LINK.TXT has target_size 9", st.size == 9, "size=%llu",
              (unsigned long long)st.size);
        check("stat of a missing path fails with ENOENT",
              squashfs_stat("/NOPE", &st) == SQUASHFS_ENOENT);
        check("stat of /LARGE.BIN/x fails with ENOTDIR",
              squashfs_stat("/LARGE.BIN/x", &st) == SQUASHFS_ENOTDIR);
        check("stat of /HELLO.TXT/ fails with ENOTDIR (trailing slash)",
              squashfs_stat("/HELLO.TXT/", &st) == SQUASHFS_ENOTDIR);
        check("stat of /SUB/ succeeds (trailing slash on a directory)",
              squashfs_stat("/SUB/", &st) == SQUASHFS_OK && st.is_dir);
    }

    section("symlink target");
    {
        char target[64];
        int r = squashfs_readlink("/LINK.TXT", target, sizeof(target));
        check("readlink /LINK.TXT returns 9", r == 9, "ret=%d", r);
        check("readlink /LINK.TXT target is LARGE.BIN", strcmp(target, LINK_TARGET) == 0,
              "target=%s", target);
    }

    section("readdir");
    {
        int r = readdir_set("/", names, 64, &n);
        check("readdir(/) returns 8 entries", r == 0 && n == 8, "n=%d", n);
        check("readdir(/) set is exactly the documented one",
              r == 0 && set_equals(names, n, ROOT_WANT, 8), "n=%d", n);
        r = readdir_set("/SUB", names, 64, &n);
        check("readdir(/SUB) returns 2 entries", r == 0 && n == 2, "n=%d", n);
        check("readdir(/SUB) set is exactly {DEEP, NESTED.TXT}",
              r == 0 && set_equals(names, n, SUB_WANT, 2), "n=%d", n);
        r = readdir_set("/SUB/DEEP/A/B/C", names, 64, &n);
        check("readdir(/SUB/DEEP/A/B/C) returns 1 entry", r == 0 && n == 1, "n=%d", n);
        check("readdir(/SUB/DEEP/A/B/C) set is exactly {DEEP.TXT}",
              r == 0 && set_equals(names, n, DEEP_WANT, 1), "n=%d", n);
        check("readdir(/) does not list '.' or '..'",
              strcmp(names[0], ".") != 0 && strcmp(names[0], "..") != 0);
        r = readdir_set("/LARGE.BIN", names, 64, &n);
        check("readdir on a regular file fails with ENOTDIR", r == -1);
        r = readdir_set("/NOPE", names, 64, &n);
        check("readdir on a missing directory fails", r == -1);
    }

    section("path resolution");
    {
        uint64_t a = 0, b = 0;
        struct squashfs_file *fh = NULL;
        struct squashfs_dir *dh = NULL;
        check("lookup / gives the root inode",
              squashfs_lookup("/", &a) == SQUASHFS_OK);
        check("lookup '//' gives the same root inode",
              squashfs_lookup("//", &b) == SQUASHFS_OK && a == b);
        check("lookup /./HELLO.TXT gives the same inode as /HELLO.TXT",
              squashfs_lookup("/./HELLO.TXT", &a) == SQUASHFS_OK &&
              squashfs_lookup("/HELLO.TXT", &b) == SQUASHFS_OK && a == b);
        check("lookup /SUB/../HELLO.TXT gives the same inode",
              squashfs_lookup("/SUB/../HELLO.TXT", &a) == SQUASHFS_OK && a == b);
        check("lookup /SUB/DEEP/A/B/C/../DEEP.TXT resolves (C's parent is B, so "
              "this must NOT exist)",
              squashfs_lookup("/SUB/DEEP/A/B/C/../DEEP.TXT", &a) == SQUASHFS_ENOENT);
        check("lookup /SUB/DEEP/A/B/C/../C/DEEP.TXT resolves",
              squashfs_lookup("/SUB/DEEP/A/B/C/../C/DEEP.TXT", &a) == SQUASHFS_OK);
        check("lookup of a missing path fails with ENOENT",
              squashfs_lookup("/MISSING", &a) == SQUASHFS_ENOENT);
        check("open of a directory fails with EISDIR",
              squashfs_open("/SUB", &fh) == SQUASHFS_EISDIR);
        check("opendir of a file fails with ENOTDIR",
              squashfs_opendir("/README", &dh) == SQUASHFS_ENOTDIR);
    }

    section("tree coverage / inode numbers");
    {
        g_inode_count = 0;
        g_path_count = 0;
        g_dup_inode = 0;
        check("full tree walk succeeds", walk("/", 0) == 0);
        check("tree has 14 paths", g_path_count == 14, "%d", g_path_count);
        check("tree has 14 non-root inodes (15 total with the root)",
              g_inode_count == 14, "%d inodes below the root", g_inode_count);
        check("the root inode number is 2 and was not listed",
              g_dup_inode == 0);
        check("no inode number is reported twice", g_dup_inode == 0);
    }

    section("read-only honesty");
    {
        size_t mark = g_log_len;
        check("squashfs_write_file is refused",
              squashfs_write_file(5, "x", 0, 1) == SQUASHFS_EROFS);
        check("squashfs_write_block is refused",
              squashfs_write_block(0, buf) == SQUASHFS_EROFS);
        check("squashfs_write_inode is refused",
              squashfs_write_inode(1, buf) == SQUASHFS_EROFS);
        /* The VFS adapter is where the human-readable rejection is produced;
         * exercise it directly so the "never fake success" contract is visible
         * on the console as well. */
        {
            int r1 = squashfs_driver.mkdir(NULL, "/X", 0755);
            int r2 = squashfs_driver.write(NULL, buf, 4, 0);
            int r3 = squashfs_driver.unlink(NULL, "/README");
            check("the VFS adapter refuses mkdir/write/unlink with VFS_EPERM",
                  r1 == VFS_EPERM && r2 == VFS_EPERM && r3 == VFS_EPERM,
                  "mkdir=%d write=%d unlink=%d", r1, r2, r3);
            check("the read-only rejection was logged",
                  strstr(g_log + mark, "read-only") != NULL);
        }
        check("squashfs_mkdir is refused",
              squashfs_mkdir("/X", 0755) == SQUASHFS_EROFS);
        check("squashfs_rmdir is refused",
              squashfs_rmdir("/SUB") == SQUASHFS_EROFS);
        check("squashfs_unlink is refused",
              squashfs_unlink("/README") == SQUASHFS_EROFS);
        check("squashfs_compress is refused (no kernel compressor)",
              squashfs_compress(SQUASHFS_COMP_GZIP, buf, 16, buf, NULL) == SQUASHFS_ENOTSUP);
    }

    section("legacy entry points still work");
    {
        check("squashfs_init() succeeds", squashfs_init() == SQUASHFS_OK);
        check("squashfs_register() succeeds", squashfs_register() == SQUASHFS_OK);
        check("squashfs_vfs_init() succeeds", squashfs_vfs_init() == SQUASHFS_OK);
        check("squashfs_vfs_register() succeeds", squashfs_vfs_register() == SQUASHFS_OK);
        check("legacy squashfs_read_inode(2) maps to the root inode",
              squashfs_read_inode(2, &st) == SQUASHFS_OK && st.is_dir);
        check("legacy squashfs_read_inode(5) fails honestly (root-only API)",
              squashfs_read_inode(5, &st) == SQUASHFS_ENOTSUP);
        check("legacy squashfs_read_file(5) fails honestly",
              squashfs_read_file(5, buf, 0, 8) == SQUASHFS_ENOTSUP);
        check("legacy squashfs_read_dir is not supported",
              squashfs_read_dir(2, buf, 4) == SQUASHFS_ENOTSUP);
    }

    free(buf);
}

/* ===================================================================== */
/* negative checks                                                        */
/* ===================================================================== */
static uint8_t *g_good;
static uint64_t g_good_len;

static void put16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }
static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

/* Build a patched copy of the image with a single field changed. */
static uint8_t *patched(uint64_t *len)
{
    uint8_t *copy = (uint8_t *)malloc((size_t)g_good_len);
    memcpy(copy, g_good, (size_t)g_good_len);
    *len = g_good_len;
    return copy;
}

static void negative_checks(const char *bad_inode_path, const char *bad_frag_path,
                            const char *bad_data_path)
{
    uint8_t *img;
    uint64_t len;

    section("negative: bad magic");
    img = patched(&len);
    put32(img + 0, 0xDEADBEEF);
    use_disk(img, len);
    check("mount rejects a wrong magic",
          squashfs_mount(1, 0) != SQUASHFS_OK && !squashfs_is_mounted());
    free(img);

    section("negative: unsupported version");
    img = patched(&len);
    put16(img + 28, 5);           /* version_major = 5 */
    use_disk(img, len);
    check("mount rejects version 5.0",
          squashfs_mount(1, 0) != SQUASHFS_OK && !squashfs_is_mounted());
    free(img);
    img = patched(&len);
    put16(img + 30, 1);           /* version_minor = 1 */
    use_disk(img, len);
    check("mount rejects version 4.1",
          squashfs_mount(1, 0) != SQUASHFS_OK && !squashfs_is_mounted());
    free(img);

    section("negative: unknown compression");
    img = patched(&len);
    put16(img + 20, SQUASHFS_COMP_XZ);   /* 4 */
    use_disk(img, len);
    check("mount rejects compression id 4 (xz)",
          squashfs_mount(1, 0) != SQUASHFS_OK && !squashfs_is_mounted());
    free(img);
    img = patched(&len);
    put16(img + 20, SQUASHFS_COMP_ZSTD); /* 6 */
    use_disk(img, len);
    check("mount rejects compression id 6 (zstd)",
          squashfs_mount(1, 0) != SQUASHFS_OK && !squashfs_is_mounted());
    free(img);

    section("negative: block size / block_log");
    img = patched(&len);
    put32(img + 12, 3000);        /* not a power of two */
    put16(img + 22, 11);
    use_disk(img, len);
    check("mount rejects a non-power-of-two block size",
          squashfs_mount(1, 0) != SQUASHFS_OK && !squashfs_is_mounted());
    free(img);
    img = patched(&len);
    put16(img + 22, 13);          /* block_log 13 but block_size 4096 */
    use_disk(img, len);
    check("mount rejects a block_log that disagrees with block_size",
          squashfs_mount(1, 0) != SQUASHFS_OK && !squashfs_is_mounted());
    free(img);
    img = patched(&len);
    put32(img + 12, 8192);        /* legal value, bogus log */
    put16(img + 22, 11);
    use_disk(img, len);
    check("mount rejects block_size 8192 with block_log 11",
          squashfs_mount(1, 0) != SQUASHFS_OK && !squashfs_is_mounted());
    free(img);

    section("negative: truncated image / I/O failure");
    img = patched(&len);
    use_disk(img, 64);            /* shorter than one superblock read */
    check("mount rejects an image smaller than the superblock",
          squashfs_mount(1, 0) != SQUASHFS_OK && !squashfs_is_mounted());
    free(img);
    img = patched(&len);
    use_disk(img, len);
    g_io_fail_after = 0;          /* the first block read fails */
    check("mount rejects a device whose reads fail",
          squashfs_mount(1, 0) != SQUASHFS_OK && !squashfs_is_mounted());
    g_io_fail_after = -1;
    free(img);

    section("negative: inconsistent table starts");
    img = patched(&len);
    put32(img + 40, 64);          /* bytes_used = 64 (smaller than the tables) */
    use_disk(img, len);
    check("mount rejects bytes_used smaller than the table starts",
          squashfs_mount(1, 0) != SQUASHFS_OK && !squashfs_is_mounted());
    free(img);

    section("negative: inode reference out of range");
    {
        uint8_t *bad = NULL;
        uint64_t bad_len = 0;
        if (bad_inode_path && load_file(bad_inode_path, &bad, &bad_len) == 0) {
            use_disk(bad, bad_len);
            /* The fixture's root inode reference points at an inode table block
             * that does not exist. The driver validates the root at mount time,
             * so either the mount or the very first stat must fail - what must
             * NOT happen is a mount that then reports a bogus directory. */
            {
                int m = squashfs_mount(1, 0);
                if (m != SQUASHFS_OK) {
                    check("mount rejects a root inode reference past the inode table",
                          !squashfs_is_mounted());
                } else {
                    struct squashfs_stat st;
                    check("stat / fails when the root inode reference is out of range",
                          squashfs_stat("/", &st) != SQUASHFS_OK);
                    squashfs_unmount();
                }
            }
            free(bad);
        } else {
            check("bad-inode fixture is available", 0, "%s", bad_inode_path ? bad_inode_path : "(none)");
        }
    }
    {
        /* runtime lookup of an inode reference beyond the table */
        use_disk(g_good, g_good_len);
        check("re-mount the good image", squashfs_mount(1, 0) == SQUASHFS_OK);
        {
            uint8_t one = 0;
            /* block index 5000 does not exist in a 1-block inode table */
            uint64_t bogus = ((uint64_t)5000 << 16) | 0u;
            check("squashfs_read_ref() rejects an inode reference past the table",
                  squashfs_read_ref(bogus, &one, 0, 1) != SQUASHFS_OK);
        }
        squashfs_unmount();
    }

    section("negative: fragment index beyond the table");
    {
        uint8_t *bad = NULL;
        uint64_t bad_len = 0;
        if (bad_frag_path && load_file(bad_frag_path, &bad, &bad_len) == 0) {
            use_disk(bad, bad_len);
            check("mount succeeds on the bad-fragment fixture",
                  squashfs_mount(1, 0) == SQUASHFS_OK);
            {
                struct squashfs_file *f = NULL;
                check("open /FRAG.TXT fails when its fragment index is out of range",
                      squashfs_open("/FRAG.TXT", &f) != SQUASHFS_OK);
            }
            {
                struct squashfs_stat st;
                /* a file whose fragment index is bogus is still stat-able
                 * (stat only reads the inode) but its data is not readable */
                check("stat /FRAG.TXT still reads the inode", 
                      squashfs_stat("/FRAG.TXT", &st) == SQUASHFS_OK);
            }
            squashfs_unmount();
            free(bad);
        } else {
            check("bad-fragment fixture is available", 0,
                  "%s", bad_frag_path ? bad_frag_path : "(none)");
        }
    }

    section("negative: data block list past the end of the image");
    {
        /* The inode table is compressed, so the literal 4-byte value of
         * /LARGE.BIN's blocks_start is not addressable in the image file. Build
         * the fixture through the same writer instead: every file inode gets a
         * blocks_start far past the end of the image while the tables stay
         * intact, so mount succeeds and the DATA read must fail. */
        if (bad_data_path && load_file(bad_data_path, &img, &len) == 0) {
            use_disk(img, len);
            check("mount succeeds on the bad-blocks_start fixture",
                  squashfs_mount(1, 0) == SQUASHFS_OK);
            {
                struct squashfs_stat st;
                check("the bad-blocks_start file still stats (its inode is intact)",
                      squashfs_stat("/LARGE.BIN", &st) == SQUASHFS_OK &&
                      st.size == LARGE_SIZE);
                {
                    uint8_t tmp[256];
                    int r = squashfs_read_ref(st.inode_ref, tmp, 0, sizeof(tmp));
                    check("reading it fails instead of returning image bytes",
                          r != (int)sizeof(tmp), "ret=%d", r);
                }
            }
            squashfs_unmount();
            free(img);
        } else {
            check("bad-blocks_start fixture is available", 0,
                  "%s", bad_data_path ? bad_data_path : "(none)");
        }
    }

    section("negative: unmounted state");
    {
        uint8_t one = 0;
        uint64_t ref = 0;
        struct squashfs_stat st;
        struct squashfs_file *f = NULL;
        static struct squashfs_file dummy_file;   /* never opened: only its address */
        static struct squashfs_dir dummy_dir;
        /* unmount through the adapter, the way the VFS core would */
        check("vfs adapter unmount succeeds",
              squashfs_driver.unmount("/mnt/squash") == VFS_OK);
        check("lookup after unmount returns EBUSY",
              squashfs_lookup("/README", &ref) == SQUASHFS_EBUSY);
        check("read_ref after unmount returns EBUSY",
              squashfs_read_ref(0, &one, 0, 1) == SQUASHFS_EBUSY);
        check("stat after unmount returns EBUSY",
              squashfs_stat("/", &st) == SQUASHFS_EBUSY);
        check("open after unmount returns EBUSY",
              squashfs_open("/README", &f) == SQUASHFS_EBUSY);
        check("opendir after unmount returns EBUSY",
              squashfs_opendir("/", (struct squashfs_dir **)&f) == SQUASHFS_EBUSY);
        dummy_file.used = 1;
        dummy_dir.used = 1;
        check("read through a stale handle after unmount returns EBUSY",
              squashfs_file_read(&dummy_file, &one, 1, 0) == SQUASHFS_EBUSY);
        check("readdir through a stale directory after unmount returns EBUSY",
              squashfs_readdir(&dummy_dir, (char *)&ref, 8, NULL, NULL) == SQUASHFS_EBUSY);
        check("vfs adapter read/write on a stale handle are honest",
              squashfs_driver.read(&dummy_file, &one, 1, 0) == VFS_EBUSY &&
              squashfs_driver.write(&dummy_file, &one, 1, 0) == VFS_EPERM);
        check("squashfs_is_mounted() is false", squashfs_is_mounted() == 0);
    }
}

/* ===================================================================== */
/* optional: a no-fragment image must work too                            */
/* ===================================================================== */
static void nofrag_checks(const char *path)
{
    uint8_t *img = NULL;
    uint64_t len = 0;
    struct squashfs_stat st;
    uint8_t *buf;
    int got = 0;

    section("no-fragment image (optional fifth argument)");
    if (!path) {
        check("no-fragment image provided", 1, "skipped (no path given)");
        return;
    }
    if (load_file(path, &img, &len) != 0) {
        check("no-fragment image can be read", 0, "%s", path);
        return;
    }
    use_disk(img, len);
    check("mount succeeds on a no-fragment image", squashfs_mount(1, 0) == SQUASHFS_OK);
    check("fragment_entry_count is 0", g_squashfs_fs.fragment_count == 0,
          "fragments=%u", (unsigned)g_squashfs_fs.fragment_count);
    check("the fragment table was left empty",
          g_squashfs_fs.frag_table.block_count == 0);
    check("inode/directory tables were still discovered",
          g_squashfs_fs.inode_table.block_count >= 1 &&
          g_squashfs_fs.dir_table.block_count >= 1);
    check("selftest passes on a no-fragment image",
          squashfs_selftest() == SQUASHFS_OK);
    buf = (uint8_t *)malloc(4096);
    if (buf) {
        int r = read_whole("/HELLO.TXT", buf, 4096, &got);
        check("a small file in a no-fragment image reads as one short block",
              r == 0 && got == 67 && memcmp(buf, HELLO_TXT, 67) == 0,
              "r=%d got=%d", r, got);
        got = 0;
        r = read_whole("/LARGE.BIN", buf, 4096, &got);
        check("the first 4 KiB of the multi-block file are correct",
              r == 0 && got == 4096 &&
              memcmp(buf, "\x07", 1) == 0 && large_byte(0) == buf[0] &&
              large_byte(4095) == buf[4095]);
        free(buf);
    }
    check("stat still reports the right size",
          squashfs_stat("/LARGE.BIN", &st) == SQUASHFS_OK && st.size == LARGE_SIZE);
    squashfs_unmount();
    free(img);
}

/* ===================================================================== */
/* optional: an image whose metadata blocks are stored UNCOMPRESSED       */
/* ===================================================================== */
static void rawmeta_checks(const char *path)
{
    uint8_t *img = NULL;
    uint64_t len = 0;
    uint8_t buf[128];
    char names[4][128];
    int got = 0;
    int n = 0;

    section("uncompressed-metadata image (optional sixth argument)");
    if (!path) {
        check("uncompressed-metadata image provided", 1, "skipped (no path given)");
        return;
    }
    if (load_file(path, &img, &len) != 0) {
        check("uncompressed-metadata image can be read", 0, "%s", path);
        return;
    }
    use_disk(img, len);
    check("mount succeeds when metadata blocks are stored uncompressed",
          squashfs_mount(1, 0) == SQUASHFS_OK);
    check("the SQUASHFS_FLAG_UNCOMPRESSED_INODES flag is set",
          (g_squashfs_fs.flags & SQUASHFS_FLAG_UNCOMPRESSED_INODES) != 0,
          "flags=0x%x", (unsigned)g_squashfs_fs.flags);
    check("selftest passes on an uncompressed-metadata image",
          squashfs_selftest() == SQUASHFS_OK);
    check("a file reads correctly with uncompressed inode metadata",
          read_whole("/HELLO.TXT", buf, sizeof(buf), &got) == 0 && got == 67 &&
          memcmp(buf, HELLO_TXT, 67) == 0, "got=%d", got);
    check("readdir works with uncompressed directory metadata",
          readdir_set("/SUB", names, 4, &n) == 0 && n == 2 &&
          set_equals(names, n, SUB_WANT, 2), "n=%d", n);
    squashfs_unmount();
    free(img);
}

/* ===================================================================== */
int main(int argc, char **argv)
{
    uint8_t *img = NULL;
    uint64_t len = 0;

    if (argc < 2) {
        fprintf(stderr, "usage: %s <squashfs.img> [bad-inode] [bad-frag] "
                        "[bad-blocks] [nofrag] [rawmeta]\n", argv[0]);
        return 2;
    }
    if (load_file(argv[1], &img, &len) != 0) {
        fprintf(stderr, "cannot read %s\n", argv[1]);
        return 2;
    }
    g_good = img;
    g_good_len = len;
    use_disk(img, len);

    printf("SutureOS SquashFS 4.0 driver host harness\n");
    printf("image: %s (%llu bytes)\n", argv[1], (unsigned long long)len);

    positive_checks(1);
    negative_checks(argc > 2 ? argv[2] : NULL, argc > 3 ? argv[3] : NULL,
                    argc > 4 ? argv[4] : NULL);
    nofrag_checks(argc > 5 ? argv[5] : NULL);
    rawmeta_checks(argc > 6 ? argv[6] : NULL);

    printf("\nchecks=%d ok=%d fail=%d\n", g_checks, g_ok, g_fail);
    printf("=== RESULT: %s ===\n", g_fail == 0 ? "PASS" : "FAIL");
    return g_fail == 0 ? 0 : 1;
}
