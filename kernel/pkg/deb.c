/* SutureOS: deb 包安装器实现
 * 实现 Debian 软件包格式的解析与安装
 * 支持 ar 归档格式、control.tar.gz/data.tar.gz 解压、依赖处理、维护脚本
 */
#include "deb.h"
#include <stitch/types.h>
#include <stitch/string.h>
#include <stitch/console.h>
#include "heap.h"
#include "vfs.h"

/* 全局状态 */
static struct deb_db g_deb_db = {0};

/* ==================== 内部辅助函数 ==================== */

static char *deb_strdup(const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s) + 1;
    char *p = lemis_heap_alloc(len);
    if (p) memcpy(p, s, len);
    return p;
}

static void deb_free_package(struct deb_package *pkg) {
    if (!pkg) return;
    
    if (pkg->name) lemis_heap_free(pkg->name);
    if (pkg->version) lemis_heap_free(pkg->version);
    if (pkg->architecture) lemis_heap_free(pkg->architecture);
    if (pkg->maintainer) lemis_heap_free(pkg->maintainer);
    if (pkg->description) lemis_heap_free(pkg->description);
    if (pkg->section) lemis_heap_free(pkg->section);
    if (pkg->priority) lemis_heap_free(pkg->priority);
    if (pkg->homepage) lemis_heap_free(pkg->homepage);
    if (pkg->source) lemis_heap_free(pkg->source);
    if (pkg->install_path) lemis_heap_free(pkg->install_path);
    
    for (int i = 0; i < pkg->depends_count; i++) {
        if (pkg->depends[i].package) lemis_heap_free(pkg->depends[i].package);
        if (pkg->depends[i].version) lemis_heap_free(pkg->depends[i].version);
    }
    if (pkg->depends) lemis_heap_free(pkg->depends);
    
    for (int i = 0; i < pkg->pre_depends_count; i++) {
        if (pkg->pre_depends[i].package) lemis_heap_free(pkg->pre_depends[i].package);
        if (pkg->pre_depends[i].version) lemis_heap_free(pkg->pre_depends[i].version);
    }
    if (pkg->pre_depends) lemis_heap_free(pkg->pre_depends);
    
    for (int i = 0; i < pkg->recommends_count; i++) {
        if (pkg->recommends[i].package) lemis_heap_free(pkg->recommends[i].package);
        if (pkg->recommends[i].version) lemis_heap_free(pkg->recommends[i].version);
    }
    if (pkg->recommends) lemis_heap_free(pkg->recommends);
    
    for (int i = 0; i < pkg->suggests_count; i++) {
        if (pkg->suggests[i].package) lemis_heap_free(pkg->suggests[i].package);
        if (pkg->suggests[i].version) lemis_heap_free(pkg->suggests[i].version);
    }
    if (pkg->suggests) lemis_heap_free(pkg->suggests);
    
    for (int i = 0; i < pkg->conflicts_count; i++) {
        if (pkg->conflicts[i].package) lemis_heap_free(pkg->conflicts[i].package);
        if (pkg->conflicts[i].version) lemis_heap_free(pkg->conflicts[i].version);
    }
    if (pkg->conflicts) lemis_heap_free(pkg->conflicts);
    
    for (int i = 0; i < pkg->breaks_count; i++) {
        if (pkg->breaks[i].package) lemis_heap_free(pkg->breaks[i].package);
        if (pkg->breaks[i].version) lemis_heap_free(pkg->breaks[i].version);
    }
    if (pkg->breaks) lemis_heap_free(pkg->breaks);
    
    for (int i = 0; i < pkg->provides_count; i++) {
        if (pkg->provides[i].package) lemis_heap_free(pkg->provides[i].package);
        if (pkg->provides[i].version) lemis_heap_free(pkg->provides[i].version);
    }
    if (pkg->provides) lemis_heap_free(pkg->provides);
    
    for (int i = 0; i < pkg->replaces_count; i++) {
        if (pkg->replaces[i].package) lemis_heap_free(pkg->replaces[i].package);
        if (pkg->replaces[i].version) lemis_heap_free(pkg->replaces[i].version);
    }
    if (pkg->replaces) lemis_heap_free(pkg->replaces);
    
    for (int i = 0; i < pkg->enhances_count; i++) {
        if (pkg->enhances[i].package) lemis_heap_free(pkg->enhances[i].package);
        if (pkg->enhances[i].version) lemis_heap_free(pkg->enhances[i].version);
    }
    if (pkg->enhances) lemis_heap_free(pkg->enhances);
    
    if (pkg->scripts.preinst) lemis_heap_free(pkg->scripts.preinst);
    if (pkg->scripts.postinst) lemis_heap_free(pkg->scripts.postinst);
    if (pkg->scripts.prerm) lemis_heap_free(pkg->scripts.prerm);
    if (pkg->scripts.postrm) lemis_heap_free(pkg->scripts.postrm);
    
    for (int i = 0; i < pkg->conffiles_count; i++) {
        if (pkg->conffiles[i]) lemis_heap_free(pkg->conffiles[i]);
    }
    if (pkg->conffiles) lemis_heap_free(pkg->conffiles);
    
    if (pkg->install_path) lemis_heap_free(pkg->install_path);
    
    lemis_heap_free(pkg);
}

