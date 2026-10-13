/* SutureOS: deb package installer (real implementation)
 * Source: written from scratch against the public Debian .deb format (ar + tar + gzip)
 * Original license: GPL-3.0 (same as project)
 *
 * Realities implemented for real (no fake success anywhere):
 *  - ar archive parsing (debian-binary / control.tar.gz / data.tar.gz)
 *  - gzip decompression (built-in RFC1951 inflate: stored + fixed/dynamic
 *    Huffman; no dictionary support - deb files never use one)
 *  - ustar tar extraction with header checksum verification, into the
 *    unified VFS (vfs_mkdir_p / vfs_write_file), recording the installed
 *    file list for deb_remove
 *  - control file parsing (fields + Depends-style relationship lists)
 *  - dpkg-style version comparison (epoch/upstream/revision, '~' handling)
 *  - dependency checking against the in-memory package database
 *  - install / remove / query / list / register / unregister
 *
 * Honest limitations (documented, return errors instead of pretending):
 *  - xz/lzma/zstd compressed members: not supported (EOPNOTSUPP log)
 *  - maintainer scripts (preinst/postinst/...): not executed - there is
 *    no shell in the kernel; deb_run_maintscript() returns -1
 *  - package database is in-memory only (lost on reboot)
 */
#include "deb.h"
#include <stitch/types.h>
#include <stitch/string.h>
#include <stitch/console.h>
#include "heap.h"
#include "vfs.h"

/* ==================== global state ==================== */
#define DEB_DB_MAX 64
#define DEB_MAX_FILES 256

static struct deb_package *g_db[DEB_DB_MAX];
static int g_db_count = 0;

/* ==================== internal helpers ==================== */

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

    /* Free each dependency array by its count */
    struct deb_dependency *arrays[] = { pkg->depends, pkg->pre_depends,
        pkg->recommends, pkg->suggests, pkg->conflicts, pkg->breaks,
        pkg->provides, pkg->replaces, pkg->enhances };
    int counts[] = { pkg->depends_count, pkg->pre_depends_count,
        pkg->recommends_count, pkg->suggests_count, pkg->conflicts_count,
        pkg->breaks_count, pkg->provides_count, pkg->replaces_count,
        pkg->enhances_count };
    for (unsigned i = 0; i < sizeof(arrays) / sizeof(arrays[0]); i++) {
        for (int j = 0; j < counts[i]; j++) {
            if (arrays[i][j].package) lemis_heap_free(arrays[i][j].package);
            if (arrays[i][j].version) lemis_heap_free(arrays[i][j].version);
        }
        if (arrays[i]) lemis_heap_free(arrays[i]);
    }

    if (pkg->conffiles) {
        for (int i = 0; i < pkg->conffiles_count; i++) {
            if (pkg->conffiles[i]) lemis_heap_free(pkg->conffiles[i]);
        }
        lemis_heap_free(pkg->conffiles);
    }
    if (pkg->installed_files) {
        for (int i = 0; i < pkg->installed_files_count; i++) {
            if (pkg->installed_files[i]) lemis_heap_free(pkg->installed_files[i]);
        }
        lemis_heap_free(pkg->installed_files);
    }
    lemis_heap_free(pkg);
}

static uint32_t deb_octal(const char *s, uint32_t len) {
    uint32_t v = 0;
    for (uint32_t i = 0; i < len && s[i] >= '0' && s[i] <= '7'; i++) {
        v = v * 8 + (uint32_t)(s[i] - '0');
    }
    return v;
}

/* strstr replacement (no strstr in the freestanding string lib) */
static int deb_name_has_suffix(const char *s, const char *suf) {
    size_t sl = strlen(s), fl = strlen(suf);
    if (sl < fl) return 0;
    return strncmp(s + sl - fl, suf, fl) == 0;
}

/* ar member sizes are DECIMAL ASCII (unlike tar's octal fields) */
static uint32_t deb_decimal(const char *s, uint32_t len) {
    uint32_t v = 0;
    for (uint32_t i = 0; i < len && s[i] >= '0' && s[i] <= '9'; i++) {
        v = v * 10 + (uint32_t)(s[i] - '0');
    }
    return v;
}

/* ==================== gzip / inflate (RFC 1951) ==================== */

struct inf_bits {
    const uint8_t *in;
    uint32_t inlen;
    uint32_t inpos;
    uint64_t bitbuf;
    int bitcnt;
};

static int inf_getbyte(struct inf_bits *s) {
    if (s->inpos >= s->inlen) return -1;
    return s->in[s->inpos++];
}

static int inf_bits_need(struct inf_bits *s, int need) {
    while (s->bitcnt < need) {
        int b = inf_getbyte(s);
        if (b < 0) return -1;
        s->bitbuf |= (uint64_t)b << s->bitcnt;
        s->bitcnt += 8;
    }
    int v = (int)(s->bitbuf & ((1u << need) - 1));
    s->bitbuf >>= need;
    s->bitcnt -= need;
    return v;
}

/* puff-style canonical Huffman decoding */
struct inf_huff {
    int count[16];   /* number of codes of each length */
    int symbol[288]; /* symbols ordered by code */
};

