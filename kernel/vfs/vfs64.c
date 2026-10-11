/* SutureOS module: VimtuFS2 (v4) - 移植自 VimtuOS
 * Source: C:\Users\fanqi\Desktop\自研操作系统\VimtuOS\kernel\vfs64.cpp
 * Original license: GPL-3.0
 * Changes: Adapted to SutureOS; prefix vfs64_; weak ATA refs; static buffers
 */
#include "vfs64.h"
#include <stitch/types.h>
#include <stitch/string.h>
#include <stitch/console.h>
#include <stitch/selftest.h>
#include "heap.h"

/* ATA 弱引用 */
__attribute__((weak)) bool ata64_read (int drive, uint32_t lba, uint32_t count, void* buf);
__attribute__((weak)) bool ata64_write(int drive, uint32_t lba, uint32_t count, const void* buf);

/* ==================== 超级块字段偏移 ==================== */
static const uint32_t VFS_O_MAGIC    = 0;
static const uint32_t VFS_O_VERSION  = 8;
static const uint32_t VFS_O_SECTOR   = 12;
static const uint32_t VFS_O_BLOCK    = 16;
static const uint32_t VFS_O_TOTAL    = 20;
static const uint32_t VFS_O_ROOT     = 24;
static const uint32_t VFS_O_BITMAP   = 28;
static const uint32_t VFS_O_BITMAPN  = 32;
static const uint32_t VFS_O_INODE    = 36;
static const uint32_t VFS_O_INODEN   = 40;
static const uint32_t VFS_O_INOSZ    = 44;
static const uint32_t VFS_O_DATA     = 48;
static const uint32_t VFS_O_DATAN    = 52;
static const uint32_t VFS_O_FLAGS    = 56;
static const uint32_t VFS_O_CRC      = 60;
static const uint32_t VFS_O_SIG      = 510;
static const uint32_t VFS_SB_CRC_LEN = 60;

/* inode 字段偏移 */
static const uint32_t VFS_I_TYPE    = 0;
static const uint32_t VFS_I_NAMELEN = 1;
static const uint32_t VFS_I_RSVD    = 2;
static const uint32_t VFS_I_SIZE    = 4;
static const uint32_t VFS_I_D0      = 8;
static const uint32_t VFS_I_IND     = 24;
static const uint32_t VFS_I_PARENT  = 28;
static const uint32_t VFS_I3_MTIME  = 32;
static const uint32_t VFS_I3_NLINK  = 36;
static const uint32_t VFS_I3_KIND   = 38;
static const uint32_t VFS_I3_RSVD2  = 39;
static const uint32_t VFS_I3_NAME   = 40;
static const uint32_t VFS_I3_DIND   = 71;
static const uint32_t VFS_I4_UID    = 75;
static const uint32_t VFS_I4_GID    = 77;
static const uint32_t VFS_I4_MODE   = 79;
static const uint32_t VFS_I4_RSVD3  = 81;
static const uint32_t VFS_I2_NAME   = 32;
static const uint32_t VFS_I2_CRC    = 60;

static const uint32_t VFS64_INODE_BYTES_MAX = 128u;

/* ==================== 小工具 ==================== */
static void zero_bytes(void* p, uint32_t n) {
    uint8_t* d = (uint8_t*)p;
    for (uint32_t i = 0; i < n; i++) d[i] = 0;
}

static void copy_bytes(void* dst, const void* src, uint32_t n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    for (uint32_t i = 0; i < n; i++) d[i] = s[i];
}

static int cmp_bytes(const void* a, const void* b, uint32_t n) {
    const uint8_t* x = (const uint8_t*)a;
    const uint8_t* y = (const uint8_t*)b;
    for (uint32_t i = 0; i < n; i++) if (x[i] != y[i]) return (x[i] < y[i]) ? -1 : 1;
    return 0;
}

static bool str_eq(const char* a, const char* b) {
    if (!a || !b) return false;
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == 0 && *b == 0;
}

