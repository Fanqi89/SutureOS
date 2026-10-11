/* SutureOS: ELF 结构定义 (供动态链接器使用) */
#ifndef STITCH_ELF_H
#define STITCH_ELF_H

#include <stitch/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ELF 魔数 */
#define ELF_MAGIC 0x464C457F  /* "\x7fELF" */
#define ELFMAG0 0x7f
#define ELFMAG1 'E'
#define ELFMAG2 'L'
#define ELFMAG3 'F'

/* ELF 类型 */
#define ELFCLASSNONE 0
#define ELFCLASS32   1
#define ELFCLASS64   2

#define ELFDATANONE 0
#define ELFDATA2LSB  1
#define ELFDATA2MSB  2

#define EV_CURRENT 1

#define ET_NONE   0
#define ET_REL    1
#define ET_EXEC   2
#define ET_DYN    3
#define ET_CORE   4

#define EM_NONE  0
#define EM_386   3
#define EM_X86_64 62

#define EV_NONE 0
#define EV_CURRENT 1

/* Section 类型 */
#define SHT_NULL     0
#define SHT_PROGBITS 1
#define SHT_SYMTAB   2
#define SHT_STRTAB   3
#define SHT_RELA     4
#define SHT_HASH     5
#define SHT_DYNAMIC  6
#define SHT_NOTE     7
#define SHT_NOBITS   8
#define SHT_REL      9
#define SHT_SHLIB    10
#define SHT_DYNSYM   11
#define SHT_INIT_ARRAY 14
#define SHT_FINI_ARRAY 15
#define SHT_PREINIT_ARRAY 16
#define SHT_GROUP    17
#define SHT_SYMTAB_SHNDX 18

/* Section 标志 */
#define SHF_WRITE     0x1
#define SHF_ALLOC     0x2
#define SHF_EXECINSTR 0x4
#define SHF_MERGE     0x10
#define SHF_STRINGS   0x20
#define SHF_INFO_LINK 0x40
#define SHF_LINK_ORDER 0x80
#define SHF_OS_NONCONFORMING 0x100
#define SHF_GROUP     0x200
#define SHF_TLS       0x400
#define SHF_COMPRESSED 0x800
#define SHF_MASKOS    0x0ff00000
#define SHF_MASKPROC  0xf0000000

/* Segment 类型 */
#define PT_NULL    0
#define PT_LOAD    1
#define PT_DYNAMIC 2
#define PT_INTERP  3
#define PT_NOTE    4
#define PT_SHLIB   5
#define PT_PHDR    6
#define PT_TLS     7
#define PT_GNU_EH_FRAME 0x6474e550
#define PT_GNU_STACK    0x6474e551
#define PT_GNU_RELRO    0x6474e552

/* Segment 标志 */
#define PF_X 1
#define PF_W 2
#define PF_R 4

/* Dynamic tag */
#define DT_NULL         0
#define DT_NEEDED       1
#define DT_PLTRELSZ     2
#define DT_PLTGOT       3
#define DT_HASH         4
#define DT_STRTAB       5
#define DT_SYMTAB       6
#define DT_RELA         7
#define DT_RELASZ       8
#define DT_RELAENT      9
#define DT_STRSZ        10
#define DT_SYMENT       11
#define DT_INIT         12
#define DT_FINI         13
#define DT_SONAME       14
#define DT_RPATH        15
#define DT_SYMBOLIC     16
#define DT_REL          17
#define DT_RELSZ        18
#define DT_RELENT       19
#define DT_PLTREL       20
#define DT_DEBUG        21
#define DT_TEXTREL      22
#define DT_JMPREL       23
#define DT_BIND_NOW     24
#define DT_INIT_ARRAY   25
#define DT_FINI_ARRAY   26
#define DT_INIT_ARRAYSZ 27
#define DT_FINI_ARRAYSZ 28
#define DT_RUNPATH      29
#define DT_FLAGS        30
#define DT_ENCODING     32
#define DT_PREINIT_ARRAY 32
#define DT_PREINIT_ARRAYSZ 33
#define DT_SYMTAB_SHNDX 34
#define DT_MAXTAG       34
#define DT_RELACOUNT    0x6ffffff9
#define DT_RELCOUNT     0x6ffffffa

/* DT_FLAGS 标志 */
#define DF_ORIGIN     0x0001
#define DF_SYMBOLIC   0x0002
#define DF_TEXTREL    0x0004
#define DF_BIND_NOW   0x0008
#define DF_STATIC_TLS 0x0010

/* 符号绑定 */
#define STB_LOCAL  0
#define STB_GLOBAL 1
#define STB_WEAK   2
#define STB_NUM    3
#define STB_LOOS   10
#define STB_HIOS   12
#define STB_LOPROC 13
#define STB_HIPROC 15

