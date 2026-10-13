/* SutureOS: musl 动态链接器实现
 * 完整实现：ELF 解析、符号表、重定位、依赖加载、TLS
 * 参考: musl src/ldso/dynlink.c
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

/* ==================== 内部辅助函数 ==================== */

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

static char *dynlink_strdup(const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s) + 1;
    char *p = lemis_heap_alloc(len);
    if (p) memcpy(p, s, len);
    return p;
}

/* ==================== ELF 解析辅助 ==================== */

static const Elf64_Phdr *elf_get_phdr(const void *elf) {
    const Elf64_Ehdr *ehdr = (const Elf64_Ehdr *)elf;
    if (ehdr->e_phoff == 0) return NULL;
    return (const Elf64_Phdr *)((const uint8_t *)elf + ehdr->e_phoff);
}

static const Elf64_Shdr *elf_get_shdr(const void *elf) {
    const Elf64_Ehdr *ehdr = (const Elf64_Ehdr *)elf;
    if (ehdr->e_shoff == 0) return NULL;
    return (const Elf64_Shdr *)((const uint8_t *)elf + ehdr->e_shoff);
}

static const char *elf_get_shstrtab(const void *elf) {
    const Elf64_Ehdr *ehdr = (const Elf64_Ehdr *)elf;
    const Elf64_Shdr *shdr = elf_get_shdr(elf);
    if (!shdr || ehdr->e_shstrndx == SHN_UNDEF) return NULL;
    const Elf64_Shdr *shstrtab = &shdr[ehdr->e_shstrndx];
    return (const char *)((const uint8_t *)elf + shstrtab->sh_offset);
}

static const Elf64_Shdr *elf_find_section(const void *elf, const char *name) {
    const Elf64_Shdr *shdr = elf_get_shdr(elf);
    if (!shdr) return NULL;
    const Elf64_Ehdr *ehdr = (const Elf64_Ehdr *)elf;
    const char *shstrtab = elf_get_shstrtab(elf);
    if (!shstrtab) return NULL;

    for (int i = 0; i < ehdr->e_shnum; i++) {
        const char *sec_name = shstrtab + shdr[i].sh_name;
        if (strcmp(sec_name, name) == 0) {
            return &shdr[i];
        }
    }
    return NULL;
}

static void *elf_get_section_data(const void *elf, const Elf64_Shdr *shdr) {
    if (!shdr || shdr->sh_type == SHT_NOBITS) return NULL;
    return (uint8_t *)elf + shdr->sh_offset;
}

/* ==================== 符号表解析 ==================== */

static int dso_parse_symtab(struct dso *dso, const void *elf) {
    const Elf64_Shdr *dynsym = elf_find_section(elf, ".dynsym");
    const Elf64_Shdr *dynstr = elf_find_section(elf, ".dynstr");
    
    if (!dynsym || !dynstr) {
        console_puts("[dynlink] WARNING: .dynsym or .dynstr not found\n");
        return -1;
    }

    dso->symtab = elf_get_section_data(elf, dynsym);
    dso->strtab = (char *)elf_get_section_data(elf, dynstr);
    dso->symtab_size = dynsym->sh_size / sizeof(Elf64_Sym);
    
    console_printf("[dynlink] Parsed %zu symbols from %s\n", dso->symtab_size, dso->name);
    return 0;
}

/* ==================== 重定位处理 ==================== */

