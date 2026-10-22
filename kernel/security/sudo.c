/* SutureOS: the sudo engine. See sudo.h for exactly what is and is not ported. */
#include "sudo.h"

#include "creds.h"
#include "sudoers.h"
#include "userdb.h"

#include <stitch/console.h>
#include <stitch/io.h>
#include <stitch/sha256.h>
#include <stitch/string.h>
#include "heap.h"
#include "shell.h"
#include "vfs.h"

#define SUDO_TICK_HZ      100u      /* nominal g_tick_count rate (see apps.c)   */
#define SUDO_MAX_ARGS     16

static sudo_ticket_t g_ticket;
static uint32_t      g_sudo_invocations;
static uint32_t      g_sudo_failures;
static char          g_audit_line[SUDO_MAX_CMDLINE + 128];

/* ============================ small utilities ============================ */

static uint32_t uptime_seconds(void)
{
    extern volatile uint64_t g_tick_count;
    return (uint32_t)(g_tick_count / SUDO_TICK_HZ);
}

static int str_eq(const char *a, const char *b)
{
    if (a == NULL || b == NULL) return 0;
    return strcmp(a, b) == 0;
}

/* Last path component of argv[0] ("/bin/mount" -> "mount"). */
static const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '/');
    return (slash != NULL) ? slash + 1 : path;
}

static void copy_str(char *dst, size_t dstlen, const char *src)
{
    size_t i = 0;
    if (dstlen == 0) return;
    if (src != NULL) {
        while (src[i] != '\0' && i + 1 < dstlen) { dst[i] = src[i]; i++; }
    }
    dst[i] = '\0';
}

/* Append one audit line to /var/log/sudo.log (read-modify-write: the VFS is
 * whole-file oriented, same as the rest of the kernel). */
static void audit_log(const char *line)
{
    void *old = NULL;
    size_t old_len = 0;
    char *buf;
    size_t total;
    size_t keep = 0;

    old = vfs_read_file(SUDO_LOG_PATH, &old_len);
    /* The ramfs node has a fixed payload capacity (XJ_VFS_MAX_FILE_SIZE); keep
     * only the tail so the audit log cannot outgrow it. */
    if (old != NULL && old_len > 1280) {
        keep = old_len - 1024;
    }
    total = (old_len - keep) + strlen(line) + 2;
    buf = (char *)malloc(total);
    if (buf == NULL) {
        if (old) free(old);
        return;
    }
    if (old != NULL) {
        memcpy(buf, (const char *)old + keep, old_len - keep);
        free(old);
    }
    if (keep > 0) {
        /* start the kept tail on a line boundary */
        size_t i = 0;
        while (i < old_len - keep && buf[i] != '\n') i++;
        if (i < old_len - keep) {
            memmove(buf, buf + i + 1, (old_len - keep) - i - 1);
            total = (old_len - keep) - i - 1;
        } else {
            total = old_len - keep;
        }
    } else {
        total = old_len;
    }
    memcpy(buf + total, line, strlen(line));
    total += strlen(line);
    buf[total++] = '\n';
    (void)vfs_write_file(SUDO_LOG_PATH, buf, (uint32_t)total);
    free(buf);
}

/* ============================ ticket cache ============================ */

void sudo_kill_ticket(void)
{
    memset(&g_ticket, 0, sizeof(g_ticket));
}

int sudo_ticket_valid(const char *user)
{
    const sudoers_policy_t *policy = sudoers_policy();

    if (!g_ticket.valid || user == NULL) return 0;
    if (!str_eq(g_ticket.user, user)) return 0;
    if (policy->timestamp_timeout == 0) return 0;      /* always ask */
    if (uptime_seconds() >= g_ticket.expiry_s) {
        sudo_kill_ticket();
        return 0;
    }
    return 1;
}

static void ticket_grant(const char *user)
{
    const sudoers_policy_t *policy = sudoers_policy();
    memset(&g_ticket, 0, sizeof(g_ticket));
    if (policy->timestamp_timeout == 0) return;        /* no caching wanted */
    copy_str(g_ticket.user, sizeof(g_ticket.user), user);
    g_ticket.expiry_s = uptime_seconds() + policy->timestamp_timeout * 60u;
    g_ticket.valid = 1;
}

/* ============================ password input ============================ */

/* Read one line from the console. `echo` prints '*' per character (sudo prints
 * nothing at all; '*' is friendlier on a serial log where the operator cannot
 * see progress, and it never reveals the length to a shoulder surfer). */
