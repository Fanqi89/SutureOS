/* SutureOS: user database implementation (/etc/passwd, /etc/shadow, /etc/group).
 * See userdb.h for the format decisions and the honest limitations.
 */
#include "userdb.h"

#include <stitch/console.h>
#include <stitch/string.h>
#include <stitch/sha256.h>
#include <stitch/io.h>
#include "heap.h"
#include "vfs.h"

#define PASSWD_PATH "/etc/passwd"
#define SHADOW_PATH "/etc/shadow"
#define GROUP_PATH  "/etc/group"

static userdb_user_t  g_users[USERDB_MAX_USERS];
static uint32_t       g_user_count;
static userdb_group_t g_groups[USERDB_MAX_GROUPS];
static uint32_t       g_group_count;
static char           g_summary[128];

/* Default file contents, written on first boot. Passwords are hashed at seed
 * time from these plain texts so the plain text never lands on disk. */
static const char *const k_seed_passwd =
    "# /etc/passwd - SutureOS user database\n"
    "# name:passwd:uid:gid:gecos:home:shell   (passwd field is always 'x':\n"
    "# the hash lives in /etc/shadow, exactly like Linux)\n"
    "root:x:0:0:root:/root:/bin/sh\n"
    "user:x:1000:1000:SutureOS User:/home/user:/bin/sh\n";

static const char *const k_seed_group =
    "# /etc/group - SutureOS groups\n"
    "# name:passwd:gid:members\n"
    "root:x:0:\n"
    "wheel:x:10:user\n"
    "user:x:1000:\n";

static const char *const k_seed_sudoers =
    "# /etc/sudoers - SutureOS sudo policy (Linux sudo syntax subset)\n"
    "#\n"
    "#   user  HOST=(RUNAS:RUNAS_GROUP) [NOPASSWD:] COMMAND\n"
    "#   %group ...            matches every member of that group\n"
    "#   Defaults key=value    timestamp_timeout (minutes), passwd_tries\n"
    "Defaults timestamp_timeout=15\n"
    "Defaults passwd_tries=3\n"
    "root ALL=(ALL:ALL) ALL\n"
    "%wheel ALL=(ALL:ALL) ALL\n"
    "user ALL=(ALL:ALL) ALL\n";

#define SEED_ROOT_PASSWORD "root"
#define SEED_USER_PASSWORD "user"

/* ============================ small helpers ============================ */

static void copy_str(char *dst, size_t dstlen, const char *src)
{
    size_t i = 0;
    if (dstlen == 0) return;
    if (src != NULL) {
        while (src[i] != '\0' && i + 1 < dstlen) {
            dst[i] = src[i];
            i++;
        }
    }
    dst[i] = '\0';
}

static uint32_t parse_u32(const char *s, uint32_t fallback)
{
    uint32_t v = 0;
    int any = 0;
    if (!s) return fallback;
    while (*s >= '0' && *s <= '9') {
        v = v * 10u + (uint32_t)(*s - '0');
        s++;
        any = 1;
    }
    return any ? v : fallback;
}

/* Constant-time-ish compare of two 32-byte digests. */
static int digest_equal(const uint8_t *a, const uint8_t *b)
{
    uint8_t diff = 0;
    for (int i = 0; i < 32; i++) diff |= (uint8_t)(a[i] ^ b[i]);
    return diff == 0;
}

/* Read a while-file into a heap buffer (NUL terminated). Caller frees. */
static char *read_text_file(const char *path, size_t *out_len)
{
    size_t len = 0;
    void *buf = vfs_read_file(path, &len);
    if (buf == NULL) return NULL;
    if (out_len) *out_len = len;
    return (char *)buf;
}

/* Iterate lines of a buffer in place (writes '\0' over the newlines). */
static char *next_line(char **cursor)
{
    char *p = *cursor;
    char *start;
    if (p == NULL || *p == '\0') {
        *cursor = NULL;
        return NULL;
    }
    start = p;
    while (*p != '\0' && *p != '\n') p++;
    if (*p == '\n') {
        *p = '\0';
        p++;
    }
    *cursor = (*p == '\0') ? NULL : p;
    return start;
}