static int dso_apply_reloc(struct dso *dso, uint64_t offset, int type,
                           uint64_t sym_val, const Elf64_Sym *sym, int64_t addend) {
    uint64_t *where = (uint64_t *)((uint8_t *)dso->base + offset);
    uint64_t val = 0;
    
    switch (type) {
        case R_X86_64_RELATIVE:
            /* RELATIVE relocation uses r_addend from Elf64_Rela, for Elf64_Rel it's implicit */
            *where = (uint64_t)dso->base + (uint64_t)addend;
            return 0;
        case R_X86_64_64:
            *where = sym_val + (uint64_t)addend;
            return 0;
        case R_X86_64_32:
            *(uint32_t *)where = (uint32_t)(sym_val + (uint64_t)addend);
            return 0;
        case R_X86_64_PC32:
            *(uint32_t *)where = (uint32_t)(sym_val + (uint64_t)addend - (offset + (uint64_t)dso->base));
            return 0;
        case R_X86_64_PLT32:
            val = sym_val + (uint64_t)addend - (offset + (uint64_t)dso->base);
            *(uint32_t *)where = (uint32_t)val;
            return 0;
        case R_X86_64_JUMP_SLOT:
            *where = sym_val;
            return 0;
        case R_X86_64_GLOB_DAT:
            *where = sym_val;
            return 0;
        case R_X86_64_COPY:
            /* COPY relocation handled by loader */
            return 0;
        case R_X86_64_DTPMOD64:
            /* Module id for dynamic TLS: one module per loaded DSO, so the
             * DSO pointer itself serves as a unique module identifier. */
            *where = (uint64_t)dso;
            return 0;
        case R_X86_64_DTPOFF64: {
            /* Offset of the symbol inside the module's TLS block. */
            int64_t off = (sym ? (int64_t)sym->st_value : 0) + addend;
            *where = (uint64_t)off;
            return 0;
        }
        case R_X86_64_TPOFF64: {
            /* Initial-exec model, x86_64 Variant II: the thread pointer
             * points PAST the end of the static TLS block, so the offset of
             * a symbol is (offset in block) - (block size). */
            if (!dso->tls_image) {
                console_printf("[dynlink] ERROR: TPOFF64 reloc but %s has no PT_TLS\n",
                               dso->name ? dso->name : "?");
                return -1;
            }
            int64_t off = (sym ? (int64_t)sym->st_value : 0) + addend;
            *where = (uint64_t)(off - (int64_t)dso->tls_size);
            return 0;
        }
        default:
            console_printf("[dynlink] WARNING: Unknown relocation type %d\n", type);
            return 0;
    }
}

static int dso_apply_rela(struct dso *dso, const Elf64_Rela *rela) {
    int type = ELF64_R_TYPE(rela->r_info);
    uint64_t sym_idx = ELF64_R_SYM(rela->r_info);
    uint64_t sym_val = 0;
    const Elf64_Sym *sym = NULL;
    
    if (sym_idx != 0 && dso->symtab) {
        sym = &((const Elf64_Sym *)dso->symtab)[sym_idx];
        const char *sym_name = dso->strtab + sym->st_name;
        
        /* 查找符号值 */
        if (sym->st_shndx != SHN_UNDEF) {
            sym_val = (uint64_t)dso->base + sym->st_value;
        } else {
            /* 外部符号，需要从其他 DSO 查找 */
            sym_val = (uint64_t)dynlink_resolve_symbol(sym_name, dso);
            if (!sym_val) {
                console_printf("[dynlink] Unresolved symbol: %s\n", sym_name);
                return -1;
            }
        }
    }
    
    return dso_apply_reloc(dso, rela->r_offset, type, sym_val, sym,
                           rela->r_addend);
}

