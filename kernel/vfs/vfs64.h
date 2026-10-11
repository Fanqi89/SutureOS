/* SutureOS module: VimtuFS2 (VimtuFS v4) - 移植自 VimtuOS
 * Source: C:\Users\fanqi\Desktop\自研操作系统\VimtuOS\kernel\vfs64.h
 * Original license: GPL-3.0
 * Changes: Adapted to SutureOS types and conventions; prefix vfs64_
 */
#ifndef STITCH_VFS64_H
#define STITCH_VFS64_H

#include <stitch/types.h>

/* ==================== 魔数/版本/几何常量 ==================== */
#define VFS64_MAGIC            "VIMTUFS2"
#define VFS64_VERSION          4u
#define VFS64_VERSION_V3       3u
#define VFS64_VERSION_V2       2u
#define VFS64_SECTOR_BYTES     512u
#define VFS64_BLOCK_BYTES      512u
#define VFS64_INODE_BYTES      128u
#define VFS64_INODES_PER_BLK   4u
#define VFS64_NAME_MAX         31u
#define VFS64_NAME_MAX_V2      27u
#define VFS64_DIRECT_BLOCKS    4u
#define VFS64_INDIRECT_PTRS    128u
#define VFS64_DIND_CHILDREN    128u
#define VFS64_L2_FIRST_BLOCK   (VFS64_DIRECT_BLOCKS + VFS64_INDIRECT_PTRS)   // 132
#define VFS64_MAX_MAP_BLOCKS   (VFS64_DIRECT_BLOCKS + VFS64_INDIRECT_PTRS + \
                                 VFS64_DIND_CHILDREN * VFS64_INDIRECT_PTRS)   // 16516
#define VFS64_MAX_FILE_BLOCKS  16384u
#define VFS64_MAX_FILE_BYTES   (VFS64_MAX_FILE_BLOCKS * VFS64_BLOCK_BYTES)   // 8388608
#define VFS64_MAX_FILE_BYTES_V2 67584u
#define VFS64_READ_CHUNK_BYTES 4096u
#define VFS64_MAX_INODES       512u
#define VFS64_BITMAP_BLK_BITS  4096u
#define VFS64_MIN_BLOCKS       32u
#define VFS64_PATH_MAX         128u
#define VFS64_PATH_DEPTH_MAX   16u
#define VFS64_LS_NAME_BUF      32u
#define VFS64_DIRSTREAM_MAX    8u
#define VFS64_SLOT_MAX         4u

/* 当前卷是否有权限字段（v4 = 1） */
int vfs64_perm_fields64(void);

/* inode 权限字段偏移（v4 有效） */
#define VFS64_INO_UID_OFF      75u
#define VFS64_INO_GID_OFF      77u
#define VFS64_INO_MODE_OFF     79u
#define VFS64_INO_RSVD4_OFF    81u

/* mode 的位 */
#define VFS64_S_IFMT           0170000u
#define VFS64_S_IFREG          0100000u
#define VFS64_S_IFDIR          0040000u
#define VFS64_S_IRWXU          0000700u
#define VFS64_S_IRWXG          0000070u
#define VFS64_S_IRWXO          0000007u
#define VFS64_S_IRWX           0000777u
#define VFS64_S_IRUSR          0000400u
#define VFS64_S_IWUSR          0000200u
#define VFS64_S_IXUSR          0000100u
#define VFS64_S_IRWX           0000777u
#define VFS64_DEF_DIR_MODE     (0x4000 | 0755u)   /* S_IFDIR | 0755 */
#define VFS64_DEF_FILE_MODE    (0x8000 | 0644u)   /* S_IFREG | 0644 */
#define VFS64_LEGACY_DIR_MODE  (0x4000 | 0755u)
#define VFS64_LEGACY_FILE_MODE (0x8000 | 0644u)
#define VFS64_UMASK_DEFAULT    022u
#define VFS64_EACCES           13
#define VFS64_EPERM            1

/* 一次调用期间"调用方身份" */
struct Vfs64Cred64 {
    uint32_t uid, gid, euid, egid;
};