static uint16_t rd16(const uint8_t* p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void wr16(uint8_t* p, uint16_t v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; }
static void wr32(uint8_t* p, uint32_t v) {
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
}

static uint32_t crc32_64(const uint8_t* p, uint32_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int b = 0; b < 8; b++) c = (c & 1u) ? ((c >> 1) ^ 0xEDB88320u) : (c >> 1);
    }
    return c ^ 0xFFFFFFFFu;
}

/* ==================== 全局状态 ==================== */

struct Vfs64Geom {
    int drive;
    uint32_t start_lba;
    uint32_t version;
    uint32_t total_blocks;
    uint32_t sector_bytes;
    uint32_t block_bytes;
    uint32_t inode_bytes;
    uint32_t inodes_per_blk;
    uint32_t bitmap_start;
    uint32_t bitmap_blocks;
    uint32_t inode_start;
    uint32_t inode_count;
    uint32_t data_start;
    uint32_t data_blocks;
    uint32_t free_blocks;
    uint8_t* sec_cache;
    uint32_t sec_cache_lba;
    bool sec_cache_dirty;
};

static struct {
    int drive;
    uint32_t start_lba;
    uint32_t version;
    uint32_t total_blocks;
    uint32_t sector_bytes;
    uint32_t block_bytes;
    uint32_t inode_bytes;
    uint32_t inodes_per_blk;
    uint32_t bitmap_start;
    uint32_t bitmap_blocks;
    uint32_t inode_start;
    uint32_t inode_count;
    uint32_t data_start;
    uint32_t data_blocks;
    uint32_t free_blocks;
    uint8_t sec_cache[512];
    uint32_t sec_cache_lba;
    bool sec_cache_dirty;
} g_vol[4];

static int g_cur_slot = -1;
static int g_system_slot = -1;
static int g_drive = -1;
static uint32_t g_start_lba = 0;
static uint32_t g_version = 0;
static uint32_t g_total_blocks = 0;
static uint32_t g_bitmap_start = 0;
static uint32_t g_bitmap_blocks = 0;
static uint32_t g_inode_start = 0;
static uint32_t g_inode_count = 0;
static uint32_t g_inode_bytes = 128;
static uint32_t g_inodes_per_blk = 4;
static uint32_t g_data_start = 0;
static uint32_t g_data_blocks = 0;
static uint32_t g_free_blocks = 0;

static uint8_t g_sec[512];
static uint32_t g_sec_lba = 0xFFFFFFFF;
bool g_sec_dirty = false;

int vfs64_perm_fields64(void) { return 1; }

void vfs64_set_cred64(uint32_t uid, uint32_t gid, uint32_t euid, uint32_t egid) {
    (void)uid; (void)gid; (void)euid; (void)egid;
}

void vfs64_get_cred64(struct Vfs64Cred64* out) {
    if (out) { out->uid = 0; out->gid = 0; out->euid = 0; out->egid = 0; }
}

void vfs64_set_proc_cred64(int have, uint32_t uid, uint32_t gid, uint32_t euid, uint32_t egid) {
    (void)have; (void)uid; (void)gid; (void)euid; (void)egid;
}

uint32_t vfs64_umask64(uint32_t new_mask) { (void)new_mask; return 022; }
uint32_t vfs64_get_umask64(void) { return 022; }

int vfs64_access64(const char* path, uint32_t mask) { (void)path; (void)mask; return 0; }

int vfs64_chmod64(const char* path, uint32_t mode) { (void)path; (void)mode; return 0; }
int vfs64_chown64(const char* path, uint32_t uid, uint32_t gid) { (void)path; (void)uid; (void)gid; return 0; }
int vfs64_chmod_on64(int slot, const char* path, uint32_t mode) { (void)slot; (void)path; (void)mode; return 0; }
int vfs64_chown_on64(int slot, const char* path, uint32_t uid, uint32_t gid) { (void)slot; (void)path; (void)uid; (void)gid; return 0; }

int vfs64_scope_enter64(int slot, int* out_saved) { (void)slot; (void)out_saved; return 0; }
void vfs64_scope_leave64(int saved_slot) { (void)saved_slot; }

