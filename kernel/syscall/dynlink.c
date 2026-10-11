/* SutureOS: musl 动态链接器实现
 * 参考: musl src/ldso/dynlink.c
 * 简化实现：仅支持基本的 dlopen/dlsym/dlclose/dlerror
 */
#include "dynlink.h"
#include <stitch/types.h>
#include <stitch/string.h>
#include <stitch/console.h>
#include "heap.h"
#include "vfs.h"
#include "elf.h"
#include "loader.h"
#include <stitch/string.h>

/* 全局状态 */
static struct dynlink_state g_dynlink = {0};
static char g_dlerror_buf[256] = {0};

/* vfs_read_file 前向声明 */
extern void *vfs_read_file(const char *path, size_t *len);

/* dynlink_strdup 前向声明 */
static char *dynlink_strdup(const char *s);

/* ==================== 错误处理 ==================== */

static void dynlink_set_error(const char *msg) {
    strncpy(g_dlerror_buf, msg, sizeof(g_dlerror_buf) - 1);
    g_dlerror_buf[sizeof(g_dlerror_buf) - 1] = '\0';
    console_printf("[dynlink] ERROR: %s\n", msg);
}

const char *dlerror(void) {
    if (g_dlerror_buf[0]) {
        const char *ret = g_dlerror_buf;
        g_dlerror_buf[0] = '\0';
        return ret;
    }
    return NULL;
}

/* 简易 strdup 实现 (使用 lemis_heap) */
static char *dynlink_strdup(const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s) + 1;
    char *p = lemis_heap_alloc(len);
    if (p) memcpy(p, s, len);
    return p;
}

/* ==================== 内部辅助函数 ==================== */

static void *read_elf_section(const void *elf, const char *section_name, size_t *out_size) {
    (void)elf; (void)section_name; (void)out_size;
    /* 简化实现：暂不解析 section header */
    return NULL;
}

static int load_dso_from_file(const char *path, struct dso **out_dso) {
    size_t len;
    void *buf = vfs_read_file(path, &len);
    if (!buf) {
        dynlink_set_error("Failed to read DSO file");
        return -1;
    }

    /* 简单的 ELF 校验 */
    if (len < 64) {
        dynlink_set_error("File too small to be ELF");
        return -1;
    }

    const uint8_t *elf = buf;
    if (elf[0] != 0x7f || elf[1] != 'E' || elf[2] != 'L' || elf[3] != 'F') {
        dynlink_set_error("Not a valid ELF file");
        return -1;
    }

    struct dso *dso = lemis_heap_alloc(sizeof(struct dso));
    if (!dso) {
        dynlink_set_error("Failed to allocate DSO structure");
        return -1;
    }
    memset(dso, 0, sizeof(struct dso));

    dso->name = dynlink_strdup(path);
    dso->path = dynlink_strdup(path);
    dso->base = buf;
    dso->size = len;
    dso->loaded = 1;

    *out_dso = dso;
    return 0;
}

/* ==================== 公共 API ==================== */

int dynlink_init(void) {
    console_puts("[dynlink] Initializing dynamic linker\n");
    memset(&g_dynlink, 0, sizeof(g_dynlink));
    return 0;
}

int dynlink_load_library(const char *name, int flags, void **out_handle) {
    (void)flags;

    if (!name) {
        dynlink_set_error("Library name is NULL");
        return -1;
    }

    console_printf("[dynlink] Loading library: %s\n", name);

    /* 搜索路径 */
    const char *search_paths[] = {
        "/system/lib/",
        "/lib/",
        "/usr/lib/",
        "",
    };

    char full_path[512];
    struct dso *dso = NULL;

    for (int i = 0; i < 4; i++) {
        if (search_paths[i][0] == '\0') {
            /* 尝试直接路径 */
            if (load_dso_from_file(name, &dso) == 0) {
                break;
            }
        } else {
            /* 简化路径拼接 */
            char *p = full_path;
            const char *s = search_paths[i];
            while (*s) *p++ = *s++;
            const char *n = name;
            while (*n) *p++ = *n++;
            *p = '\0';
            if (load_dso_from_file(full_path, &dso) == 0) {
                break;
            }
        }
    }

    if (!dso) {
        dynlink_set_error("Library not found");
        return -1;
    }

    /* 加入全局链表 */
    dso->next = g_dynlink.dso_list;
    g_dynlink.dso_list = dso;

    if (out_handle) *out_handle = dso;

    console_printf("[dynlink] Loaded: %s at %p\n", dso->name, dso->base);
    return 0;
}

void *dynlink_resolve_symbol(const char *name, struct dso *from) {
    (void)name; (void)from;

    /* 简化实现：遍历已加载的 DSO 查找符号 */
    for (struct dso *dso = g_dynlink.dso_list; dso; dso = dso->next) {
        if (dso->symtab && dso->strtab) {
            /* 这里需要真实解析 .dynsym 和 .dynstr */
            /* 简化实现：返回 NULL */
        }
    }

    return NULL;
}

void *dlopen(const char *filename, int flags) {
    void *handle = NULL;
    int ret = dynlink_load_library(filename, flags, &handle);
    if (ret != 0) {
        return NULL;
    }
    return handle;
}

void *dlsym(void *handle, const char *symbol) {
    if (!handle || !symbol) {
        dynlink_set_error("Invalid handle or symbol name");
        return NULL;
    }

    struct dso *dso = (struct dso *)handle;
    (void)dso;

    /* 简化实现：全局符号查找 */
    return dynlink_resolve_symbol(symbol, dso);
}

int dlclose(void *handle) {
    if (!handle) {
        dynlink_set_error("Invalid handle");
        return -1;
    }

    struct dso *dso = (struct dso *)handle;

    /* 从链表中移除 */
    struct dso **pp = &g_dynlink.dso_list;
    while (*pp) {
        if (*pp == dso) {
            *pp = dso->next;
            break;
        }
        pp = &(*pp)->next;
    }

    /* 释放资源 */
    if (dso->base) lemis_heap_free(dso->base);
    if (dso->name) lemis_heap_free(dso->name);
    if (dso->path) lemis_heap_free(dso->path);
    lemis_heap_free(dso);

    return 0;
}

int dynlink_load_musl_libc(void) {
    console_puts("[dynlink] Loading musl libc...\n");
    return dynlink_load_library("libc.so", 0, NULL);
}

int dynlink_add_search_path(const char *path) {
    if (!path || g_dynlink.search_path_count >= 16) {
        return -1;
    }

    g_dynlink.search_paths[g_dynlink.search_path_count] = dynlink_strdup(path);
    g_dynlink.search_path_count++;
    return 0;
}

void dynlink_cleanup(void) {
    struct dso *dso = g_dynlink.dso_list;
    while (dso) {
        struct dso *next = dso->next;
        if (dso->base) lemis_heap_free(dso->base);
        if (dso->name) lemis_heap_free(dso->name);
        if (dso->path) lemis_heap_free(dso->path);
        lemis_heap_free(dso);
        dso = next;
    }
    g_dynlink.dso_list = NULL;
    console_puts("[dynlink] Cleaned up\n");
}