/* Split on ':' in place; returns the next field or NULL. */
static char *field(char **cursor)
{
    char *p;
    char *start;
    if (cursor == NULL || *cursor == NULL) return NULL;
    start = *cursor;
    p = start;
    while (*p != '\0' && *p != ':') p++;
    if (*p == ':') {
        *p = '\0';
        *cursor = p + 1;
    } else {
        *cursor = NULL;
    }
    return start;
}

/* Split on '$' in place (the /etc/shadow hash spec uses '$' as its separator,
 * unlike the ':' separated passwd/group lines). */
static char *dollar_field(char **cursor)
{
    char *start;
    char *p;
    if (cursor == NULL || *cursor == NULL) return NULL;
    start = *cursor;
    p = start;
    while (*p != '\0' && *p != '$') p++;
    if (*p == '$') {
        *p = '\0';
        *cursor = p + 1;
    } else {
        *cursor = NULL;
    }
    return start;
}

static int is_blank_or_comment(const char *line)
{
    while (*line == ' ' || *line == '\t') line++;
    return (*line == '\0' || *line == '#');
}

/* ============================ password hashing ============================ */

void userdb_hash_password(const char *password, const uint8_t salt[USERDB_SALT_BYTES],
                          uint32_t iterations, uint8_t hash[32])
{
    const size_t plen = (password != NULL) ? strlen(password) : 0;
    if (iterations == 0) iterations = USERDB_ITERATIONS;
    pbkdf2_sha256(password ? password : "", plen, salt, USERDB_SALT_BYTES,
                  iterations, hash, 32);
}

void userdb_new_salt(const char *name, uint8_t salt[USERDB_SALT_BYTES])
{
    /* No CSPRNG yet (getrandom() in the syscall layer is a LCG), so mix every
     * cheap entropy source we have and hash it. Good enough for account salts
     * (which only need to be unique, not secret), and documented as such. */
    uint8_t seed[96];
    uint64_t tsc;
    static uint32_t counter;
    sha256_ctx_t ctx;
    uint8_t digest[32];
    size_t namelen = (name != NULL) ? strlen(name) : 0;

    __asm__ volatile("rdtsc" : "=A"(tsc));
    memset(seed, 0, sizeof(seed));
    memcpy(seed, &tsc, sizeof(tsc));
    memcpy(seed + 8, &counter, sizeof(counter));
    memcpy(seed + 12, &g_user_count, sizeof(g_user_count));
    if (namelen > 32) namelen = 32;
    if (namelen > 0) memcpy(seed + 16, name, namelen);

    sha256_init(&ctx);
    sha256_update(&ctx, seed, sizeof(seed));
    sha256_final(&ctx, digest);
    counter++;
    memcpy(salt, digest, USERDB_SALT_BYTES);
}

/* ============================ shadow parsing ============================ */

/* "$sutureos$pbkdf2-sha256$<iters>$<salt-hex>$<hash-hex>" */
static int parse_hash_string(const char *spec, userdb_user_t *out)
{
    char work[192];
    char *cur;
    const char *alg;
    const char *iters;
    const char *salt_hex;
    const char *hash_hex;

    copy_str(work, sizeof(work), spec);
    if (work[0] != '$') return -1;

    cur = work + 1;
    {
        char *scheme = dollar_field(&cur);
        if (scheme == NULL || strcmp(scheme, "sutureos") != 0) return -1;
    }
    alg = dollar_field(&cur);
    if (alg == NULL || strcmp(alg, "pbkdf2-sha256") != 0) return -1;
    iters = dollar_field(&cur);
    salt_hex = dollar_field(&cur);
    hash_hex = dollar_field(&cur);
    if (iters == NULL || salt_hex == NULL || hash_hex == NULL) return -1;
    if (strlen(salt_hex) != USERDB_SALT_BYTES * 2) return -1;
    if (strlen(hash_hex) != 64) return -1;

    out->iterations = parse_u32(iters, USERDB_ITERATIONS);
    if (out->iterations == 0) out->iterations = USERDB_ITERATIONS;
    if (hex_to_bytes(salt_hex, out->salt, USERDB_SALT_BYTES) != 0) return -1;
    if (hex_to_bytes(hash_hex, out->hash, 32) != 0) return -1;
    out->has_password = 1;
    return 0;
}