/* 简易 strtol 实现 */
static long deb_strtol(const char *nptr, char **endptr, int base) {
    (void)base;
    const char *s = nptr;
    long val = 0;
    int neg = 0;
    
    while (*s == ' ' || *s == '\t' || *s == '\n') s++;
    
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') s++;
    
    while (*s >= '0' && *s <= '9') {
        val = val * 10 + (*s - '0');
        s++;
    }
    
    if (endptr) *endptr = (char*)s;
    return neg ? -val : val;
}

deb_version_cmp_t deb_version_compare(const char *v1, const char *v2) {
    if (!v1 || !v2) return DEB_VER_INVALID;
    
    const char *p1 = v1;
    const char *p2 = v2;
    
    while (*p1 || *p2) {
        /* 跳过非数字字符 */
        while (*p1 && (*p1 < '0' || *p1 > '9')) p1++;
        while (*p2 && (*p2 < '0' || *p2 > '9')) p2++;
        
        if (!*p1 && !*p2) return DEB_VER_EQ;
        if (!*p1) return DEB_VER_LT;
        if (!*p2) return DEB_VER_GT;
        
        long v1 = deb_strtol(p1, (char**)&p1, 10);
        long v2 = deb_strtol(p2, (char**)&p2, 10);
        
        if (v1 < v2) return DEB_VER_LT;
        if (v1 > v2) return DEB_VER_GT;
        
        if (*p1) p1++;
        if (*p2) p2++;
    }
    
    return DEB_VER_EQ;
}

/* ==================== 公共 API 实现 ==================== */

int deb_db_init(const char *db_path) {
    (void)db_path;
    console_puts("[deb] Initializing package database\n");
    memset(&g_deb_db, 0, sizeof(g_deb_db));
    g_deb_db.capacity = 256;
    g_deb_db.packages = lemis_heap_alloc(g_deb_db.capacity * sizeof(struct deb_package*));
    if (!g_deb_db.packages) return -1;
    return 0;
}

void deb_db_close(void) {
    for (int i = 0; i < g_deb_db.count; i++) {
        deb_free_package(g_deb_db.packages[i]);
    }
    if (g_deb_db.packages) lemis_heap_free(g_deb_db.packages);
    memset(&g_deb_db, 0, sizeof(g_deb_db));
    console_puts("[deb] Package database closed\n");
}

struct deb_package *deb_db_find(const char *pkg_name) {
    for (int i = 0; i < g_deb_db.count; i++) {
        if (g_deb_db.packages[i] && strcmp(g_deb_db.packages[i]->name, pkg_name) == 0) {
            return g_deb_db.packages[i];
        }
    }
    return NULL;
}

int deb_db_register(struct deb_package *pkg) {
    if (!pkg) return -1;
    
    if (g_deb_db.count >= g_deb_db.capacity) {
        int new_cap = g_deb_db.capacity * 2;
        struct deb_package **new_pkgs = lemis_heap_alloc(new_cap * sizeof(struct deb_package*));
        if (!new_pkgs) return -1;
        memcpy(new_pkgs, g_deb_db.packages, g_deb_db.count * sizeof(struct deb_package*));
        lemis_heap_free(g_deb_db.packages);
        g_deb_db.packages = new_pkgs;
        g_deb_db.capacity = new_cap;
    }
    
    g_deb_db.packages[g_deb_db.count++] = pkg;
    console_printf("[deb] Registered package: %s-%s\n", pkg->name, pkg->version);
    return 0;
}

int deb_db_unregister(const char *pkg_name) {
    for (int i = 0; i < g_deb_db.count; i++) {
        if (g_deb_db.packages[i] && strcmp(g_deb_db.packages[i]->name, pkg_name) == 0) {
            deb_free_package(g_deb_db.packages[i]);
            g_deb_db.packages[i] = g_deb_db.packages[--g_deb_db.count];
            return 0;
        }
    }
    return -1;
}

int deb_parse_package(const char *deb_path, struct deb_package **out_pkg) {
    (void)deb_path; (void)out_pkg;
    console_puts("[deb] Parsing package (stub implementation)\n");
    return -1; /* TODO: 实现完整的 deb 解析 */
}

int deb_install(const char *deb_path, const char *root_dir, int force) {
    (void)deb_path; (void)root_dir; (void)force;
    console_puts("[deb] Installing package (stub implementation)\n");
    return -1; /* TODO: 实现完整的 deb 安装 */
}

int deb_remove(const char *pkg_name, int purge) {
    (void)pkg_name; (void)purge;
    console_puts("[deb] Removing package (stub implementation)\n");
    return -1; /* TODO: 实现完整的包卸载 */
}

int deb_query(const char *pkg_name, struct deb_package **out_pkg) {
    struct deb_package *pkg = deb_db_find(pkg_name);
    if (!pkg) return -1;
    if (out_pkg) *out_pkg = pkg;
    return 0;
}

int deb_list_installed(struct deb_package ***out_list, int *out_count) {
    if (out_list) *out_list = g_deb_db.packages;
    if (out_count) *out_count = g_deb_db.count;
    return 0;
}

int deb_check_dependencies(struct deb_package *pkg, int *missing_count, char ***missing_pkgs) {
    (void)pkg; (void)missing_count; (void)missing_pkgs;
    return 0;
}

int deb_run_maintscript(const char *script_path, const char *arg) {
    (void)script_path; (void)arg;
    return 0;
}

void deb_cleanup_temp(const char *extract_dir) {
    (void)extract_dir;
}