/* 设置/读取会话身份 */
void vfs64_set_cred64(uint32_t uid, uint32_t gid, uint32_t euid, uint32_t egid);
void vfs64_get_cred64(struct Vfs64Cred64* out);

/* 进程身份发布 */
void vfs64_set_proc_cred64(int have, uint32_t uid, uint32_t gid, uint32_t euid, uint32_t egid);

/* umask */
uint32_t vfs64_umask64(uint32_t new_mask);
uint32_t vfs64_get_umask64(void);

/* access(2) */
int vfs64_access64(const char* path, uint32_t mask);

/* chmod / chown */
int vfs64_chmod64(const char* path, uint32_t mode);
int vfs64_chown64(const char* path, uint32_t uid, uint32_t gid);
int vfs64_chmod_on64(int slot, const char* path, uint32_t mode);
int vfs64_chown_on64(int slot, const char* path, uint32_t uid, uint32_t gid);

/* 卷槽作用域 */
int  vfs64_scope_enter64(int slot, int* out_saved);
void vfs64_scope_leave64(int saved_slot);

/* 块数计算 */
uint32_t vfs64_blocks_for_bytes64(uint32_t bytes);
uint32_t vfs64_max_file_bytes64(void);

/* inode 类型 */
#define VFS64_TYPE_FREE        0u
#define VFS64_TYPE_FILE        1u
#define VFS64_TYPE_DIR         2u

/* 类型判定 kind */
#define VFS64_KIND_NONE        0u
#define VFS64_KIND_FILE        1u
#define VFS64_KIND_DIR         2u
#define VFS64_KIND_VAP         3u
#define VFS64_KIND_ELF         4u
#define VFS64_KIND_TEXT        5u
#define VFS64_KIND_BIN         6u

/* 元数据结构 */
struct Vfs64Info64 {
    uint32_t index;
    uint32_t type;
    uint32_t size;
    uint32_t mtime;
    uint32_t parent;
    uint32_t nlink;
    uint32_t kind;
    uint32_t uid;
    uint32_t gid;
    uint32_t mode;
    char     name[32];
    uint32_t name_len;
};

struct Vfs64Dirent64 {
    char     name[32];
    uint32_t index;
    uint32_t type;
    uint32_t size;
    uint32_t mtime;
    uint32_t kind;
    uint32_t uid;
    uint32_t gid;
    uint32_t mode;
};

struct Vfs64Time64 {
    uint16_t year;
    uint8_t  month, day, hour, minute, second;
};

struct Vfs64VolInfo64 {
    uint32_t version;
    uint32_t blocks;
    uint32_t inodes;
    uint32_t inode_bytes;
    uint32_t bitmap_start;
    uint32_t bitmap_blocks;
    uint32_t data_start;
    uint32_t data_blocks;
    uint32_t free_blocks;
};

/* 时间戳/类型判定 */
uint32_t vfs64_pack_time64(int year, int month, int day, int hour, int minute, int second);
void     vfs64_unpack_time64(uint32_t packed, struct Vfs64Time64* out);
uint32_t vfs64_now64(void);
void     vfs64_time_str64(uint32_t packed, char* ymd, int ymd_cap, char* hms, int hms_cap);
uint32_t vfs64_kind_of_data64(const void* data, uint32_t len, const char* name, uint32_t name_len);
uint32_t vfs64_kind_by_name64(uint32_t type, const char* name, uint32_t name_len);
const char* vfs64_kind_str64(uint32_t kind);

/* ==================== 卷生命周期 ==================== */
int  vfs64_format(int drive, uint32_t start_lba, uint32_t total_sectors);
int  vfs64_mount(int drive, uint32_t start_lba);
int  vfs64_probe_volume64(int drive, uint32_t start_lba, struct Vfs64VolInfo64* out);
int  vfs64_mounted_volume64(int* drive, uint32_t* start_lba, struct Vfs64VolInfo64* out);

/* 多卷 */
int  vfs64_mount_system64(int drive, uint32_t start_lba);
int  vfs64_mount_slot64(int slot, int drive, uint32_t start_lba);
int  vfs64_activate_slot64(int slot);
int  vfs64_current_slot64(void);
int  vfs64_system_slot64(void);
int  vfs64_slot_used64(int slot);
int  vfs64_slot_alloc64(void);
int  vfs64_slot_find64(int drive, uint32_t start_lba);
int  vfs64_slot_info64(int slot, int* drive, uint32_t* start_lba, struct Vfs64VolInfo64* out);
void vfs64_slots_dump64(void);