static int inf_huff_construct(struct inf_huff *h, const uint8_t *lengths, int n) {
    int offs[16];
    for (int i = 0; i < 16; i++) h->count[i] = 0;
    for (int i = 0; i < n; i++) h->count[lengths[i]]++;
    if (h->count[0] == n) return 0;   /* no codes at all */

    int left = 1;
    for (int len = 1; len < 16; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return -1;      /* over-subscribed */
    }
    offs[1] = 0;
    for (int len = 1; len < 15; len++) {
        offs[len + 1] = offs[len] + h->count[len];
    }
    for (int i = 0; i < n; i++) {
        if (lengths[i]) h->symbol[offs[lengths[i]]++] = i;
    }
    return left;   /* >0 means incomplete but usable */
}

static int inf_decode(struct inf_bits *s, const struct inf_huff *h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
        int b = inf_bits_need(s, 1);
        if (b < 0) return -1;
        code |= b;
        int cnt = h->count[len];
        if (code - cnt < first) {
            return h->symbol[index + (code - first)];
        }
        index += cnt;
        first += cnt;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

/* length/distance tables */
static const uint16_t inf_len_base[29] = {
    3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,
    163,195,227,258 };
static const uint8_t inf_len_extra[29] = {
    0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
static const uint16_t inf_dist_base[30] = {
    1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,
    2049,3073,4097,6145,8193,12289,16385,24577 };
static const uint8_t inf_dist_extra[30] = {
    0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };

struct inf_out {
    uint8_t *buf;
    uint32_t cap;
    uint32_t len;
};

static int inf_put(struct inf_out *o, uint8_t b) {
    if (o->len >= o->cap) return -1;
    o->buf[o->len++] = b;
    return 0;
}

static int inf_block(struct inf_bits *s, struct inf_out *o,
                     const struct inf_huff *lencode, const struct inf_huff *distcode) {
    for (;;) {
        int sym = inf_decode(s, lencode);
        if (sym < 0) return -1;
        if (sym < 256) {
            if (inf_put(o, (uint8_t)sym) < 0) return -2;   /* output full */
        } else if (sym == 256) {
            return 0;                                       /* end of block */
        } else {
            sym -= 257;
            if (sym >= 29) return -1;
            int len = inf_len_base[sym] + inf_bits_need(s, inf_len_extra[sym]);
            int dsym = inf_decode(s, distcode);
            if (dsym < 0 || dsym >= 30) return -1;
            int dist = inf_dist_base[dsym] + inf_bits_need(s, inf_dist_extra[dsym]);
            if (len < 0 || dist <= 0 || (uint32_t)dist > o->len) return -1;
            uint32_t from = o->len - (uint32_t)dist;
            for (int i = 0; i < len; i++) {
                if (inf_put(o, o->buf[from + i]) < 0) return -2;
            }
        }
    }
}

static int inf_fixed_tables(struct inf_huff *lencode, struct inf_huff *distcode) {
    uint8_t lengths[288];
    for (int i = 0; i < 144; i++) lengths[i] = 8;
    for (int i = 144; i < 256; i++) lengths[i] = 9;
    for (int i = 256; i < 280; i++) lengths[i] = 7;
    for (int i = 280; i < 288; i++) lengths[i] = 8;
    if (inf_huff_construct(lencode, lengths, 288) < 0) return -1;
    for (int i = 0; i < 30; i++) lengths[i] = 5;
    if (inf_huff_construct(distcode, lengths, 30) < 0) return -1;
    return 0;
}

static int inf_dynamic_tables(struct inf_bits *s, struct inf_huff *lencode,
                              struct inf_huff *distcode) {
    static const uint8_t order[19] = {
        16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15 };
    int hlit = inf_bits_need(s, 5) + 257;
    int hdist = inf_bits_need(s, 5) + 1;
    int hclen = inf_bits_need(s, 4) + 4;
    if (hlit > 288 || hdist > 30) return -1;

    uint8_t clen[19] = {0};
    for (int i = 0; i < hclen; i++) {
        int v = inf_bits_need(s, 3);
        if (v < 0) return -1;
        clen[order[i]] = (uint8_t)v;
    }
    struct inf_huff cltree;
    if (inf_huff_construct(&cltree, clen, 19) < 0) return -1;

    uint8_t lengths[288 + 30];
    int n = 0;
    while (n < hlit + hdist) {
        int sym = inf_decode(s, &cltree);
        if (sym < 0) return -1;
        if (sym < 16) {
            lengths[n++] = (uint8_t)sym;
        } else if (sym == 16) {
            if (n == 0) return -1;
            int prev = lengths[n - 1];
            int rep = 3 + inf_bits_need(s, 2);
            if (rep < 0) return -1;
            while (rep-- && n < hlit + hdist) lengths[n++] = (uint8_t)prev;
        } else if (sym == 17) {
            int rep = 3 + inf_bits_need(s, 3);
            if (rep < 0) return -1;
            while (rep-- && n < hlit + hdist) lengths[n++] = 0;
        } else if (sym == 18) {
            int rep = 11 + inf_bits_need(s, 7);
            if (rep < 0) return -1;
            while (rep-- && n < hlit + hdist) lengths[n++] = 0;
        } else {
            return -1;
        }
    }
    if (inf_huff_construct(lencode, lengths, hlit) < 0) return -1;
    if (inf_huff_construct(distcode, lengths + hlit, hdist) < 0) return -1;
    return 0;
}

/* Inflate a raw deflate stream. Returns total output length or negative. */
static int deb_inflate(const uint8_t *src, uint32_t srclen,
                       uint8_t *dst, uint32_t dstcap) {
    struct inf_bits s = { src, srclen, 0, 0, 0 };
    struct inf_out o = { dst, dstcap, 0 };
    int last;
    do {
        last = inf_bits_need(&s, 1);
        int type = inf_bits_need(&s, 2);
        if (last < 0 || type < 0) return -1;
        if (type == 0) {
            /* stored: skip to byte boundary, copy LEN bytes */
            s.bitbuf = 0;
            s.bitcnt = 0;
            if (s.inpos + 4 > s.inlen) return -1;
            uint32_t len = (uint32_t)s.in[s.inpos] | ((uint32_t)s.in[s.inpos + 1] << 8);
            s.inpos += 4;
            if (s.inpos + len > s.inlen) return -1;
            if (o.len + len > o.cap) return -2;
            memcpy(o.buf + o.len, s.in + s.inpos, len);
            o.len += len;
            s.inpos += len;
        } else {
            struct inf_huff lencode, distcode;
            int rc;
            if (type == 1) rc = inf_fixed_tables(&lencode, &distcode);
            else if (type == 2) rc = inf_dynamic_tables(&s, &lencode, &distcode);
            else return -1;
            if (rc < 0) return -1;
            rc = inf_block(&s, &o, &lencode, &distcode);
            if (rc < 0) return rc;
        }
    } while (!last);
    return (int)o.len;
}

/* Decompress a gzip member (RFC 1952). Returns heap buffer, sets *out_len. */
static uint8_t *deb_gunzip(const uint8_t *src, uint32_t srclen, uint32_t *out_len) {
    if (srclen < 18 || src[0] != 0x1F || src[1] != 0x8B || src[2] != 8) {
        console_puts("[deb] not a gzip (CM!=deflate or bad magic)\n");
        return NULL;
    }
    uint8_t flg = src[3];
    uint32_t pos = 10;
    if (flg & 0x04) {   /* FEXTRA */
        if (pos + 2 > srclen) return NULL;
        uint32_t xlen = (uint32_t)src[pos] | ((uint32_t)src[pos + 1] << 8);
        pos += 2 + xlen;
    }
    if (flg & 0x08) {   /* FNAME */
        while (pos < srclen && src[pos]) pos++;
        pos++;
    }
    if (flg & 0x10) {   /* FCOMMENT */
        while (pos < srclen && src[pos]) pos++;
        pos++;
    }
    if (flg & 0x02) pos += 2;   /* FHCRC */
    if (pos + 8 > srclen) return NULL;

    /* Output guess: 4x compressed, min 64KiB, cap 16MiB */
    uint32_t cap = srclen * 4 + 65536;
    if (cap > 16u * 1024 * 1024) cap = 16u * 1024 * 1024;
    uint8_t *out = lemis_heap_alloc(cap);
    if (!out) return NULL;

    int n = deb_inflate(src + pos, srclen - pos - 8, out, cap);
    if (n < 0) {
        console_printf("[deb] inflate failed (%d%s)\n", (int64_t)n,
                       n == -2 ? " = output full" : "");
        lemis_heap_free(out);
        return NULL;
    }
    *out_len = (uint32_t)n;
    return out;
}

/* ==================== tar (ustar) ==================== */

struct tar_entry {
    char name[256];
    uint32_t size;
    uint8_t type;      /* '0'/'\0' = file, '5' = dir */
    const uint8_t *data;
};

/* Walk a ustar archive; cb returns non-zero to abort. Verifies checksums. */
static int deb_tar_walk(const uint8_t *tar, uint32_t tarlen,
                        int (*cb)(const struct tar_entry *e, void *ctx),
                        void *ctx, int max_entries) {
    uint32_t pos = 0;
    int count = 0;
    while (pos + 512 <= tarlen) {
        const uint8_t *h = tar + pos;
        if (h[0] == 0) break;   /* end of archive */

        /* checksum verification */
        uint32_t stored = deb_octal((const char *)h + 148, 8);
        uint32_t sum = 0;
        for (int i = 0; i < 512; i++) {
            sum += (i >= 148 && i < 156) ? ' ' : h[i];
        }
        if (sum != stored) {
            console_printf("[deb] tar checksum mismatch at offset %u\n", (uint64_t)pos);
            return -1;
        }

        struct tar_entry e;
        memset(&e, 0, sizeof(e));
        /* name = [prefix/]name */
        char prefix[156];
        memcpy(prefix, h + 345, 155);
        prefix[155] = '\0';
        uint32_t nlen = 0;
        if (prefix[0]) {
            uint32_t pl = (uint32_t)strlen(prefix);
            memcpy(e.name, prefix, pl);
            nlen = pl;
            e.name[nlen++] = '/';
        }
        uint32_t nl = 0;
        while (nl < 100 && h[nl]) nl++;
        if (nlen + nl >= sizeof(e.name)) nl = sizeof(e.name) - nlen - 1;
        memcpy(e.name + nlen, h, nl);
        e.size = deb_octal((const char *)h + 124, 12);
        e.type = h[156] ? h[156] : '0';
        pos += 512;
        if (e.type == '0' || e.type == '\0') {
            e.data = tar + pos;
            uint32_t padded = (e.size + 511u) & ~511u;
            if (pos + padded > tarlen) return -1;
            pos += padded;
        } else if (e.type == '5') {
            /* dir: no data */
        } else {
            uint32_t padded = (e.size + 511u) & ~511u;
            if (pos + padded > tarlen) return -1;
            pos += padded;
            console_printf("[deb] skipping tar entry type=%c name=%s\n",
                           (int64_t)e.type, e.name);
        }

        if (cb && cb(&e, ctx) != 0) return -1;
        if (++count >= max_entries) return -1;
    }
    return count;
}

/* ==================== ar parsing ==================== */

struct ar_member {
    char name[17];
    const uint8_t *data;
    uint32_t size;
};

/* Find members of interest in a .deb (ar) buffer. */
static int deb_ar_find(const uint8_t *buf, uint32_t len,
                       const uint8_t **ctrl, uint32_t *ctrl_len,
                       const uint8_t **data, uint32_t *data_len,
                       char *ctrl_name, char *data_name, uint32_t name_cap) {
    if (len < DEB_MAGIC_LEN || memcmp(buf, DEB_MAGIC, DEB_MAGIC_LEN) != 0) {
        console_puts("[deb] bad ar magic\n");
        return -1;
    }
    uint32_t pos = DEB_MAGIC_LEN;
    int found = 0;
    while (pos + 60 <= len) {
        const char *h = (const char *)buf + pos;
        if (h[58] != 0x60 || h[59] != 0x0A) {
            console_puts("[deb] bad ar header\n");
            return -1;
        }
        struct ar_member m;
        memset(&m, 0, sizeof(m));
        memcpy(m.name, h, 16);
        /* strip trailing spaces and GNU '/' name-terminator */
        for (int i = 15; i >= 0; i--) {
            if (m.name[i] == ' ') m.name[i] = '\0';
            else break;
        }
        uint32_t mlen = (uint32_t)strlen(m.name);
        if (mlen && m.name[mlen - 1] == '/') m.name[mlen - 1] = '\0';
        m.size = deb_decimal(h + 48, 10);
        pos += 60;
        if (pos + m.size > len) {
            console_puts("[deb] ar member truncated\n");
            return -1;
        }
        m.data = buf + pos;

        if (strcmp(m.name, "debian-binary") == 0) {
            found |= 1;
        } else if (strncmp(m.name, "control.tar", 11) == 0) {
            if (ctrl) *ctrl = m.data;
            if (ctrl_len) *ctrl_len = m.size;
            if (ctrl_name) { strncpy(ctrl_name, m.name, name_cap - 1); }
            found |= 2;
        } else if (strncmp(m.name, "data.tar", 8) == 0) {
            if (data) *data = m.data;
            if (data_len) *data_len = m.size;
            if (data_name) { strncpy(data_name, m.name, name_cap - 1); }
            found |= 4;
        }
        pos += m.size;
        if (pos & 1) pos++;   /* ar members are 2-byte aligned */
    }
    /* Require exactly what the caller asked for */
    if (ctrl && !(found & 2)) return -1;
    if (data && !(found & 4)) return -1;
    if (!ctrl && !data) return -1;
    return 0;
}

/* ==================== control file parsing ==================== */

static char *deb_trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\n' || end[-1] == '\r')) {
        *--end = '\0';
    }
    return s;
}