static int dso_process_relocations(struct dso *dso, const void *elf) {
    /* 处理 .rel.dyn 和 .rela.dyn */
    const Elf64_Shdr *rel = elf_find_section(elf, ".rel.dyn");
    const Elf64_Shdr *rela = elf_find_section(elf, ".rela.dyn");
    const Elf64_Shdr *pltrel = elf_find_section(elf, ".rel.plt");
    const Elf64_Shdr *pltrela = elf_find_section(elf, ".rela.plt");
    
    int ret = 0;
    
    if (rel) {
        const Elf64_Rel *rels = elf_get_section_data(elf, rel);
        size_t count = rel->sh_size / sizeof(Elf64_Rel);
        for (size_t i = 0; i < count; i++) {
            int type = ELF64_R_TYPE(rels[i].r_info);
            uint64_t sym_idx = ELF64_R_SYM(rels[i].r_info);
            uint64_t sym_val = 0;
            const Elf64_Sym *sym = NULL;
            
            if (sym_idx != 0 && dso->symtab) {
                sym = &((const Elf64_Sym *)dso->symtab)[sym_idx];
                if (sym->st_shndx != SHN_UNDEF) {
                    sym_val = (uint64_t)dso->base + sym->st_value;
                } else {
                    const char *sym_name = dso->strtab + sym->st_name;
                    sym_val = (uint64_t)dynlink_resolve_symbol(sym_name, dso);
                    if (!sym_val) {
                        console_printf("[dynlink] Unresolved symbol in .rel.dyn: %s\n", sym_name);
                        return -1;
                    }
                }
            }
            /* REL-style: the addend lives at the relocation target */
            int64_t addend = *(int64_t *)((uint8_t *)dso->base + rels[i].r_offset);
            if (dso_apply_reloc(dso, rels[i].r_offset, type, sym_val, sym,
                                addend) < 0) return -1;
        }
    }
    
    if (rela) {
        const Elf64_Rela *relas = elf_get_section_data(elf, rela);
        size_t count = rela->sh_size / sizeof(Elf64_Rela);
        for (size_t i = 0; i < count; i++) {
            if (dso_apply_rela(dso, &relas[i]) < 0) return -1;
        }
    }
    
    /* PLT 重定位 */
    if (pltrel) {
        const Elf64_Rel *rels = elf_get_section_data(elf, pltrel);
        size_t count = pltrel->sh_size / sizeof(Elf64_Rel);
        for (size_t i = 0; i < count; i++) {
            int type = ELF64_R_TYPE(rels[i].r_info);
            if (type == R_X86_64_JUMP_SLOT) {
                uint64_t sym_idx = ELF64_R_SYM(rels[i].r_info);
                if (sym_idx != 0 && dso->symtab) {
                    const Elf64_Sym *sym = &((const Elf64_Sym *)dso->symtab)[sym_idx];
                    const char *sym_name = dso->strtab + sym->st_name;
                    uint64_t sym_val = (uint64_t)dynlink_resolve_symbol(sym_name, dso);
                    if (!sym_val) {
                        console_printf("[dynlink] Unresolved PLT symbol: %s\n", sym_name);
                        return -1;
                    }
                    uint64_t *where = (uint64_t *)((uint8_t *)dso->base + rels[i].r_offset);
                    *where = sym_val;
                }
            }
        }
    }
    
    if (pltrela) {
        const Elf64_Rela *relas = elf_get_section_data(elf, pltrela);
        size_t count = pltrela->sh_size / sizeof(Elf64_Rela);
        for (size_t i = 0; i < count; i++) {
            int type = ELF64_R_TYPE(relas[i].r_info);
            if (type == R_X86_64_JUMP_SLOT) {
                uint64_t sym_idx = ELF64_R_SYM(relas[i].r_info);
                if (sym_idx != 0 && dso->symtab) {
                    const Elf64_Sym *sym = &((const Elf64_Sym *)dso->symtab)[sym_idx];
                    const char *sym_name = dso->strtab + sym->st_name;
                    uint64_t sym_val = (uint64_t)dynlink_resolve_symbol(sym_name, dso);
                    if (!sym_val) {
                        console_printf("[dynlink] Unresolved PLT symbol (rela): %s\n", sym_name);
                        return -1;
                    }
                    uint64_t *where = (uint64_t *)((uint8_t *)dso->base + relas[i].r_offset);
                    *where = sym_val;
                }
            }
        }
    }
    
    return 0;
}

/* ==================== 动态段解析 ==================== */

