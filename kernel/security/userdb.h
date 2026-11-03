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
 *
 * ACCOUNT MANAGEMENT (v0.31.0 multiuser batch): useradd/userdel/passwd/users in
 * the shell drive userdb_add()/userdb_remove()/userdb_save(). /etc/passwd,
 * /etc/shadow and /etc/group are always rewritten together from the in-memory
 * database, so the three files cannot disagree with each other.
 *
 * PASSWORD STATES - the /etc/shadow password field distinguishes three states on
 * purpose (this is the documented "empty password" decision, see docs/multiuser.md):
 *
 *   name:$sutureos$pbkdf2-sha256$...  a password is required  (state=password)
 *   name::0                           EMPTY field = NO password required
 *                                     (state=none / "passwordless"): login, su
 *                                     and the GUI switch accept the account
 *                                     without prompting. An installer that leaves
 *                                     the root password empty writes this, so it
 *                                     is an explicit state instead of an
 *                                     accidental "any password works" hole.
 *                                     sudo still denies such an account: there is
 *                                     no password to prove.
 *   name:!:0                          LOCKED (`!` or `*` prefix): password login
 *                                     is refused for every identity. useradd
 *                                     leaves the account locked when no password
 *                                     is entered ("留空=禁用密码").
 */
#ifndef STITCH_USERDB_H
#define STITCH_USERDB_H

#include <stitch/types.h>

#define USERDB_MAX_USERS   16
#define USERDB_MAX_GROUPS  16
#define USERDB_NAME_LEN    32
#define USERDB_PATH_LEN    48
#define USERDB_SALT_BYTES  16
#define USERDB_ITERATIONS  2000u    /* PBKDF2 iterations for new passwords
                                     * (10k cost ~1 s per check on this kernel and
                                     *  stalled the shell; 2k is ~0.2 s. The count
                                     *  is stored per entry so old hashes still work) */

/* First uid handed out by useradd. */
#define USERDB_UID_MIN     1000u

/* Password state of one account, derived from /etc/shadow. */
typedef enum userdb_pwstate {
    USERDB_PW_NOUSER    = -1,   /* no such account                      */
    USERDB_PW_PASSWORD  = 0,    /* a hash is present: password required  */
    USERDB_PW_NONE      = 1,    /* empty shadow field: no password       */
    USERDB_PW_LOCKED    = 2     /* `!` / `*`: password login refused     */
} userdb_pwstate_t;

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
    int      has_password;       /* 1 = a usable hash is present          */
    int      locked;             /* `!` / `*`: no password login at all   */
    int      passwordless;       /* empty shadow field: no password needed */
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

/* 1 = password matches, 0 = wrong, -1 = unknown user / locked / no hash.
 * A passwordless account deliberately returns -1 as well: callers must use
 * userdb_pwstate() to implement "no password needed", so a locked or hash-less
 * account can never be opened by an empty password. */
int userdb_check_password(const char *name, const char *password);

/* Re-hash and rewrite /etc/shadow (also clears "locked"/"passwordless").
 * Returns 0 on success. */
int userdb_set_password(const char *name, const char *password);

/* ==================== account management ====================
 * These only touch the in-memory database; call userdb_save() to persist and let
 * the command layer decide who may call them (useradd/userdel are root-only). */

int  userdb_valid_name(const char *name);        /* 1 = usable account name */
uint32_t userdb_next_uid(void);                  /* lowest free uid >= 1000 */
/* password != NULL and not empty : hashed password
 * password != NULL and empty     : passwordless (empty shadow field)
 * password == NULL               : locked/disabled account (`!`)
 * Returns 0 on success, -1 invalid name, -2 name already taken, -3 no slot. */
int  userdb_add(const char *name, const char *password, uint32_t *out_uid);
int  userdb_remove(const char *name);            /* 0 on success, -1 absent */
int  userdb_save(void);                          /* rewrite passwd+shadow+group */
int  userdb_lock(const char *name);              /* `passwd -l`            */
int  userdb_unlock(const char *name);            /* `passwd -u`            */
int  userdb_clear_password(const char *name);    /* `passwd -d` -> passwordless */

userdb_pwstate_t userdb_pwstate(const char *name);
int  userdb_is_passwordless(const char *name);
int  userdb_is_locked(const char *name);
/* "password" / "none" / "locked" / "unknown" for `users` and the GUI. */
const char *userdb_pwstate_text(const char *name);
/* "wheel,user" style comma separated group list (primary group first). */
void userdb_format_groups(const char *name, char *out, size_t outlen);

/* 1 when `user` is a member of `group` (primary gid or /etc/group members). */
int userdb_in_group(const char *user, const char *group);

/* Group table accessors (`id` and `users` print supplementary groups from here;
 * the previous code walked the *user* table by mistake and only looked right
 * while "user" happened to be both a user name and a group name). */
uint32_t userdb_group_count(void);
const userdb_group_t *userdb_group_at(uint32_t index);
const char *userdb_group_name(uint32_t gid);   /* "-" when unknown */

/* Hash one password exactly the way /etc/shadow entries are produced
 * (exposed for `sudo selftest` and the passwd command). */
void userdb_hash_password(const char *password, const uint8_t salt[USERDB_SALT_BYTES],
                          uint32_t iterations, uint8_t hash[32]);
void userdb_new_salt(const char *name, uint8_t salt[USERDB_SALT_BYTES]);

/* Diagnostic summary for the boot log: "2 users (root, user), 2 groups". */
const char *userdb_summary(void);

#endif /* STITCH_USERDB_H */