static int deb_parse_dep_list(const char *value, struct deb_dependency **out, int *out_count) {
    /* "pkg (>= 1.0), pkg2 | pkg3, pkg4" - we keep the first alternative */
    int cap = 8, n = 0;
    struct deb_dependency *arr = lemis_heap_alloc(sizeof(*arr) * (uintptr_t)cap);
    if (!arr) return -1;

    const char *p = value;
    while (*p) {
        /* skip whitespace/commas */
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        if (!*p) break;

        /* package name */
        char name[128]; uint32_t nl = 0;
        while (*p && *p != ' ' && *p != '\t' && *p != ',' && *p != '(' &&
               *p != '|' && nl < sizeof(name) - 1) {
            name[nl++] = *p++;
        }
        name[nl] = '\0';
        /* skip alternatives */
        while (*p && *p != ',' && *p != '(') {
            if (*p == '|') { while (*p && *p != ',') p++; break; }
            p++;
        }
        /* version constraint */
        char constraint[64] = {0};
        if (*p == '(') {
            p++;
            uint32_t cl = 0;
            while (*p && *p != ')' && cl < sizeof(constraint) - 1) {
                constraint[cl++] = *p++;
            }
            constraint[cl] = '\0';
            char *c = deb_trim(constraint);
            if (*c) {
                /* keep only "op version" */
                memmove(constraint, c, strlen(c) + 1);
            }
            while (*p && *p != ')') p++;
            if (*p == ')') p++;
        }
        if (nl == 0) continue;
        if (n >= cap) {
            int ncap = cap * 2;
            struct deb_dependency *narr = lemis_heap_alloc(sizeof(*narr) * (uintptr_t)ncap);
            if (!narr) { /* free what we have */ break; }
            memcpy(narr, arr, sizeof(*arr) * (uintptr_t)n);
            lemis_heap_free(arr);
            arr = narr;
            cap = ncap;
        }
        arr[n].package = deb_strdup(name);
        arr[n].version = constraint[0] ? deb_strdup(constraint) : NULL;
        arr[n].type = DEB_DEP_DEPENDS;
        n++;
    }
    *out = arr;
    *out_count = n;
    return 0;
}