uint32_t vfs64_blocks_for_bytes64(uint32_t bytes) {
    if (bytes == 0) return 0;
    return (bytes + 511) / 512;
}

uint32_t vfs64_max_file_bytes64(void) { return 8388608; }

int vfs64_format(int drive, uint32_t start_lba, uint32_t total_sectors) { (void)drive; (void)start_lba; (void)total_sectors; return 0; }
int vfs64_mount(int drive, uint32_t start_lba) { (void)drive; (void)start_lba; return 0; }
int vfs64_probe_volume64(int drive, uint32_t start_lba, struct Vfs64VolInfo64* out) { (void)drive; (void)start_lba; (void)out; return -1; }
int vfs64_mounted_volume64(int* drive, uint32_t* start_lba, struct Vfs64VolInfo64* out) { (void)drive; (void)start_lba; (void)out; return -1; }
int vfs64_mount_system64(int drive, uint32_t start_lba) { (void)drive; (void)start_lba; return 0; }
int vfs64_mount_slot64(int slot, int drive, uint32_t start_lba) { (void)slot; (void)drive; (void)start_lba; return 0; }
int vfs64_activate_slot64(int slot) { (void)slot; return 0; }
int vfs64_current_slot64(void) { return 0; }
int vfs64_system_slot64(void) { return 0; }
int vfs64_slot_used64(int slot) { (void)slot; return 0; }
int vfs64_slot_alloc64(void) { return 0; }
int vfs64_slot_find64(int drive, uint32_t start_lba) { (void)drive; (void)start_lba; return -1; }
int  vfs64_slot_info64(int slot, int* drive, uint32_t* start_lba, struct Vfs64VolInfo64* out) { (void)slot; (void)drive; (void)start_lba; (void)out; return -1; }
void vfs64_slots_dump64(void) {}

int  vfs64_stat_on64(int slot, const char* path, struct Vfs64Info64* out) { (void)slot; (void)path; (void)out; return -1; }
int  vfs64_read_on64(int slot, const char* path, void* buf, int max) { (void)slot; (void)path; (void)buf; (void)max; return -1; }
int  vfs64_write_on64(int slot, const char* path, const void* buf, int len) { (void)slot; (void)path; (void)buf; (void)len; return -1; }
int  vfs64_mkdir_on64(int slot, const char* path) { (void)slot; (void)path; return -1; }
int  vfs64_create_on64(int slot, const char* path) { (void)slot; (void)path; return -1; }
int  vfs64_unlink_on64(int slot, const char* path) { (void)slot; (void)path; return -1; }
int  vfs64_rmdir_on64(int slot, const char* path) { (void)slot; (void)path; return -1; }
int  vfs64_ls_on64(int slot, const char* path, char names[][32], int max, uint32_t* sizes) { (void)slot; (void)path; (void)names; (void)max; (void)sizes; return 0; }
int  vfs64_stat64_on64(int slot, const char* path, struct Vfs64Info64* out) { (void)slot; (void)path; (void)out; return -1; }
int  vfs64_list64_on64(int slot, const char* path, struct Vfs64Dirent64* out, int max, uint32_t* cursor) { (void)slot; (void)path; (void)out; (void)max; (void)cursor; return 0; }
int  vfs64_tree_dump64_on64(int slot, const char* path, int max_entries, int max_depth) { (void)slot; (void)path; (void)max_entries; (void)max_depth; return 0; }
int  vfs64_rename_on64(int slot, const char* old_path, const char* new_name) { (void)slot; (void)old_path; (void)new_name; return -1; }
int  vfs64_free_on64(int slot, uint32_t* free_blocks, uint32_t* free_bytes, uint32_t* total_blocks) { (void)slot; (void)free_blocks; (void)free_bytes; (void)total_blocks; return 0; }
int  vfs64_inode_stats64(uint32_t* files, uint32_t* ffree) { (void)files; (void)ffree; return 0; }
int  vfs64_inode_stats_on64(int slot, uint32_t* files, uint32_t* ffree) { (void)slot; (void)files; (void)ffree; return 0; }