static void format_hash_string(const userdb_user_t *user, char *out, size_t outlen)
{
    char salt_hex[USERDB_SALT_BYTES * 2 + 1];
    char hash_hex[65];
    static const char hexd[] = "0123456789abcdef";
    int i;
    for (i = 0; i < USERDB_SALT_BYTES; i++) {
        salt_hex[i * 2]     = hexd[(user->salt[i] >> 4) & 0xF];
        salt_hex[i * 2 + 1] = hexd[user->salt[i] & 0xF];
    }
    salt_hex[USERDB_SALT_BYTES * 2] = '\0';
    sha256_hex(user->hash, hash_hex);
    snprintf(out, outlen, "$sutureos$pbkdf2-sha256$%u$%s$%s",
             (unsigned)user->iterations, salt_hex, hash_hex);
}

/* ============================ seeding ============================ */

static userdb_user_t *find_mut(const char *name)
{
    if (name == NULL) return NULL;
    for (uint32_t i = 0; i < g_user_count; i++) {
        if (strcmp(g_users[i].name, name) == 0) return &g_users[i];
    }
    return NULL;
}

static int write_shadow_file(void)
{
    char buf[1024];
    size_t off = 0;
    int n;

    n = snprintf(buf, sizeof(buf),
                 "# /etc/shadow - SutureOS password hashes\n"
                 "# name:$sutureos$pbkdf2-sha256$<iters>$<salt>$<hash>:<flags>\n");
    if (n < 0) return -1;
    off = (size_t)n;

    for (uint32_t i = 0; i < g_user_count && off + 200 < sizeof(buf); i++) {
        char spec[192];
        if (!g_users[i].has_password) {
            n = snprintf(buf + off, sizeof(buf) - off, "%s:!:%u\n",
                         g_users[i].name, (unsigned)g_users[i].locked);
        } else {
            format_hash_string(&g_users[i], spec, sizeof(spec));
            n = snprintf(buf + off, sizeof(buf) - off, "%s:%s:%u\n",
                         g_users[i].name, spec, (unsigned)g_users[i].locked);
        }
        if (n < 0) return -1;
        off += (size_t)n;
    }
    return vfs_write_file(SHADOW_PATH, buf, (uint32_t)off);
}

/* Create the default accounts (with hashed passwords) if /etc/passwd is missing. */
static int seed_defaults(void)
{
    size_t len = 0;
    char *existing = read_text_file(PASSWD_PATH, &len);

    if (existing != NULL) {
        free(existing);
        return 0;                       /* already provisioned */
    }

    /* root */
    memset(&g_users[0], 0, sizeof(g_users[0]));
    copy_str(g_users[0].name, USERDB_NAME_LEN, "root");
    g_users[0].uid = 0;
    g_users[0].gid = 0;
    copy_str(g_users[0].gecos, USERDB_PATH_LEN, "root");
    copy_str(g_users[0].home, USERDB_PATH_LEN, "/root");
    copy_str(g_users[0].shell, USERDB_PATH_LEN, "/bin/sh");
    g_users[0].iterations = USERDB_ITERATIONS;
    userdb_new_salt("root", g_users[0].salt);
    userdb_hash_password(SEED_ROOT_PASSWORD, g_users[0].salt,
                         g_users[0].iterations, g_users[0].hash);
    g_users[0].has_password = 1;

    /* user */
    memset(&g_users[1], 0, sizeof(g_users[1]));
    copy_str(g_users[1].name, USERDB_NAME_LEN, "user");
    g_users[1].uid = 1000;
    g_users[1].gid = 1000;
    copy_str(g_users[1].gecos, USERDB_PATH_LEN, "SutureOS User");
    copy_str(g_users[1].home, USERDB_PATH_LEN, "/home/user");
    copy_str(g_users[1].shell, USERDB_PATH_LEN, "/bin/sh");
    g_users[1].iterations = USERDB_ITERATIONS;
    userdb_new_salt("user", g_users[1].salt);
    userdb_hash_password(SEED_USER_PASSWORD, g_users[1].salt,
                         g_users[1].iterations, g_users[1].hash);
    g_users[1].has_password = 1;

    g_user_count = 2;

    (void)vfs_write_file(PASSWD_PATH, k_seed_passwd, (uint32_t)strlen(k_seed_passwd));
    (void)vfs_write_file(GROUP_PATH, k_seed_group, (uint32_t)strlen(k_seed_group));
    (void)vfs_write_file("/etc/sudoers", k_seed_sudoers, (uint32_t)strlen(k_seed_sudoers));
    if (write_shadow_file() != 0) {
        console_puts("[userdb] warning: could not write /etc/shadow\n");
    }
    console_printf("[userdb] seeded %s, %s, %s, /etc/sudoers "
                   "(root/%s, user/%s)\n",
                   PASSWD_PATH, SHADOW_PATH, GROUP_PATH,
                   SEED_ROOT_PASSWORD, SEED_USER_PASSWORD);
    return 0;
}