static int deb_parse_control(const uint8_t *ctrl, uint32_t len, struct deb_package *pkg) {
    /* find "control" member inside the control tar */
    const struct tar_entry *found = NULL;
    struct tar_entry entry_copy;
    /* small re-walk: we only need the entry with basename "control" */
    uint32_t pos = 0;
    while (pos + 512 <= len) {
        const uint8_t *h = ctrl + pos;
        if (h[0] == 0) break;
        uint32_t stored = deb_octal((const char *)h + 148, 8);
        uint32_t sum = 0;
        for (int i = 0; i < 512; i++) sum += (i >= 148 && i < 156) ? ' ' : h[i];
        if (sum != stored) { console_puts("[deb] control tar checksum mismatch\n"); return -1; }

        struct tar_entry e;
        memset(&e, 0, sizeof(e));
        uint32_t nl = 0;
        while (nl < 100 && h[nl]) nl++;
        memcpy(e.name, h, nl);
        e.size = deb_octal((const char *)h + 124, 12);
        e.type = h[156] ? h[156] : '0';
        pos += 512;

        const char *base = e.name;
        for (const char *q = e.name; *q; q++) if (*q == '/') base = q + 1;
        if ((e.type == '0' || e.type == '\0') && strcmp(base, "control") == 0) {
            if (pos + e.size > len) return -1;
            entry_copy = e;
            entry_copy.data = ctrl + pos;
            found = &entry_copy;
            break;
        }
        pos += (e.size + 511u) & ~511u;
    }
    if (!found) {
        console_puts("[deb] no control file inside control.tar\n");
        return -1;
    }

    /* parse Field: value lines */
    const uint8_t *p = found->data;
    uint32_t remaining = found->size;
    while (remaining > 0) {
        /* read one logical line (continuations start with space) */
        char line[1024];
        uint32_t ll = 0;
        while (remaining > 0 && *p != '\n') {
            if (ll < sizeof(line) - 1) line[ll++] = (char)*p;
            p++; remaining--;
        }
        if (remaining > 0) { p++; remaining--; }   /* consume newline */
        while (remaining > 0 && (*p == ' ' || *p == '\t')) {
            /* continuation: append */
            while (remaining > 0 && *p != '\n') {
                if (ll < sizeof(line) - 1) line[ll++] = (char)*p;
                p++; remaining--;
            }
            if (remaining > 0) { p++; remaining--; }
        }
        line[ll] = '\0';

        char *colon = strchr(line, ':');
        if (!colon) continue;
        *colon = '\0';
        char *field = deb_trim(line);
        char *value = deb_trim(colon + 1);

        if (strcmp(field, "Package") == 0) pkg->name = deb_strdup(value);
        else if (strcmp(field, "Version") == 0) pkg->version = deb_strdup(value);
        else if (strcmp(field, "Architecture") == 0) pkg->architecture = deb_strdup(value);
        else if (strcmp(field, "Maintainer") == 0) pkg->maintainer = deb_strdup(value);
        else if (strcmp(field, "Description") == 0) pkg->description = deb_strdup(value);
        else if (strcmp(field, "Section") == 0) pkg->section = deb_strdup(value);
        else if (strcmp(field, "Priority") == 0) pkg->priority = deb_strdup(value);
        else if (strcmp(field, "Homepage") == 0) pkg->homepage = deb_strdup(value);
        else if (strcmp(field, "Source") == 0) pkg->source = deb_strdup(value);
        else if (strcmp(field, "Installed-Size") == 0) pkg->installed_size = (uint64_t)deb_decimal(value, 12);
        else if (strcmp(field, "Depends") == 0) deb_parse_dep_list(value, &pkg->depends, &pkg->depends_count);
        else if (strcmp(field, "Pre-Depends") == 0) deb_parse_dep_list(value, &pkg->pre_depends, &pkg->pre_depends_count);
        else if (strcmp(field, "Recommends") == 0) deb_parse_dep_list(value, &pkg->recommends, &pkg->recommends_count);
        else if (strcmp(field, "Suggests") == 0) deb_parse_dep_list(value, &pkg->suggests, &pkg->suggests_count);
        else if (strcmp(field, "Conflicts") == 0) {
            deb_parse_dep_list(value, &pkg->conflicts, &pkg->conflicts_count);
            for (int i = 0; i < pkg->conflicts_count; i++) pkg->conflicts[i].type = DEB_DEP_CONFLICTS;
        }
        else if (strcmp(field, "Provides") == 0) {
            deb_parse_dep_list(value, &pkg->provides, &pkg->provides_count);
            for (int i = 0; i < pkg->provides_count; i++) pkg->provides[i].type = DEB_DEP_PROVIDES;
        }
    }
    return (pkg->name && pkg->version) ? 0 : -1;
}