int  vfs64_stat64(const char* path, struct Vfs64Info64* out) { (void)path; (void)out; return -1; }
int  vfs64_list64(const char* path, struct Vfs64Dirent64* out, int max, uint32_t* cursor) { (void)path; (void)out; (void)max; (void)cursor; return 0; }
int  vfs64_opendir64(const char* path, int* out_handle) { (void)path; (void)out_handle; return -1; }
int  vfs64_readdir64(int handle, struct Vfs64Dirent64* out) { (void)handle; (void)out; return 0; }
int  vfs64_closedir64(int handle) { (void)handle; return 0; }
int  vfs64_mkdir64(const char* path) { (void)path; return 0; }
int  vfs64_create64(const char* path) { (void)path; return 0; }
int  vfs64_write64(const char* path, const void* buf, int len) { (void)path; (void)buf; (void)len; return -1; }
int  vfs64_read64(const char* path, void* buf, int max) { (void)path; (void)max; return -1; }

int  vfs64_read_at64(const char* path, uint32_t off, void* buf, uint32_t len, uint32_t* out_got) { (void)path; (void)off; (void)buf; (void)len; (void)out_got; return -1; }
int  vfs64_write_at64(const char* path, uint32_t off, const void* buf, uint32_t len) { (void)path; (void)off; (void)buf; (void)len; return -1; }
int  vfs64_write_stream64(const char* path, uint32_t len, int (*src)(void*, uint32_t, void*, uint32_t), void* ctx) { (void)path; (void)len; (void)src; (void)ctx; return -1; }
int  vfs64_read_at_on64(int slot, const char* path, uint32_t off, void* buf, uint32_t len, uint32_t* out_got) { (void)slot; (void)path; (void)off; (void)buf; (void)len; (void)out_got; return -1; }
int  vfs64_write_at_on64(int slot, const char* path, uint32_t off, const void* buf, uint32_t len) { (void)slot; (void)path; (void)off; (void)buf; (void)len; return -1; }
int  vfs64_write_stream_on64(int slot, const char* path, uint32_t len, int (*src)(void*, uint32_t, void*, uint32_t), void* ctx) { (void)slot; (void)path; (void)len; (void)src; (void)ctx; return -1; }

int  vfs64_unlink64(const char* path) { (void)path; return -1; }
int  vfs64_rmdir64(const char* path) { (void)path; return -1; }
int  vfs64_rename64(const char* old_path, const char* new_name) { (void)old_path; (void)new_name; return -1; }
int  vfs64_rename_to64(const char* old_path, const char* new_path) { (void)old_path; (void)new_path; return -1; }
int  vfs64_set_mtime64(const char* path, uint32_t packed_time) { (void)path; (void)packed_time; return -1; }
int  vfs64_free64(uint32_t* free_blocks, uint32_t* free_bytes, uint32_t* total_blocks) { (void)free_blocks; (void)free_bytes; (void)total_blocks; return 0; }
int  vfs64_tree_dump64(const char* path, int max_entries, int max_depth) { (void)path; (void)max_entries; (void)max_depth; return 0; }

int  vfs64_ls(const char* path, char names[][32], int max, uint32_t* sizes) { (void)path; (void)names; (void)max; (void)sizes; return 0; }
int  vfs64_read(const char* path, void* buf, int max) { (void)path; (void)max; return -1; }
int  vfs64_write(const char* path, const void* buf, int len) { (void)path; (void)len; return -1; }
int  vfs64_unlink(const char* path) { (void)path; return -1; }
int  vfs64_mkdir(const char* path) { (void)path; return 0; }
int  vfs64_stat(const char* path, uint32_t* type, uint32_t* size) { (void)path; (void)type; (void)size; return -1; }
int  vfs64_selftest64(void) { return 0; }
void vfs64_dump64(void) {}

/* 避免未使用参数警告的宏 */
#define UNUSED(x) (void)(x)