/* 符号类型 */
#define STT_NOTYPE  0
#define STT_OBJECT  1
#define STT_FUNC    2
#define STT_SECTION 3
#define STT_FILE    4
#define STT_COMMON  5
#define STT_TLS     6
#define STT_NUM     7
#define STT_LOOS    10
#define STT_HIOS    12
#define STT_LOPROC  13
#define STT_HIPROC  15

/* 符号可见性 */
#define STV_DEFAULT   0
#define STV_INTERNAL  1
#define STV_HIDDEN    2
#define STV_PROTECTED 3

/* 重定位类型 (x86_64) */
#define R_X86_64_NONE        0
#define R_X86_64_64          1
#define R_X86_64_PC32        2
#define R_X86_64_GOT32       3
#define R_X86_64_PLT32       4
#define R_X86_64_COPY        5
#define R_X86_64_GLOB_DAT    6
#define R_X86_64_JUMP_SLOT   7
#define R_X86_64_RELATIVE    8
#define R_X86_64_GOTPCREL    9
#define R_X86_64_32          10
#define R_X86_64_32S         11
#define R_X86_64_16          12
#define R_X86_64_PC16        13
#define R_X86_64_8           14
#define R_X86_64_DTPMOD64    15
#define R_X86_64_DTPOFF64    16
#define R_X86_64_TPOFF64     17
#define R_X86_64_TLSGD       18
#define R_X86_64_TLSLD       19
#define R_X86_64_TLSDESC     20
#define R_X86_64_TLSDESC_CALL 21
#define R_X86_64_TLSDESC_REF 22

/* ELF 头 */
typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} Elf64_Ehdr;

/* 程序头 */
typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} Elf64_Phdr;

/* 节头 */
typedef struct {
    uint32_t sh_name;
    uint32_t sh_type;
    uint64_t sh_flags;
    uint64_t sh_addr;
    uint64_t sh_offset;
    uint64_t sh_size;
    uint32_t sh_link;
    uint32_t sh_info;
    uint64_t sh_addralign;
    uint64_t sh_entsize;
} Elf64_Shdr;

/* 符号表项 */
typedef struct {
    uint32_t st_name;
    uint8_t  st_info;
    uint8_t  st_other;
    uint16_t st_shndx;
    uint64_t st_value;
    uint64_t st_size;
} Elf64_Sym;

/* 重定位项 (无加数) */
typedef struct {
    uint64_t r_offset;
    uint64_t r_info;
} Elf64_Rel;

/* 重定位项 (带加数) */
typedef struct {
    uint64_t r_offset;
    uint64_t r_info;
    int64_t  r_addend;
} Elf64_Rela;

/* 动态段项 */
typedef struct {
    int64_t d_tag;
    union {
        uint64_t d_val;
        uint64_t d_ptr;
    } d_un;
} Elf64_Dyn;

/* 宏定义 */
#define ELF64_R_SYM(info)  ((info) >> 32)
#define ELF64_R_TYPE(info) ((info) & 0xffffffff)
#define ELF64_R_INFO(sym, type) (((uint64_t)(sym) << 32) + (uint64_t)(type))

#define ELF64_ST_BIND(info)  ((info) >> 4)
#define ELF64_ST_TYPE(info)  ((info) & 0xf)
#define ELF64_ST_INFO(bind, type) (((bind) << 4) + ((type) & 0xf))

#define ELF64_ST_VISIBILITY(other) ((other) & 0x3)

/* 程序头标志 */
#define PF_R 0x4
#define PF_W 0x2
#define PF_X 0x1

/* 节头标志 */
#define SHF_WRITE        0x1
#define SHF_ALLOC        0x2
#define SHF_EXECINSTR    0x4
#define SHF_MERGE        0x10
#define SHF_STRINGS      0x20
#define SHF_INFO_LINK    0x40
#define SHF_LINK_ORDER   0x80
#define SHF_OS_NONCONFORMING 0x100
#define SHF_GROUP        0x200
#define SHF_TLS          0x400
#define SHF_COMPRESSED   0x800
#define SHF_MASKOS       0x0ff00000
#define SHF_MASKPROC     0xf0000000

/* 特殊节索引 */
#define SHN_UNDEF     0
#define SHN_LORESERVE  0xff00
#define SHN_LOPROC     0xff00
#define SHN_HIPROC     0xff1f
#define SHN_LOOS       0xff20
#define SHN_HIOS       0xff3f
#define SHN_ABS        0xfff1
#define SHN_COMMON     0xfff2
#define SHN_XINDEX     0xffff
#define SHN_HIRESERVE  0xffff

#ifdef __cplusplus
}
#endif

#endif /* STITCH_ELF_H */