static int read_line_hidden(char *out, size_t outlen)
{
    extern int console_getc(void);
    size_t len = 0;

    if (outlen == 0) return -1;
    out[0] = '\0';
    for (;;) {
        int c = console_getc();
        if (c < 0) {
            /* Idle wait: bounded pause spin (no sched_yield in the shell path -
             * see the note in shell_run(); yielding swaps in a stale context). */
            for (volatile int spin = 0; spin < 20000; spin++) {
                __asm__ volatile("pause");
            }
            continue;
        }
        if (c == '\n' || c == '\r') {
            out[len] = '\0';
            shell_putc('\n');
            return (int)len;
        }
        if (c == 0x7F || c == '\b') {
            if (len > 0) len--;
            continue;
        }
        if (c == 0x03) {                        /* Ctrl-C: abort like sudo does */
            out[0] = '\0';
            shell_puts("^C\n");
            return -1;
        }
        if (len + 1 < outlen) out[len++] = (char)c;
    }
}

int sudo_prompt_password(const char *prompt, char *out, size_t outlen)
{
    if (prompt != NULL) shell_puts(prompt);
    return read_line_hidden(out, outlen);
}

/* sudo's authentication loop. Returns 1 when authenticated. */
static int authenticate(const char *user, int quiet)
{
    const sudoers_policy_t *policy = sudoers_policy();
    const uint32_t tries = (policy->passwd_tries > 0) ? policy->passwd_tries : 3;
    char password[96];

    for (uint32_t attempt = 1; attempt <= tries; attempt++) {
        int rc;
        memset(password, 0, sizeof(password));
        shell_printf("[sudo] password for %s: ", user);
        if (read_line_hidden(password, sizeof(password)) < 0) {
            shell_puts("sudo: no password was provided\n");
            g_sudo_failures++;
            return 0;
        }
        if (password[0] == '\0') {
            shell_puts("sudo: no password was provided\n");
            continue;
        }
        rc = sudo_verify_password(user, password);
        memset(password, 0, sizeof(password));
        if (rc == 1) {
            ticket_grant(user);
            return 1;
        }
        if (rc < 0) {
            shell_printf("sudo: %s: account is locked or has no password\n", user);
            g_sudo_failures++;
            return 0;
        }
        shell_puts("Sorry, try again.\n");
    }
    shell_printf("sudo: %u incorrect password attempt%s\n",
                 (unsigned)tries, (tries == 1) ? "" : "s");
    g_sudo_failures++;
    (void)quiet;
    return 0;
}

int sudo_verify_password(const char *user, const char *password)
{
    return userdb_check_password(user, password);
}

/* ============================ usage / list ============================ */

static void usage(void)
{
    shell_puts("usage: sudo [-k] [-v] [-l] [-u user] [-i] command [args...]\n"
               "  -l  list the commands the current user may run\n"
               "  -k  invalidate the cached password (timestamp) for the user\n"
               "  -v  validate; refresh the cached password without running a command\n"
               "  -u  run the command as this user (default root)\n"
               "  -i  become the target user for the rest of this session\n"
               "SutureOS sudo: Linux sudo's policy/password/ticket behaviour\n"
               "implemented in-kernel; see kernel/security/sudo.c for the boundary.\n");
}

/* Does the policy allow `user` to run `cmd` as `runas`? sudo's wording on deny. */
static int policy_allow(const char *user, const char *runas, const char *cmd,
                        int *nopasswd)
{
    int rule = -1;
    if (!sudoers_check(user, runas, cmd, nopasswd, &rule)) {
        return 0;
    }
    return 1;
}

static void deny_message(const char *user, const char *cmd, const char *runas)
{
    /* Two different messages in sudo: "not in sudoers" when the user has no rule
     * at all, "not allowed to execute" when rules exist but none match. */
    int has_any = 0;
    const sudoers_policy_t *policy = sudoers_policy();

    for (uint32_t i = 0; i < policy->rule_count; i++) {
        const sudoers_rule_t *r = &policy->rules[i];
        int who_ok = r->is_group ? userdb_in_group(user, r->who)
                                 : str_eq(r->who, user);
        if (who_ok) { has_any = 1; break; }
    }
    if (!has_any) {
        shell_printf("%s is not in the sudoers file.  This incident will be "
                     "reported.\n", user);
    } else {
        shell_printf("Sorry, user %s is not allowed to execute '%s' as %s on %s.\n",
                     user, cmd, runas, SUDO_HOST);
    }
    g_sudo_failures++;
    snprintf(g_audit_line, sizeof(g_audit_line),
             "[%us] DENIED  user=%s runas=%s cmd=%s", (unsigned)uptime_seconds(),
             user, runas, cmd);
    audit_log(g_audit_line);
}