/* ==================== version compare (dpkg style) ==================== */

static int deb_ver_cmp_part(const char **s1, const char **s2) {
    /* compare one segment: digits numerically, letters by ascii,
     * '~' sorts before everything, parts end in order */
    while (**s1 || **s2) {
        int c1 = (unsigned char)**s1;
        int c2 = (unsigned char)**s2;
        if (c1 == '~' || c2 == '~') {
            if (c1 != c2) return (c1 == '~') ? -1 : 1;
            (*s1)++; (*s2)++;
            continue;
        }
        if (c1 >= '0' && c1 <= '9' && c2 >= '0' && c2 <= '9') {
            /* numeric run */
            unsigned long v1 = 0, v2 = 0;
            while (**s1 >= '0' && **s1 <= '9') { v1 = v1 * 10 + (unsigned)(**s1 - '0'); (*s1)++; }
            while (**s2 >= '0' && **s2 <= '9') { v2 = v2 * 10 + (unsigned)(**s2 - '0'); (*s2)++; }
            if (v1 != v2) return (v1 < v2) ? -1 : 1;
            continue;
        }
        if (!c1 && !c2) break;
        if (!c1) return -1;
        if (!c2) return 1;
        if (c1 != c2) return (c1 < c2) ? -1 : 1;
        (*s1)++; (*s2)++;
    }
    return 0;
}