static int dso_parse_dynamic(struct dso *dso, const void *elf) {
    const Elf64_Shdr *dyn = elf_find_section(elf, ".dynamic");
    if (!dyn) {
        console_puts("[dynlink] WARNING: .dynamic section not found\n");
        return -1;
    }
    
    dso->dynamic = elf_get_section_data(elf, dyn);
    const Elf64_Dyn *dyn_arr = (const Elf64_Dyn *)dso->dynamic;
    
    for (size_t i = 0; dyn_arr[i].d_tag != DT_NULL; i++) {
        switch (dyn_arr[i].d_tag) {
            case DT_NEEDED:
                /* 记录依赖库名称索引 */
                if (dso->needed_count < 32) {
                    dso->needed[dso->needed_count++] = (char *)(dyn_arr[i].d_un.d_val);
                }
                break;
            case DT_STRTAB:
                if (!dso->strtab) dso->strtab = (char *)((uint8_t *)dso->base + dyn_arr[i].d_un.d_ptr);
                break;
            case DT_SYMTAB:
                if (!dso->symtab) dso->symtab = (void *)((uint8_t *)dso->base + dyn_arr[i].d_un.d_ptr);
                break;
            case DT_STRSZ:
                /* 字符串表大小 */
                break;
            case DT_SYMENT:
                /* 符号表项大小 */
                break;
            case DT_HASH:
                /* 哈希表 */
                break;
            case DT_REL:
                dso->rel = (void *)((uint8_t *)dso->base + dyn_arr[i].d_un.d_ptr);
                break;
            case DT_RELSZ:
                dso->rel_size = dyn_arr[i].d_un.d_val;
                break;
            case DT_RELENT:
                /* REL 项大小 */
                break;
            case DT_RELA:
                dso->rela = (void *)((uint8_t *)dso->base + dyn_arr[i].d_un.d_ptr);
                break;
            case DT_RELASZ:
                dso->rela_size = dyn_arr[i].d_un.d_val;
                break;
            case DT_RELAENT:
                /* RELA 项大小 */
                break;
            case DT_PLTREL:
                /* PLT 重定位类型 */
                break;
            case DT_PLTRELSZ:
                dso->plt_rel_size = dyn_arr[i].d_un.d_val;
                break;
            case DT_PLTGOT:
                dso->plt_rela = (void *)((uint8_t *)dso->base + dyn_arr[i].d_un.d_ptr);
                break;
            case DT_JMPREL:
                dso->plt_rel = (void *)((uint8_t *)dso->base + dyn_arr[i].d_un.d_ptr);
                break;
            case DT_FLAGS:
                /* 标志 */
                break;
            default:
                break;
        }
    }
    return 0;
}

/* ==================== DSO 加载与初始化 ==================== */

static int load_dso_from_file(const char *path, struct dso **out_dso) {
    size_t len;
    void *buf = vfs_read_file(path, &len);
    if (!buf) {
        dynlink_set_error("Failed to read DSO file");
        return -1;
    }

    if (len < 64) {
        dynlink_set_error("File too small to be ELF");
        return -1;
    }

    const uint8_t *elf = buf;
    if (elf[0] != 0x7f || elf[1] != 'E' || elf[2] != 'L' || elf[3] != 'F') {
        dynlink_set_error("Not a valid ELF file");
        return -1;
    }

    const Elf64_Ehdr *ehdr = (const Elf64_Ehdr *)elf;
    if (ehdr->e_type != ET_DYN && ehdr->e_type != ET_EXEC) {
        dynlink_set_error("Not a shared object or executable");
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

    /* 解析符号表 */
    if (dso_parse_symtab(dso, elf) < 0) {
        /* 非致命，某些库可能没有符号表 */
    }

    /* 解析动态段 */
    if (dso_parse_dynamic(dso, elf) < 0) {
        console_puts("[dynlink] WARNING: Failed to parse .dynamic\n");
    }

    /* 解析 PT_TLS：静态 TLS 块的镜像/大小/对齐（TLS 重定位需要） */
    {
        const Elf64_Phdr *ph = elf_get_phdr(elf);
        if (ph) {
            for (int i = 0; i < ehdr->e_phnum; i++) {
                if (ph[i].p_type == PT_TLS) {
                    dso->tls_image = (uint8_t *)elf + ph[i].p_offset;
                    dso->tls_size  = ph[i].p_memsz;
                    dso->tls_align = ph[i].p_align;
                    console_printf("[dynlink] %s PT_TLS: image=%p size=%zu align=%zu\n",
                                   path, dso->tls_image, dso->tls_size, dso->tls_align);
                    break;
                }
            }
        }
    }

    /* 处理重定位 */
    if (dso_process_relocations(dso, elf) < 0) {
        console_puts("[dynlink] ERROR: Relocation processing failed\n");
        lemis_heap_free(buf);
        lemis_heap_free(dso->name);
        lemis_heap_free(dso->path);
        lemis_heap_free(dso);
        return -1;
    }

    /* 找到入口点 */
    const Elf64_Phdr *phdr = elf_get_phdr(elf);
    if (phdr) {
        for (int i = 0; i < ehdr->e_phnum; i++) {
            if (phdr[i].p_type == PT_LOAD && (phdr[i].p_flags & PF_X)) {
                dso->entry = phdr[i].p_vaddr;
                break;
            }
        }
    }

    *out_dso = dso;
    return 0;
}

/* 处理 DT_NEEDED 依赖 */
static int dso_load_dependencies(struct dso *dso) {
    if (!dso->dynamic) return 0;
    
    const Elf64_Dyn *dyn = (const Elf64_Dyn *)dso->dynamic;
    const char *strtab = dso->strtab;
    
    for (size_t i = 0; dyn[i].d_tag != DT_NULL; i++) {
        if (dyn[i].d_tag == DT_NEEDED) {
            const char *needed_name = strtab + dyn[i].d_un.d_val;
            console_printf("[dynlink] Loading dependency: %s\n", needed_name);
            
            void *handle = NULL;
            if (dynlink_load_library(needed_name, 0, &handle) < 0) {
                console_printf("[dynlink] Failed to load dependency: %s\n", needed_name);
                return -1;
            }
        }
    }
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
            if (load_dso_from_file(name, &dso) == 0) break;
        } else {
            char *p = full_path;
            const char *s = search_paths[i];
            while (*s) *p++ = *s++;
            const char *n = name;
            while (*n) *p++ = *n++;
            *p = '\0';
            if (load_dso_from_file(full_path, &dso) == 0) break;
        }
    }

    if (!dso) {
        dynlink_set_error("Library not found");
        return -1;
    }

    /* 加载依赖 */
    if (dso_load_dependencies(dso) < 0) {
        console_puts("[dynlink] Failed to load dependencies\n");
        return -1;
    }

    /* 加入全局链表 */
    dso->next = g_dynlink.dso_list;
    g_dynlink.dso_list = dso;

    if (out_handle) *out_handle = dso;

    console_printf("[dynlink] Loaded: %s at %p (entry: %p)\n", dso->name, dso->base, (void *)dso->entry);
    return 0;
}

