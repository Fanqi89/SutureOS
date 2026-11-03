/* SutureOS: per-task credential table. See creds.h for the ring-0 boundary note. */
#include "creds.h"
#include "userdb.h"

#include <stitch/console.h>
#include <stitch/string.h>
#include "task.h"

static cred_t g_creds[SCHED_MAX_TASKS];

static void cred_make_root(cred_t *c)
{
    c->uid  = CRED_UID_ROOT;
    c->gid  = CRED_GID_ROOT;
    c->euid = CRED_UID_ROOT;
    c->egid = CRED_GID_ROOT;
    c->suid = CRED_UID_ROOT;
    c->sgid = CRED_GID_ROOT;
    c->valid = 1;
}

void cred_init(void)
{
    memset(g_creds, 0, sizeof(g_creds));
    for (int i = 0; i < SCHED_MAX_TASKS; i++) {
        cred_make_root(&g_creds[i]);
    }
    /* Freshly created tasks inherit the creator's ids in Linux; there is only
     * the boot/shell task today, and it starts as root (root console). */
}

cred_t *cred_current(void)
{
    int slot = sched_cpu.current;
    if (slot < 0 || slot >= SCHED_MAX_TASKS) slot = 0;
    if (!g_creds[slot].valid) cred_make_root(&g_creds[slot]);
    return &g_creds[slot];
}

uint32_t cred_uid(void)  { return cred_current()->uid; }
uint32_t cred_euid(void) { return cred_current()->euid; }
uint32_t cred_gid(void)  { return cred_current()->gid; }
uint32_t cred_egid(void) { return cred_current()->egid; }

int cred_is_root(void)
{
    return cred_current()->euid == CRED_UID_ROOT;
}

int cred_set_user(const char *name)
{
    const userdb_user_t *u = userdb_by_name(name);
    cred_t *c;

    if (u == NULL) return -1;
    c = cred_current();
    c->uid  = u->uid;
    c->gid  = u->gid;
    c->euid = u->uid;
    c->egid = u->gid;
    c->suid = u->uid;
    c->sgid = u->gid;
    c->valid = 1;
    return 0;
}

int cred_set_euid(uint32_t euid)
{
    cred_t *c = cred_current();
    const uint32_t prev = c->euid;

    /* Linux rule: euid may only become the saved set-user-id (setuid binaries)
     * or be dropped. Becoming root without that is exactly what sudo has to
     * authenticate for first, so it goes through cred_escalate_root(). */
    if (euid != c->suid && !cred_is_root()) return -1;
    c->euid = euid;
    return (int)prev;
}

int cred_escalate_root(void)
{
    cred_t *c = cred_current();
    const uint32_t prev = c->euid;
    /* Called only by sudo after it has verified the password and the policy.
     * Logged by the caller (audit trail), like a setuid binary would be. */
    c->euid = CRED_UID_ROOT;
    c->egid = CRED_GID_ROOT;
    return (int)prev;
}

void cred_restore_euid(uint32_t prev_euid)
{
    cred_t *c = cred_current();
    if (prev_euid <= c->suid || prev_euid == CRED_UID_ROOT) {
        c->euid = prev_euid;
    }
}

const char *cred_username(void)
{
    const userdb_user_t *u = userdb_by_uid(cred_euid());
    return (u != NULL) ? u->name : "-";
}

void cred_format_id(char *out, size_t outlen)
{
    const userdb_user_t *u = userdb_by_uid(cred_euid());
    const char *const uname = cred_username();
    const char *gname = userdb_group_name(cred_egid());
    size_t off;
    int n;
    int first = 1;

    n = snprintf(out, outlen, "uid=%u(%s) gid=%u(%s) groups=",
                 (unsigned)cred_euid(), (u != NULL) ? u->name : "-",
                 (unsigned)cred_egid(), gname);
    if (n < 0) return;
    off = (size_t)n;

    /* groups = primary gid first, then every supplementary group from
     * /etc/group the account belongs to (v0.31.0: walk the GROUP table - the
     * old code walked the user table and only looked right by accident). */
    for (uint32_t i = 0; i < userdb_group_count() && off + 32 < outlen; i++) {
        const userdb_group_t *g = userdb_group_at(i);
        int member;
        if (g == NULL) continue;
        member = (g->gid == cred_egid()) || userdb_in_group(uname, g->name);
        if (!member) continue;
        n = snprintf(out + off, outlen - off, "%s%u(%s)", first ? "" : ",",
                     (unsigned)g->gid, g->name);
        if (n < 0) break;
        off += (size_t)n;
        first = 0;
    }
    if (first) {
        snprintf(out + off, outlen - off, "%u(%s)", (unsigned)cred_egid(), gname);
    }
}