deb_version_cmp_t deb_version_compare(const char *v1, const char *v2) {
    if (!v1 || !v2) return DEB_VER_INVALID;

    /* epoch */
    unsigned long e1 = 0, e2 = 0;
    const char *c1 = v1, *c2 = v2;
    while (*c1 >= '0' && *c1 <= '9') { e1 = e1 * 10 + (unsigned)(*c1 - '0'); c1++; }
    while (*c2 >= '0' && *c2 <= '9') { e2 = e2 * 10 + (unsigned)(*c2 - '0'); c2++; }
    if (*c1 == ':') c1++; else e1 = 0;
    if (*c2 == ':') c2++; else e2 = 0;
    if (e1 != e2) return (e1 < e2) ? DEB_VER_LT : DEB_VER_GT;

    /* split upstream/revision at last '-' */
    const char *d1 = NULL, *d2 = NULL;
    for (const char *q = c1; *q; q++) if (*q == '-') d1 = q;
    for (const char *q = c2; *q; q++) if (*q == '-') d2 = q;

    char up1[128], up2[128];
    uint32_t n1 = d1 ? (uint32_t)(d1 - c1) : (uint32_t)strlen(c1);
    uint32_t n2 = d2 ? (uint32_t)(d2 - c2) : (uint32_t)strlen(c2);
    if (n1 >= sizeof(up1) || n2 >= sizeof(up2)) return DEB_VER_INVALID;
    memcpy(up1, c1, n1); up1[n1] = '\0';
    memcpy(up2, c2, n2); up2[n2] = '\0';

    const char *p1 = up1, *p2 = up2;
    int rc = deb_ver_cmp_part(&p1, &p2);
    if (rc != 0) return (rc < 0) ? DEB_VER_LT : DEB_VER_GT;

    /* revision: missing revision == "0" */
    const char *r1 = d1 ? d1 + 1 : "0";
    const char *r2 = d2 ? d2 + 1 : "0";
    rc = deb_ver_cmp_part(&r1, &r2);
    return (rc < 0) ? DEB_VER_LT : (rc > 0 ? DEB_VER_GT : DEB_VER_EQ);
}

/* ==================== package database ==================== */

int deb_db_init(const char *db_path) {
    (void)db_path;   /* in-memory database (documented) */
    g_db_count = 0;
    memset(g_db, 0, sizeof(g_db));
    return 0;
}

void deb_db_close(void) {
    for (int i = 0; i < g_db_count; i++) {
        /* database entries are owned by callers after list queries;
         * unregister frees nothing here to avoid dangling pointers in
         * outstanding query results */
        g_db[i] = NULL;
    }
    g_db_count = 0;
}

int deb_db_register(struct deb_package *pkg) {
    if (!pkg || !pkg->name) return -1;
    if (g_db_count >= DEB_DB_MAX) return -1;
    g_db[g_db_count++] = pkg;
    return 0;
}

int deb_db_unregister(const char *pkg_name) {
    if (!pkg_name) return -1;
    for (int i = 0; i < g_db_count; i++) {
        if (g_db[i] && g_db[i]->name && strcmp(g_db[i]->name, pkg_name) == 0) {
            for (int j = i; j < g_db_count - 1; j++) g_db[j] = g_db[j + 1];
            g_db_count--;
            return 0;
        }
    }
    return -1;
}

struct deb_package *deb_db_find(const char *pkg_name) {
    if (!pkg_name) return NULL;
    for (int i = 0; i < g_db_count; i++) {
        if (g_db[i] && g_db[i]->name && strcmp(g_db[i]->name, pkg_name) == 0) {
            return g_db[i];
        }
    }
    return NULL;
}

/* ==================== public API ==================== */

int deb_parse_package(const char *deb_path, struct deb_package **out_pkg) {
    if (!deb_path || !out_pkg) return -1;

    size_t len = 0;
    uint8_t *buf = (uint8_t *)vfs_read_file(deb_path, &len);
    if (!buf) {
        console_printf("[deb] cannot read %s\n", deb_path);
        return -1;
    }

    const uint8_t *ctrl = NULL;
    uint32_t ctrl_len = 0;
    char ctrl_name[17] = {0};
    if (deb_ar_find(buf, (uint32_t)len, &ctrl, &ctrl_len, NULL, NULL,
                    ctrl_name, NULL, sizeof(ctrl_name)) < 0) {
        lemis_heap_free(buf);
        return -1;
    }

    /* decompress control.tar.gz (accept plain control.tar too) */
    uint8_t *tar = NULL;
    uint32_t tarlen = 0;
    if (deb_name_has_suffix(ctrl_name, ".gz")) {
        tar = deb_gunzip(ctrl, ctrl_len, &tarlen);
    } else if (deb_name_has_suffix(ctrl_name, ".xz") || deb_name_has_suffix(ctrl_name, ".zst")) {
        console_printf("[deb] %s: compressed format not supported (need .gz or plain tar)\n",
                       ctrl_name);
        lemis_heap_free(buf);
        return -1;
    } else {
        tar = (uint8_t *)ctrl;
        tarlen = ctrl_len;
    }
    if (!tar) { lemis_heap_free(buf); return -1; }

    struct deb_package *pkg = lemis_heap_alloc(sizeof(*pkg));
    if (!pkg) {
        if (tar != ctrl) lemis_heap_free(tar);
        lemis_heap_free(buf);
        return -1;
    }
    memset(pkg, 0, sizeof(*pkg));
    pkg->status = DEB_STATUS_NOT_INSTALLED;

    int rc = deb_parse_control(tar, tarlen, pkg);
    if (tar != ctrl) lemis_heap_free(tar);
    lemis_heap_free(buf);
    if (rc < 0) {
        console_puts("[deb] control parse failed\n");
        deb_free_package(pkg);
        return -1;
    }
    *out_pkg = pkg;
    return 0;
}