/* ============================ parsing ============================ */

static void parse_passwd(void)
{
    size_t len = 0;
    char *buf = read_text_file(PASSWD_PATH, &len);
    char *cur;
    char *line;

    if (buf == NULL) return;

    g_user_count = 0;
    cur = buf;
    while ((line = next_line(&cur)) != NULL && g_user_count < USERDB_MAX_USERS) {
        char *p = line;
        char *name;
        char *passwd;
        char *uid;
        char *gid;
        char *gecos;
        char *home;
        char *shell;

        if (is_blank_or_comment(line)) continue;
        name   = field(&p);
        passwd = field(&p);
        uid    = field(&p);
        gid    = field(&p);
        gecos  = field(&p);
        home   = field(&p);
        shell  = field(&p);
        (void)passwd;
        if (name == NULL || uid == NULL || gid == NULL) continue;

        userdb_user_t *u = &g_users[g_user_count];
        memset(u, 0, sizeof(*u));
        copy_str(u->name, USERDB_NAME_LEN, name);
        u->uid = parse_u32(uid, 0);
        u->gid = parse_u32(gid, 0);
        copy_str(u->gecos, USERDB_PATH_LEN, gecos ? gecos : "");
        copy_str(u->home, USERDB_PATH_LEN, home ? home : "/");
        copy_str(u->shell, USERDB_PATH_LEN, shell ? shell : "/bin/sh");
        u->iterations = USERDB_ITERATIONS;
        g_user_count++;
    }
    free(buf);
}

static void parse_shadow(void)
{
    size_t len = 0;
    char *buf = read_text_file(SHADOW_PATH, &len);
    char *cur;
    char *line;

    if (buf == NULL) return;
    cur = buf;
    while ((line = next_line(&cur)) != NULL) {
        char *p = line;
        char *name;
        char *spec;
        char *flags;
        userdb_user_t *u;

        if (is_blank_or_comment(line)) continue;
        name  = field(&p);
        spec  = field(&p);
        flags = field(&p);
        if (name == NULL || spec == NULL) continue;
        u = find_mut(name);
        if (u == NULL) continue;

        if (spec[0] == '!' || spec[0] == '*') {
            u->has_password = 0;         /* locked account, like Linux `!` */
            u->locked = 1;
            continue;
        }
        if (parse_hash_string(spec, u) != 0) {
            u->has_password = 0;
            console_printf("[userdb] %s: unusable hash in %s (account locked)\n",
                           name, SHADOW_PATH);
            console_printf("[userdb]   spec len=%u first=%c%.32s\n",
                           (uint64_t)strlen(spec),
                           (spec[0] != '\0') ? spec[0] : '?',
                           spec);
            continue;
        }
        u->locked = (flags != NULL) ? (int)parse_u32(flags, 0) : 0;
    }
    free(buf);
}