/* 按槽操作 */
int  vfs64_stat_on64(int slot, const char* path, struct Vfs64Info64* out);
int  vfs64_read_on64(int slot, const char* path, void* buf, int max);
int  vfs64_write_on64(int slot, const char* path, const void* buf, int len);
int  vfs64_mkdir_on64(int slot, const char* path);
int  vfs64_create_on64(int slot, const char* path);
int  vfs64_unlink_on64(int slot, const char* path);
int  vfs64_rmdir_on64(int slot, const char* path);
int  vfs64_ls_on64(int slot, const char* path, char names[][32], int max, uint32_t* sizes);
int  vfs64_stat64_on64(int slot, const char* path, struct Vfs64Info64* out);
int  vfs64_list64_on64(int slot, const char* path, struct Vfs64Dirent64* out, int max, uint32_t* cursor);
int  vfs64_tree_dump64_on64(int slot, const char* path, int max_entries, int max_depth);
int  vfs64_rename_on64(int slot, const char* old_path, const char* new_name);
int  vfs64_free_on64(int slot, uint32_t* free_blocks, uint32_t* free_bytes, uint32_t* total_blocks);
int  vfs64_inode_stats64(uint32_t* files, uint32_t* ffree);
int  vfs64_inode_stats_on64(int slot, uint32_t* files, uint32_t* ffree);

/* 多级路径 API */
int  vfs64_stat64(const char* path, struct Vfs64Info64* out);
int  vfs64_list64(const char* path, struct Vfs64Dirent64* out, int max, uint32_t* cursor);
int  vfs64_opendir64(const char* path, int* out_handle);
int  vfs64_readdir64(int handle, struct Vfs64Dirent64* out);
int  vfs64_closedir64(int handle);
int  vfs64_mkdir64(const char* path);
int  vfs64_create64(const char* path);
int  vfs64_write64(const char* path, const void* buf, int len);
int  vfs64_read64(const char* path, void* buf, int max);

/* 大文件读写 */
int  vfs64_read_at64(const char* path, uint32_t off, void* buf, uint32_t len, uint32_t* out_got);
int  vfs64_write_at64(const char* path, uint32_t off, const void* buf, uint32_t len);
typedef int (*Vfs64Src64)(void* ctx, uint32_t off, void* dst, uint32_t want);
int  vfs64_write_stream64(const char* path, uint32_t len, int (*src)(void*, uint32_t, void*, uint32_t), void* ctx);
int  vfs64_read_at_on64(int slot, const char* path, uint32_t off, void* buf, uint32_t len, uint32_t* out_got);
int  vfs64_write_at_on64(int slot, const char* path, uint32_t off, const void* buf, uint32_t len);
int  vfs64_write_stream_on64(int slot, const char* path, uint32_t len, int (*src)(void*, uint32_t, void*, uint32_t), void* ctx);

/* unlink/rmdir/rename */
int  vfs64_unlink64(const char* path);
int  vfs64_rmdir64(const char* path);
int  vfs64_rename64(const char* old_path, const char* new_name);
int  vfs64_rename_to64(const char* old_path, const char* new_path);
int  vfs64_set_mtime64(const char* path, uint32_t packed_time);
int  vfs64_free64(uint32_t* free_blocks, uint32_t* free_bytes, uint32_t* total_blocks);
int  vfs64_tree_dump64(const char* path, int max_entries, int max_depth);

/* 兼容 API */
int  vfs64_ls(const char* path, char names[][32], int max, uint32_t* sizes);
int  vfs64_read(const char* path, void* buf, int max);
int  vfs64_write(const char* path, const void* buf, int len);
int  vfs64_unlink(const char* path);
int  vfs64_mkdir(const char* path);
int  vfs64_stat(const char* path, uint32_t* type, uint32_t* size);
int  vfs64_selftest64(void);
void vfs64_dump64(void);

#endif /* STITCH_VFS64_H */