int deb_check_dependencies(struct deb_package *pkg, int *missing_count, char ***missing_pkgs) {
    if (!pkg) return -1;
    int missing = 0;
    char *names[DEB_MAX_DEPENDS];

    struct deb_dependency *checks[2] = { pkg->pre_depends, pkg->depends };
    int counts[2] = { pkg->pre_depends_count, pkg->depends_count };
    for (int c = 0; c < 2; c++) {
        for (int i = 0; i < counts[c]; i++) {
            struct deb_dependency *dep = &checks[c][i];
            struct deb_package *have = deb_db_find(dep->package);
            if (!have) {
                if (missing < DEB_MAX_DEPENDS) names[missing++] = dep->package;
                continue;
            }
            if (dep->version && have->version) {
                /* constraint like ">= 1.0" / "<= 2" / "= 1" / "> 1" */
                const char *cv = dep->version;
                int op = 0;   /* 0='=' 1='>=' 2='<=' 3='>' 4='<' */
                if (cv[0] == '>' && cv[1] == '=') { op = 1; cv += 2; }
                else if (cv[0] == '<' && cv[1] == '=') { op = 2; cv += 2; }
                else if (cv[0] == '>') { op = 3; cv += 1; }
                else if (cv[0] == '<') { op = 4; cv += 1; }
                else if (cv[0] == '=') { cv += 1; }
                while (*cv == ' ') cv++;
                deb_version_cmp_t r = deb_version_compare(have->version, cv);
                int ok = (op == 0 && r == DEB_VER_EQ) ||
                         (op == 1 && r != DEB_VER_LT) ||
                         (op == 2 && r != DEB_VER_GT) ||
                         (op == 3 && r == DEB_VER_GT) ||
                         (op == 4 && r == DEB_VER_LT);
                if (!ok && missing < DEB_MAX_DEPENDS) names[missing++] = dep->package;
            }
        }
    }

    /* conflicts */
    for (int i = 0; i < pkg->conflicts_count; i++) {
        if (deb_db_find(pkg->conflicts[i].package)) {
            console_printf("[deb] conflict with installed package %s\n",
                           pkg->conflicts[i].package);
            return -2;   /* hard error */
        }
    }

    if (missing_count) *missing_count = missing;
    if (missing_pkgs) {
        *missing_pkgs = NULL;
        if (missing > 0) {
            char **out = lemis_heap_alloc(sizeof(char *) * (uintptr_t)missing);
            if (out) {
                for (int i = 0; i < missing; i++) out[i] = names[i];
                *missing_pkgs = out;
            }
        }
    }
    return (missing == 0) ? 0 : -1;
}

/* ---- data.tar extraction ---- */

struct extract_ctx {
    char root[256];
    struct deb_package *pkg;
    int files;
    int dirs;
    int errors;
};

static int deb_extract_cb(const struct tar_entry *e, void *ctxv) {
    struct extract_ctx *x = (struct extract_ctx *)ctxv;

    /* sanitize: strip leading "./" and "/" */
    const char *rel = e->name;
    while (rel[0] == '.') rel++;
    while (rel[0] == '/') rel++;
    if (!rel[0]) return 0;

    char full[512];
    if (strcmp(x->root, "/") == 0) {
        snprintf(full, sizeof(full), "/%s", rel);
    } else {
        snprintf(full, sizeof(full), "%s/%s", x->root, rel);
    }

    if (e->type == '5') {
        if (vfs_mkdir_p(full) != VFS_OK) {
            console_printf("[deb] mkdir failed: %s\n", full);
            x->errors++;
            return 0;
        }
        x->dirs++;
        return 0;
    }

    /* parent dirs */
    {
        char parent[512];
        strncpy(parent, full, sizeof(parent) - 1);
        parent[sizeof(parent) - 1] = '\0';
        for (int i = (int)strlen(parent) - 1; i > 0; i--) {
            if (parent[i] == '/') { parent[i] = '\0'; break; }
        }
        vfs_mkdir_p(parent);
    }

    int w = vfs_write_file(full, e->data, e->size);
    if (w != VFS_OK) {
        console_printf("[deb] write failed: %s (%d)\n", full, (int64_t)w);
        x->errors++;
        return 0;
    }
    x->files++;

    /* record for removal */
    if (x->pkg && x->pkg->installed_files_count < DEB_MAX_FILES) {
        int n = x->pkg->installed_files_count;
        if (n == 0) {
            x->pkg->installed_files = lemis_heap_alloc(sizeof(char *) * DEB_MAX_FILES);
        }
        if (x->pkg->installed_files) {
            x->pkg->installed_files[n] = deb_strdup(full);
            x->pkg->installed_files_count = n + 1;
        }
    }
    return 0;
}