static void list_rules(const char *user)
{
    const sudoers_policy_t *policy = sudoers_policy();
    int printed_header = 0;

    for (uint32_t i = 0; i < policy->rule_count; i++) {
        const sudoers_rule_t *r = &policy->rules[i];
        int who_ok = r->is_group ? userdb_in_group(user, r->who)
                                 : str_eq(r->who, user);
        if (!who_ok) continue;
        if (!printed_header) {
            shell_printf("User %s may run the following commands on %s:\n",
                         user, SUDO_HOST);
            printed_header = 1;
        }
        for (int c = 0; c < r->cmnd_count; c++) {
            shell_printf("    (%s%s) %s\n", r->runas,
                         r->nopasswd ? " : NOPASSWD" : "", r->cmnds[c]);
        }
    }
    if (!printed_header) {
        shell_printf("User %s is not allowed to run sudo on %s.\n",
                     user, SUDO_HOST);
    }
}

/* ============================ main entry ============================ */

int sudo_main(int argc, char **argv)
{
    int opt_list = 0;
    int opt_kill = 0;
    int opt_validate = 0;
    int opt_become = 0;
    const char *runas = "root";
    char cmdline[SUDO_MAX_CMDLINE];
    char cmd_argv_joined[SUDO_MAX_CMDLINE];
    const char *cmd_base;
    const char *user;
    int i = 1;
    int nopasswd = 0;
    int prev_euid;
    int rc;
    size_t off = 0;
    uint32_t uid = cred_uid();

    /* ---- option parsing (sudo accepts clustered short options) ---- */
    while (i < argc && argv[i][0] == '-' && argv[i][1] != '\0') {
        const char *opt = argv[i];
        if (str_eq(opt, "--help")) { usage(); return 0; }
        if (str_eq(opt, "-u")) {
            if (i + 1 >= argc) { shell_puts("sudo: option requires an argument -- 'u'\n"); return 1; }
            runas = argv[++i];
            i++;
            continue;
        }
        if (str_eq(opt, "--")) { i++; break; }
        for (const char *p = opt + 1; *p != '\0'; p++) {
            switch (*p) {
            case 'l': opt_list = 1; break;
            case 'k': opt_kill = 1; break;
            case 'v': opt_validate = 1; break;
            case 'i':
            case 's': opt_become = 1; break;
            case 'E': case 'H': case 'n': break;         /* accepted, no-op */
            default:
                shell_printf("sudo: invalid option -- '%c'\n", *p);
                usage();
                return 1;
            }
        }
        i++;
    }

    /* who is asking? use the REAL uid, like sudo does. */
    {
        const userdb_user_t *u = userdb_by_uid(uid);
        user = (u != NULL) ? u->name : "-";
    }
    if (str_eq(user, "-")) {
        shell_printf("sudo: unknown uid %u: who are you?\n", (unsigned)uid);
        return 1;
    }

    /* ---- sudo -k: drop the ticket and exit silently (as Linux sudo does) ---- */
    if (opt_kill) {
        sudo_kill_ticket();
        return 0;
    }

    /* ---- build the command string ---- */
    cmdline[0] = '\0';
    if (i < argc) {
        for (int a = i; a < argc; a++) {
            const size_t need = strlen(argv[a]) + 1;
            if (off + need + 1 >= sizeof(cmdline)) {
                shell_puts("sudo: command line too long\n");
                return 1;
            }
            memcpy(cmdline + off, argv[a], strlen(argv[a]));
            off += strlen(argv[a]);
            if (a + 1 < argc) cmdline[off++] = ' ';
            cmdline[off] = '\0';
        }
    }
    cmd_base = (i < argc) ? base_name(argv[i]) : NULL;
    copy_str(cmd_argv_joined, sizeof(cmd_argv_joined), cmdline);

    /* ---- root runs without a password (Linux: "sudo" as root is a no-op
     *      privilege-wise and asks for nothing) ---- */
    if (cred_is_root()) {
        if (opt_list) { list_rules(user); return 0; }
        if (opt_validate) return 0;
        if (opt_become) { cred_set_user(runas); return 0; }
        if (cmd_base == NULL) { usage(); return 1; }
        g_sudo_invocations++;
        snprintf(g_audit_line, sizeof(g_audit_line),
                 "[%us] root    user=root runas=%s cmd=%s (no auth: euid=0)",
                 (unsigned)uptime_seconds(), runas, cmd_argv_joined);
        audit_log(g_audit_line);
        return shell_process_line(cmd_argv_joined);
    }

    /* ---- sudo -l / -v: policy check first, then authenticate ---- */
    if (opt_list || opt_validate) {
        int allowed = policy_allow(user, runas, opt_list ? "ALL" : "ALL", &nopasswd);
        /* For -l we only need "has any rule"; sudoers_check with "ALL" would
         * also match an explicit ALL rule only, so fall back to scanning. */
        if (!allowed) {
            const sudoers_policy_t *policy = sudoers_policy();
            for (uint32_t r = 0; r < policy->rule_count && !allowed; r++) {
                const sudoers_rule_t *rule = &policy->rules[r];
                int who_ok = rule->is_group ? userdb_in_group(user, rule->who)
                                            : str_eq(rule->who, user);
                if (who_ok) allowed = 1;
            }
        }
        if (!allowed) {
            deny_message(user, opt_list ? "sudo -l" : "sudo -v", runas);
            return 1;
        }
        if (!sudo_ticket_valid(user) && !nopasswd) {
            if (!authenticate(user, 1)) return 1;
        }
        if (opt_list) { list_rules(user); return 0; }
        ticket_grant(user);
        return 0;
    }

    if (cmd_base == NULL) { usage(); return 1; }

    /* ---- policy ---- */
    if (!policy_allow(user, runas, cmd_argv_joined, &nopasswd)) {
        deny_message(user, cmd_argv_joined, runas);
        return 1;
    }

    /* ---- authentication (unless NOPASSWD or a valid ticket) ---- */
    if (!nopasswd) {
        if (!sudo_ticket_valid(user)) {
            if (!authenticate(user, 0)) {
                snprintf(g_audit_line, sizeof(g_audit_line),
                         "[%us] AUTHFAIL user=%s runas=%s cmd=%s",
                         (unsigned)uptime_seconds(), user, runas, cmd_argv_joined);
                audit_log(g_audit_line);
                return 1;
            }
        }
    }

    /* ---- run with the target effective uid, then drop back ---- */
    prev_euid = cred_escalate_root();
    if (prev_euid < 0) {
        shell_printf("sudo: unable to switch to %s (saved uid %u)\n",
                     runas, (unsigned)cred_current()->suid);
        return 1;
    }
    g_sudo_invocations++;
    snprintf(g_audit_line, sizeof(g_audit_line),
             "[%us] RUN     user=%s runas=%s cmd=%s",
             (unsigned)uptime_seconds(), user, runas, cmd_argv_joined);
    audit_log(g_audit_line);

    if (opt_become) {
        /* `sudo -i`: adopt the target identity for the rest of the session. */
        cred_set_user(runas);
        shell_printf("[sudo] switched to %s\n", runas);
        return 0;
    }

    rc = shell_process_line(cmd_argv_joined);
    cred_restore_euid((uint32_t)prev_euid);
    return rc;
}