static void parse_group(void)
{
    size_t len = 0;
    char *buf = read_text_file(GROUP_PATH, &len);
    char *cur;
    char *line;

    if (buf == NULL) return;
    g_group_count = 0;
    cur = buf;
    while ((line = next_line(&cur)) != NULL && g_group_count < USERDB_MAX_GROUPS) {
        char *p = line;
        char *name;
        char *passwd;
        char *gid;
        char *members;
        userdb_group_t *g;

        if (is_blank_or_comment(line)) continue;
        name    = field(&p);
        passwd  = field(&p);
        gid     = field(&p);
        members = field(&p);
        (void)passwd;
        if (name == NULL || gid == NULL) continue;

        g = &g_groups[g_group_count];
        memset(g, 0, sizeof(*g));
        copy_str(g->name, USERDB_NAME_LEN, name);
        g->gid = parse_u32(gid, 0);
        copy_str(g->members, sizeof(g->members), members ? members : "");
        g_group_count++;
    }
    free(buf);
}

int userdb_init(void)
{
    memset(g_users, 0, sizeof(g_users));
    memset(g_groups, 0, sizeof(g_groups));
    g_user_count = 0;
    g_group_count = 0;

    if (seed_defaults() != 0) return -1;
    parse_passwd();
    parse_shadow();
    parse_group();

    if (g_user_count == 0) {
        console_puts("[userdb] error: no users parsed from /etc/passwd\n");
        return -1;
    }

    /* human readable summary for the boot log */
    {
        size_t off = 0;
        int n = snprintf(g_summary, sizeof(g_summary), "%u users (",
                         (unsigned)g_user_count);
        if (n > 0) off = (size_t)n;
        for (uint32_t i = 0; i < g_user_count && off + 40 < sizeof(g_summary); i++) {
            n = snprintf(g_summary + off, sizeof(g_summary) - off, "%s%s",
                         (i == 0) ? "" : ", ", g_users[i].name);
            if (n < 0) break;
            off += (size_t)n;
        }
        snprintf(g_summary + off, sizeof(g_summary) - off, "), %u groups",
                 (unsigned)g_group_count);
    }
    return 0;
}

const char *userdb_summary(void) { return g_summary; }

/* ============================ lookups / auth ============================ */

const userdb_user_t *userdb_by_name(const char *name)
{
    return find_mut(name);
}

const userdb_user_t *userdb_by_uid(uint32_t uid)
{
    for (uint32_t i = 0; i < g_user_count; i++) {
        if (g_users[i].uid == uid) return &g_users[i];
    }
    return NULL;
}

uint32_t userdb_count(void) { return g_user_count; }

const userdb_user_t *userdb_at(uint32_t index)
{
    return (index < g_user_count) ? &g_users[index] : NULL;
}

int userdb_check_password(const char *name, const char *password)
{
    const userdb_user_t *u = userdb_by_name(name);
    uint8_t computed[32];

    if (u == NULL) return -1;
    if (!u->has_password || u->locked) return -1;

    userdb_hash_password(password, u->salt, u->iterations, computed);
    return digest_equal(computed, u->hash) ? 1 : 0;
}

int userdb_set_password(const char *name, const char *password)
{
    userdb_user_t *u = find_mut(name);
    if (u == NULL) return -1;

    u->iterations = USERDB_ITERATIONS;
    userdb_new_salt(name, u->salt);
    userdb_hash_password(password, u->salt, u->iterations, u->hash);
    u->has_password = 1;
    u->locked = 0;
    return write_shadow_file();
}

int userdb_in_group(const char *user, const char *group)
{
    const userdb_user_t *u = userdb_by_name(user);

    if (u == NULL || group == NULL) return 0;
    for (uint32_t i = 0; i < g_group_count; i++) {
        const userdb_group_t *g = &g_groups[i];
        const char *m;
        if (strcmp(g->name, group) != 0) continue;
        if (g->gid == u->gid) return 1;          /* primary group */
        /* members: "a,b,c" */
        m = g->members;
        while (*m != '\0') {
            const char *start = m;
            size_t n;
            while (*m != '\0' && *m != ',') m++;
            n = (size_t)(m - start);
            if (n == strlen(u->name) && strncmp(start, u->name, n) == 0) return 1;
            if (*m == ',') m++;
        }
    }
    return 0;
}