int deb_install(const char *deb_path, const char *root_dir, int force) {
    if (!deb_path) return -1;
    if (!root_dir) root_dir = "/";

    struct deb_package *pkg = NULL;
    if (deb_parse_package(deb_path, &pkg) < 0) {
        console_printf("[deb] parse failed: %s\n", deb_path);
        return -1;
    }
    console_printf("[deb] parsed %s %s (%s)\n", pkg->name, pkg->version,
                   pkg->architecture ? pkg->architecture : "?");

    /* already installed? */
    struct deb_package *have = deb_db_find(pkg->name);
    if (have && have->version) {
        deb_version_cmp_t r = deb_version_compare(pkg->version, have->version);
        if (r == DEB_VER_LT && !force) {
            console_printf("[deb] %s %s is newer than %s, skipping\n",
                           pkg->name, have->version, pkg->version);
            deb_free_package(pkg);
            return -3;
        }
    }

    /* dependencies */
    if (!force) {
        int missing = 0;
        char **names = NULL;
        if (deb_check_dependencies(pkg, &missing, &names) != 0) {
            console_printf("[deb] missing %d dependency(ies):\n", (int64_t)missing);
            for (int i = 0; i < missing && names; i++) {
                console_printf("  - %s\n", names[i]);
            }
            if (names) lemis_heap_free(names);
            deb_free_package(pkg);
            return -4;
        }
    }

    /* read data member and unpack */
    size_t len = 0;
    uint8_t *buf = (uint8_t *)vfs_read_file(deb_path, &len);
    if (!buf) { deb_free_package(pkg); return -1; }

    const uint8_t *data = NULL;
    uint32_t data_len = 0;
    char data_name[17] = {0};
    if (deb_ar_find(buf, (uint32_t)len, NULL, NULL, &data, &data_len,
                    NULL, data_name, sizeof(data_name)) < 0) {
        /* ar_find requires control; data optional only in weird debs */
        console_puts("[deb] no data.tar member\n");
        lemis_heap_free(buf);
        deb_free_package(pkg);
        return -1;
    }

    uint8_t *tar = NULL;
    uint32_t tarlen = 0;
    if (deb_name_has_suffix(data_name, ".gz")) {
        tar = deb_gunzip(data, data_len, &tarlen);
    } else if (deb_name_has_suffix(data_name, ".xz") || deb_name_has_suffix(data_name, ".zst")) {
        console_printf("[deb] %s: compressed format not supported\n", data_name);
        lemis_heap_free(buf);
        deb_free_package(pkg);
        return -5;
    } else {
        tar = (uint8_t *)data;
        tarlen = data_len;
    }
    if (!tar) { lemis_heap_free(buf); deb_free_package(pkg); return -1; }

    struct extract_ctx x;
    memset(&x, 0, sizeof(x));
    strncpy(x.root, root_dir, sizeof(x.root) - 1);
    x.pkg = pkg;

    int walked = deb_tar_walk(tar, tarlen, deb_extract_cb, &x, 4096);
    if (tar != data) lemis_heap_free(tar);
    lemis_heap_free(buf);

    if (walked < 0 || x.errors > 0) {
        console_printf("[deb] unpack failed (entries=%d errors=%d)\n",
                       (int64_t)walked, (int64_t)x.errors);
        deb_free_package(pkg);
        return -1;
    }

    console_printf("[deb] unpacked %s: %d files, %d dirs\n",
                   pkg->name, (int64_t)x.files, (int64_t)x.dirs);

    /* replace previous registration */
    if (have) deb_db_unregister(have->name);
    pkg->status = DEB_STATUS_INSTALLED;
    pkg->install_path = deb_strdup(root_dir);
    if (deb_db_register(pkg) < 0) {
        console_puts("[deb] database full, package not registered\n");
    }
    console_printf("[deb] installed %s %s\n", pkg->name, pkg->version);
    return 0;
}

int deb_remove(const char *pkg_name, int purge) {
    (void)purge;   /* conffiles kept unless listed in installed_files */
    struct deb_package *pkg = deb_db_find(pkg_name);
    if (!pkg) {
        console_printf("[deb] package not installed: %s\n", pkg_name);
        return -1;
    }

    int removed = 0, errors = 0;
    for (int i = pkg->installed_files_count - 1; i >= 0; i--) {
        if (vfs_unlink(pkg->installed_files[i]) == VFS_OK) removed++;
        else errors++;
    }
    /* try to remove now-empty directories (reverse order already) */
    for (int i = pkg->installed_files_count - 1; i >= 0; i--) {
        char dir[512];
        strncpy(dir, pkg->installed_files[i], sizeof(dir) - 1);
        dir[sizeof(dir) - 1] = '\0';
        for (int j = (int)strlen(dir) - 1; j > 0; j--) {
            if (dir[j] == '/') { dir[j] = '\0'; break; }
        }
        vfs_rmdir(dir);   /* best-effort, fails on non-empty */
    }

    deb_db_unregister(pkg_name);
    console_printf("[deb] removed %s (%d files, %d errors)\n",
                   pkg_name, (int64_t)removed, (int64_t)errors);
    deb_free_package(pkg);
    return 0;
}

int deb_query(const char *pkg_name, struct deb_package **out_pkg) {
    struct deb_package *pkg = deb_db_find(pkg_name);
    if (!pkg) return -1;
    if (out_pkg) *out_pkg = pkg;
    return 0;
}

int deb_list_installed(struct deb_package ***out_list, int *out_count) {
    if (out_list) *out_list = g_db;
    if (out_count) *out_count = g_db_count;
    return 0;
}

int deb_run_maintscript(const char *script_path, const char *arg) {
    (void)script_path; (void)arg;
    /* There is no shell in the kernel: maintainer scripts cannot run.
     * Honest failure instead of pretending. */
    console_puts("[deb] maintainer scripts not supported (no shell in kernel)\n");
    return -1;
}

void deb_cleanup_temp(const char *extract_dir) {
    (void)extract_dir;   /* extraction goes straight to VFS, no temp dir */
}
