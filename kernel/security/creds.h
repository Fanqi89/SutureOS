/* SutureOS: per-task credentials (uid/gid/euid/egid).
 *
 * Linux keeps credentials in `task_struct->cred`. SutureOS has a single task
 * table (sched_cpu.tasks[SCHED_MAX_TASKS]), so credentials live in a parallel
 * array indexed by task slot, exactly like kernel/syscall/signal.c keys its
 * signal state.
 *
 * HONEST BOUNDARY: today every task still runs in ring 0 and the shell is the
 * only "user". These credentials therefore implement *policy* (who may run
 * mount/shutdown/...), not a hardware-enforced security boundary. They become a
 * real boundary once userland (ring 3 + syscalls) is wired up; the API is the
 * same shape as Linux's so that switch is mechanical.
 *
 * The initial state mirrors a Linux box that booted to a root console: task 0/1
 * (idle + shell) are uid 0. `su user` drops to uid 1000; `sudo` raises euid back
 * to 0 for one command.
 */
#ifndef STITCH_CREDS_H
#define STITCH_CREDS_H

#include <stitch/types.h>

#define CRED_UID_ROOT 0u
#define CRED_GID_ROOT 0u

typedef struct cred {
    uint32_t uid;       /* real user id          */
    uint32_t gid;       /* real group id         */
    uint32_t euid;      /* effective user id     */
    uint32_t egid;      /* effective group id    */
    uint32_t suid;      /* saved set-user-id     */
    uint32_t sgid;      /* saved set-group-id    */
    int      valid;
} cred_t;

/* Initialise the credential table (all tasks root, like a root console). */
void cred_init(void);

/* Credential of the calling task. Never NULL. */
cred_t *cred_current(void);

/* Absolute getters (Linux getuid/geteuid/getgid/getegid). */
uint32_t cred_uid(void);
uint32_t cred_euid(void);
uint32_t cred_gid(void);
uint32_t cred_egid(void);

/* 1 when the effective uid is root. */
int cred_is_root(void);

/* Replace the whole identity (used by `su` / `login`): sets real, effective and
 * saved ids. Returns 0 on success, -1 when no such user. */
int cred_set_user(const char *name);

/* Temporary euid raise/lower around one command (what sudo does with
 * setresuid()). Returns the previous effective uid, or -1 on error.
 *
 * cred_set_euid(0) deliberately FAILS for a normal user (no setuid binary
 * raised suid to 0); only cred_escalate_root(), called by sudo after a
 * successful authentication, may do it. That is what makes the difference
 * between "sudo works" and "anyone can become root". */
int  cred_escalate_root(void);
int  cred_set_euid(uint32_t euid);
void cred_restore_euid(uint32_t prev_euid);

/* Resolve a name for the *effective* uid; "-" when unknown. */
const char *cred_username(void);
/* "uid=1000(user) gid=1000(user) groups=1000(user),10(wheel)" for `id`. */
void cred_format_id(char *out, size_t outlen);

#endif /* STITCH_CREDS_H */