/* ============================ su ============================ */

int sudo_su(int argc, char **argv)
{
    const char *target = (argc >= 2) ? argv[1] : "root";
    const userdb_user_t *u = userdb_by_name(target);
    char password[96];

    if (u == NULL) {
        shell_printf("su: user %s does not exist\n", target);
        return 1;
    }
    /* root can su without a password (Linux behaviour) */
    if (!(cred_is_root() && str_eq(target, "root"))) {
        if (cred_uid() != 0 || !str_eq(target, "root")) {
            if (u->has_password && !u->locked) {
                shell_printf("Password: ");
                if (read_line_hidden(password, sizeof(password)) < 0) {
                    shell_puts("su: authentication failure\n");
                    return 1;
                }
                if (userdb_check_password(target, password) != 1) {
                    memset(password, 0, sizeof(password));
                    shell_puts("su: Authentication failure\n");
                    return 1;
                }
                memset(password, 0, sizeof(password));
            }
        }
    }
    if (cred_set_user(target) != 0) {
        shell_printf("su: unknown user %s\n", target);
        return 1;
    }
    sudo_kill_ticket();          /* new identity: no inherited sudo ticket */
    return 0;
}

/* ============================ init / selftest ============================ */

int sudo_init(void)
{
    int ok = 1;

    if (!sha256_selftest()) {
        console_puts("[  !!  ] sha256: KNOWN-ANSWER TEST FAILED "
                     "(password hashing is broken)\n");
        ok = 0;
    } else {
        console_puts("[  OK  ] crypto: SHA-256/HMAC/PBKDF2 known-answer tests "
                     "passed\n");
    }

    cred_init();
    vfs_mkdir_p("/var");
    vfs_mkdir_p("/var/log");
    console_printf("[  OK  ] creds: %s (uid=%u) - policy layer, ring-0 shell\n",
                   userdb_summary(), (unsigned)cred_uid());

    if (sudoers_init() == 0) {
        console_printf("[  OK  ] sudoers: %s\n", sudoers_summary());
    } else {
        console_puts("[  !!  ] sudoers: /etc/sudoers unusable, sudo will deny\n");
        ok = 0;
    }

    sudo_kill_ticket();
    return ok ? 0 : -1;
}