void *dynlink_resolve_symbol(const char *name, struct dso *from) {
    if (!name) return NULL;

    /* 首先在 from DSO 中查找（如果提供） */
    if (from && from->symtab && from->strtab) {
        for (size_t i = 0; i < from->symtab_size; i++) {
            const Elf64_Sym *sym = &((const Elf64_Sym *)from->symtab)[i];
            if (sym->st_shndx != SHN_UNDEF && 
                ELF64_ST_TYPE(sym->st_info) == STT_FUNC &&
                strcmp(from->strtab + sym->st_name, name) == 0) {
                return (void *)((uint8_t *)from->base + sym->st_value);
            }
        }
    }

    /* 在全局链表中查找 */
    for (struct dso *dso = g_dynlink.dso_list; dso; dso = dso->next) {
        if (dso == from) continue; /* 已经查过 */
        if (dso->symtab && dso->strtab) {
            for (size_t i = 0; i < dso->symtab_size; i++) {
                const Elf64_Sym *sym = &((const Elf64_Sym *)dso->symtab)[i];
                if (sym->st_shndx != SHN_UNDEF && 
                    (ELF64_ST_TYPE(sym->st_info) == STT_FUNC || 
                     ELF64_ST_TYPE(sym->st_info) == STT_OBJECT) &&
                    strcmp(dso->strtab + sym->st_name, name) == 0) {
                    return (void *)((uint8_t *)dso->base + sym->st_value);
                }
            }
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
    return dynlink_resolve_symbol(symbol, dso);
}

int dlclose(void *handle) {
    if (!handle) {
        dynlink_set_error("Invalid handle");
        return -1;
    }

    struct dso *dso = (struct dso *)handle;

    struct dso **pp = &g_dynlink.dso_list;
    while (*pp) {
        if (*pp == dso) {
            *pp = dso->next;
            break;
        }
        pp = &(*pp)->next;
    }

    if (dso->base) lemis_heap_free(dso->base);
    if (dso->name) lemis_heap_free(dso->name);
    if (dso->path) lemis_heap_free(dso->path);
    if (dso->needed) {
        for (int i = 0; i < dso->needed_count; i++) {
            if (dso->needed[i]) lemis_heap_free(dso->needed[i]);
        }
    }
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
        if (dso->needed) {
            for (int i = 0; i < dso->needed_count; i++) {
                if (dso->needed[i]) lemis_heap_free(dso->needed[i]);
            }
        }
        lemis_heap_free(dso);
        dso = next;
    }
    g_dynlink.dso_list = NULL;
    console_puts("[dynlink] Cleaned up\n");
}