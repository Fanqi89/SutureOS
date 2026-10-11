/* SutureOS: deb 包安装器
 * 实现 Debian 软件包格式的解析与安装
 * 支持 ar 归档格式、control.tar.gz/data.tar.gz 解压、依赖处理、维护脚本
 */
#ifndef STITCH_DEB_H
#define STITCH_DEB_H

#include <stitch/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* deb 包格式常量 */
#define DEB_MAGIC "!<arch>\n"
#define DEB_MAGIC_LEN 8

/* ar 归档头部 */
#define AR_HDR_NAME_LEN 16
#define AR_HDR_DATE_LEN 12
#define AR_HDR_UID_LEN 6
#define AR_HDR_GID_LEN 6
#define AR_HDR_MODE_LEN 8
#define AR_HDR_SIZE_LEN 10
#define AR_HDR_FMAG "`\n"

/* 控制文件字段最大长度 */
#define DEB_CTRL_FIELD_MAX 1024
#define DEB_MAX_DEPENDS 64
#define DEB_MAX_PROVIDES 32

/* 包状态 */
typedef enum {
    DEB_STATUS_NOT_INSTALLED = 0,
    DEB_STATUS_INSTALLED,
    DEB_STATUS_HALF_INSTALLED,
    DEB_STATUS_HALF_CONFIGURED,
    DEB_STATUS_UNPACKED,
    DEB_STATUS_CONFIG_FILES
} deb_pkg_status_t;

/* 依赖关系类型 */
typedef enum {
    DEB_DEP_PREDEPENDS = 0,
    DEB_DEP_DEPENDS,
    DEB_DEP_RECOMMENDS,
    DEB_DEP_SUGGESTS,
    DEB_DEP_ENHANCES,
    DEB_DEP_CONFLICTS,
    DEB_DEP_BREAKS,
    DEB_DEP_PROVIDES,
    DEB_DEP_REPLACES
} deb_dep_type_t;

/* 版本比较结果 */
typedef enum {
    DEB_VER_LT = -1,
    DEB_VER_EQ = 0,
    DEB_VER_GT = 1,
    DEB_VER_INVALID = -2
} deb_version_cmp_t;

/* ar 归档头部 */
struct ar_hdr {
    char name[AR_HDR_NAME_LEN];
    char date[AR_HDR_DATE_LEN];
    char uid[AR_HDR_UID_LEN];
    char gid[AR_HDR_GID_LEN];
    char mode[AR_HDR_MODE_LEN];
    char size[AR_HDR_SIZE_LEN];
    char fmag[2];
};

/* deb 控制文件字段 */
struct deb_control_field {
    char *name;
    char *value;
};

/* 依赖关系 */
struct deb_dependency {
    char *package;
    char *version;    /* 版本限制，如 ">= 1.0" */
    deb_dep_type_t type;
};

/* 维护脚本 */
struct deb_maintscript {
    char *preinst;    /* 安装前 */
    char *postinst;   /* 安装后 */
    char *prerm;      /* 卸载前 */
    char *postrm;     /* 卸载后 */
};

/* deb 包元数据 */
struct deb_package {
    char *name;
    char *version;
    char *architecture;
    char *maintainer;
    char *description;
    char *section;
    char *priority;
    char *homepage;
    char *source;
    uint64_t installed_size;  /* KB */
    
    /* 依赖关系 */
    struct deb_dependency *depends;
    int depends_count;
    struct deb_dependency *pre_depends;
    int pre_depends_count;
    struct deb_dependency *recommends;
    int recommends_count;
    struct deb_dependency *suggests;
    int suggests_count;
    struct deb_dependency *conflicts;
    int conflicts_count;
    struct deb_dependency *breaks;
    int breaks_count;
    struct deb_dependency *provides;
    int provides_count;
    struct deb_dependency *replaces;
    int replaces_count;
    struct deb_dependency *enhances;
    int enhances_count;
    
    /* 维护脚本 */
    struct deb_maintscript scripts;
    
    /* 文件列表 */
    char **conffiles;   /* 配置文件列表 */
    int conffiles_count;
    
    /* 安装状态 */
    deb_pkg_status_t status;
    char *install_path;  /* 安装根目录 */
};

/* 安装上下文 */
struct deb_install_ctx {
    char *deb_path;        /* .deb 文件路径 */
    char *extract_dir;     /* 解压目录 */
    char *root_dir;        /* 安装根目录 */
    struct deb_package *pkg;
    int force_overwrite;   /* 强制覆盖 */
    int ignore_deps;       /* 忽略依赖 */
    int dry_run;           /* 仅模拟 */
};

/* 已安装包数据库 */
struct deb_db {
    struct deb_package **packages;
    int count;
    int capacity;
};

/* ==================== 公共 API ==================== */

/* 解析 deb 包文件 */
int deb_parse_package(const char *deb_path, struct deb_package **out_pkg);

/* 安装 deb 包 */
int deb_install(const char *deb_path, const char *root_dir, int force);

/* 卸载包 */
int deb_remove(const char *pkg_name, int purge);

/* 查询包信息 */
int deb_query(const char *pkg_name, struct deb_package **out_pkg);

/* 列出已安装包 */
int deb_list_installed(struct deb_package ***out_list, int *out_count);

/* 检查依赖 */
int deb_check_dependencies(struct deb_package *pkg, int *missing_count, char ***missing_pkgs);

/* 比较版本 */
deb_version_cmp_t deb_version_compare(const char *v1, const char *v2);

/* 运行维护脚本 */
int deb_run_maintscript(const char *script_path, const char *arg);

/* 初始化包数据库 */
int deb_db_init(const char *db_path);

/* 关闭包数据库 */
void deb_db_close(void);

/* 注册已安装包 */
int deb_db_register(struct deb_package *pkg);

/* 从数据库移除包 */
int deb_db_unregister(const char *pkg_name);

/* 查找已安装包 */
struct deb_package *deb_db_find(const char *pkg_name);

/* 清理临时文件 */
void deb_cleanup_temp(const char *extract_dir);

#ifdef __cplusplus
}
#endif

#endif /* STITCH_DEB_H */