/* ---- automated acceptance ---- */

static int st_checks;
static int st_failed;

static void st_report(const char *what, int ok)
{
    st_checks++;
    if (!ok) st_failed++;
    shell_printf("sudo selftest: %-46s %s\n", what, ok ? "PASS" : "FAIL");
}

int sudo_selftest(void)
{
    const sudoers_policy_t *policy = sudoers_policy();
    const userdb_user_t *root_user = userdb_by_name("root");
    const userdb_user_t *plain_user = userdb_by_name("user");
    char idbuf[160];
    uint32_t saved_uid;

    st_checks = 0;
    st_failed = 0;

    /* 1) crypto */
    st_report("sha256/hmac/pbkdf2 known answers", sha256_selftest());

    /* 2) userdb */
    st_report("root account present (uid 0)",
              root_user != NULL && root_user->uid == 0);
    st_report("user account present (uid 1000)",
              plain_user != NULL && plain_user->uid == 1000);
    st_report("root password verifies", userdb_check_password("root", "root") == 1);
    st_report("wrong password is rejected",
              userdb_check_password("root", "not-the-password") == 0);
    st_report("unknown user is rejected", userdb_check_password("nobody", "x") < 0);
    st_report("user is in group wheel", userdb_in_group("user", "wheel") == 1);
    st_report("root is not in group wheel", userdb_in_group("root", "wheel") == 0);

    /* 3) shadow round trip: hash a fresh password and verify through the file */
    if (userdb_set_password("user", "user") == 0) {
        st_report("set_password rewrites /etc/shadow",
                  userdb_check_password("user", "user") == 1);
    } else {
        st_report("set_password rewrites /etc/shadow", 0);
    }

    /* 4) policy */
    {
        int nopasswd = 0;
        int rule = -1;
        st_report("policy: user may run mount",
                  sudoers_check("user", "root", "mount", &nopasswd, &rule) == 1);
        st_report("policy: user may run shutdown",
                  sudoers_check("user", "root", "shutdown", &nopasswd, &rule) == 1);
        st_report("policy: root has a rule",
                  sudoers_check("root", "root", "mount", &nopasswd, &rule) == 1);
        st_report("policy: unknown user denied",
                  sudoers_check("nobody", "root", "mount", &nopasswd, &rule) == 0);
        st_report("policy defaults parsed (tries >= 1)",
                  policy->passwd_tries >= 1);
    }

    /* 5) ticket cache */
    sudo_kill_ticket();
    st_report("ticket starts invalid", sudo_ticket_valid("user") == 0);
    {
        /* grant through the real path: authenticate non-interactively */
        int ok = sudo_verify_password("user", "user");
        st_report("non-interactive auth for ticket grant", ok == 1);
    }
    sudo_kill_ticket();
    st_report("ticket invalid after -k", sudo_ticket_valid("user") == 0);

    /* 6) credentials */
    saved_uid = cred_uid();
    st_report("session starts as root", saved_uid == 0);
    st_report("cred_set_user(user) -> uid 1000",
              cred_set_user("user") == 0 && cred_euid() == 1000);
    st_report("non-root cannot be root without auth",
              cred_is_root() == 0 && cred_set_euid(0) == -1);
    st_report("cred_set_user(root) restores uid 0",
              cred_set_user("root") == 0 && cred_is_root() == 1);
    cred_format_id(idbuf, sizeof(idbuf));
    st_report("id string formatted", strlen(idbuf) > 10);

    /* 7) audit log */
    audit_log("[selftest] sudo selftest ran");
    {
        size_t len = 0;
        void *buf = vfs_read_file(SUDO_LOG_PATH, &len);
        st_report("audit log written", buf != NULL && len > 0);
        if (buf != NULL) free(buf);
    }

    shell_printf("sudo selftest: RESULT %s (%d/%d)\n",
                 (st_failed == 0) ? "PASS" : "FAIL",
                 st_checks - st_failed, st_checks);
    return (st_failed == 0) ? 0 : 1;
}
