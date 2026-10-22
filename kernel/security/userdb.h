/* SutureOS: user database (/etc/passwd + /etc/shadow + /etc/group).
 *
 * This is the piece Linux's `sudo` needs in order to be real: identities and
 * password verification. It follows the /etc/passwd and /etc/group line formats
 * verbatim (name:passwd:uid:gid:gecos:home:shell and name:passwd:gid:members) so
 * the files look like a Linux box, and stores password hashes in /etc/shadow as
 *
 *     name:$sutureos$pbkdf2-sha256$<iterations>$<salt-hex>$<hash-hex>:<flags>
 *
 * The hash is salted PBKDF2-HMAC-SHA256 (lib/sha256.c). Linux would use
 * crypt(3)/yescrypt through PAM; that needs a userland libc we do not have yet,
 * so this is the honest equivalent rather than a literal binary port. The format
 * is deliberately NOT Linux's crypt format so nobody mistakes it for one.
 *
 * Default accounts (seeded on first boot, documented in README):
 *     root / "root"   (uid 0)
 *     user / "user"   (uid 1000, member of wheel)
 */
#ifndef STITCH_USERDB_H
#define STITCH_USERDB_H

#include <stitch/types.h>

#define USERDB_MAX_USERS   8
#define USERDB_MAX_GROUPS  8
#define USERDB_NAME_LEN    32
#define USERDB_PATH_LEN    48
#define USERDB_SALT_BYTES  16
#define USERDB_ITERATIONS  2000u    /* PBKDF2 iterations for new passwords
                                     * (10k cost ~1 s per check on this kernel and
                                     *  stalled the shell; 2k is ~0.2 s. The count
                                     * is stored per entry so old hashes still work) */

typedef struct userdb_user {
    char     name[USERDB_NAME_LEN];
    uint32_t uid;
    uint32_t gid;
    char     gecos[USERDB_PATH_LEN];
    char     home[USERDB_PATH_LEN];
    char     shell[USERDB_PATH_LEN];
    /* parsed from /etc/shadow */
    uint32_t iterations;
    uint8_t  salt[USERDB_SALT_BYTES];
    uint8_t  hash[32];
    int      has_password;       /* 0 = account has no usable hash (locked) */
    int      locked;             /* shadow flags */
} userdb_user_t;

typedef struct userdb_group {
    char     name[USERDB_NAME_LEN];
    uint32_t gid;
    char     members[128];       /* comma separated, as in /etc/group */
} userdb_group_t;

/* Seed the /etc files when missing, then parse them. Returns 0 on success. */
int  userdb_init(void);

/* Lookups. Return NULL when absent. */
const userdb_user_t *userdb_by_name(const char *name);
const userdb_user_t *userdb_by_uid(uint32_t uid);
uint32_t             userdb_count(void);
const userdb_user_t *userdb_at(uint32_t index);

/* 1 = password matches, 0 = wrong, -1 = unknown user / locked / no hash. */
int userdb_check_password(const char *name, const char *password);

/* Re-hash and rewrite /etc/shadow. Returns 0 on success. */
int userdb_set_password(const char *name, const char *password);

/* 1 when `user` is a member of `group` (primary gid or /etc/group members). */
int userdb_in_group(const char *user, const char *group);

/* Hash one password exactly the way /etc/shadow entries are produced
 * (exposed for `sudo selftest` and the passwd command). */
void userdb_hash_password(const char *password, const uint8_t salt[USERDB_SALT_BYTES],
                          uint32_t iterations, uint8_t hash[32]);
void userdb_new_salt(const char *name, uint8_t salt[USERDB_SALT_BYTES]);

/* Diagnostic summary for the boot log: "2 users (root, user), 2 groups". */
const char *userdb_summary(void);

#endif /* STITCH_USERDB_H */
