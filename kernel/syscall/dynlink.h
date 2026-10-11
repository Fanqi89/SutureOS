/* SutureOS: musl 动态链接器接口
 * 实现 musl libc 所需的动态链接器功能
 * 参考: musl src/ldso/dynlink.c
 */
#ifndef STITCH_DYNLINK_H
#define STITCH_DYNLINK_H

#include <stitch/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 动态链接器状态 */
struct dynlink_state {
    /* 已加载的共享库链表 */
    struct dso *dso_list;
    /* 主程序 DSO */
    struct dso *main_dso;
    /* 搜索路径 */
    const char **search_paths;
    int search_path_count;
};

/* 共享对象 (DSO) 结构 */
struct dso {
    struct dso *next;
    char *name;
    char *path;
    void *base;
    size_t size;
    uint64_t entry;
    /* 符号表 */
    void *symtab;
    char *strtab;
    size_t symtab_size;
    /* 重定位表 */
    void *rel;
    size_t rel_size;
    void *rela;
    size_t rela_size;
    void *plt_rel;
    size_t plt_rel_size;
    void *plt_rela;
    size_t plt_rela_size;
    /* 动态段 */
    void *dynamic;
    /* 依赖 */
    char **needed;
    int needed_count;
    /* 标志 */
    int loaded;
    int global;
    /* TLS */
    void *tls_image;
    size_t tls_size;
    size_t tls_align;
};

/* 初始化动态链接器 */
int dynlink_init(void);

/* 加载共享库 */
int dynlink_load_library(const char *name, int flags, void **out_handle);

/* 解析符号 */
void *dynlink_resolve_symbol(const char *name, struct dso *from);

/* dlopen 兼容接口 */
void *dlopen(const char *filename, int flags);
void *dlsym(void *handle, const char *symbol);
int dlclose(void *handle);
const char *dlerror(void);

/* 加载 musl libc 共享库 */
int dynlink_load_musl_libc(void);

/* 设置动态链接器搜索路径 */
int dynlink_add_search_path(const char *path);

/* 卸载所有共享库 */
void dynlink_cleanup(void);

#ifdef __cplusplus
}
#endif

#endif /* STITCH_DYNLINK_H */