/* SutureOS: login / identity switching. See login.h for the password semantics
 * and the "who may switch" rules; this file only implements them.
 */
#include "login.h"

#include <stitch/console.h>
#include <stitch/string.h>
#include "creds.h"
#include "userdb.h"
#include "sudo.h"
#include "shell.h"
#include "vfs.h"

/* The last account that adopted this session; used by the GUI to show "current
 * user" without a userdb scan and kept in sync by login_switch(). */
static char g_session_user[USERDB_NAME_LEN];
static uint32_t g_session_uid;

/* ============================ state queries ============================ */

const char *login_current_user(void)
{
    const char *by_cred = cred_username();
    if (by_cred != NULL && by_cred[0] != '\0' && strcmp(by_cred, "-") != 0) {
        return by_cred;
    }
    return (g_session_user[0] != '\0') ? g_session_user : "-";
}

uint32_t login_current_uid(void)
{
    return cred_uid();
}

int login_requires_password(const char *user)
{
    return userdb_pwstate(user) == USERDB_PW_PASSWORD;
}

const char *login_error_text(int rc)
{
    switch (rc) {
        case LOGIN_OK:          return "ok";
        case LOGIN_ERR_NOUSER:  return "no such user";
        case LOGIN_ERR_LOCKED:  return "account is locked";
        case LOGIN_ERR_BADPASS: return "incorrect password";
        case LOGIN_ERR_INVALID: return "login aborted";
        default:                return "login failed";
    }
}

/* ============================ authentication ============================ */

int login_authenticate(const char *user, const char *password)
{
    const userdb_user_t *u;

    if (user == NULL || user[0] == '\0') return LOGIN_ERR_NOUSER;
    u = userdb_by_name(user);
    if (u == NULL) return LOGIN_ERR_NOUSER;

    switch (userdb_pwstate(user)) {
        case USERDB_PW_NONE:
            /* Explicitly passwordless: no prompt, no password to check. */
            return LOGIN_OK;
        case USERDB_PW_LOCKED:
            return LOGIN_ERR_LOCKED;
        case USERDB_PW_PASSWORD:
            if (password == NULL || password[0] == '\0') return LOGIN_ERR_BADPASS;
            return (userdb_check_password(user, password) == 1) ? LOGIN_OK
                                                                : LOGIN_ERR_BADPASS;
        default:
            return LOGIN_ERR_NOUSER;
    }
}

int login_switch(const char *user)
{
    const userdb_user_t *u;

    if (user == NULL || user[0] == '\0') return LOGIN_ERR_NOUSER;
    u = userdb_by_name(user);
    if (u == NULL) return LOGIN_ERR_NOUSER;
    if (cred_set_user(user) != 0) return LOGIN_ERR_NOUSER;

    /* A new identity must not inherit the previous user's cached sudo ticket. */
    sudo_kill_ticket();
    snprintf(g_session_user, sizeof(g_session_user), "%s", user);
    g_session_uid = u->uid;
    shell_refresh_prompt();
    console_printf("[login] session identity = %s (uid=%u)\n", user,
                   (uint64_t)u->uid);
    return LOGIN_OK;
}

int login_switch_authenticated(const char *user, const char *password)
{
    int rc = login_authenticate(user, password);
    if (rc != LOGIN_OK) return rc;
    return login_switch(user);
}

/* ============================ console input ============================ */

int login_read_line(const char *prompt, char *out, size_t outlen, int hidden)
{
    extern int console_getc(void);
    size_t len = 0;

    if (out == NULL || outlen == 0) return -1;
    out[0] = '\0';
    if (prompt != NULL) shell_puts(prompt);

    for (;;) {
        const int c = console_getc();
        if (c < 0) {
            /* Idle wait with a bounded pause spin: the shell path must not call
             * sched_yield (stale EEVDF context, see shell_run()). */
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
        if (c == 0x03) {                      /* Ctrl-C aborts the prompt */
            out[0] = '\0';
            shell_puts("^C\n");
            return -1;
        }
        if (len + 1 < outlen) {
            out[len++] = (char)c;
            if (!hidden) shell_putc((char)c);
        }
    }
}

/* ============================ shell: login ============================ */

/* Shared body of login: adopt an identity after authenticating. */
static int do_login(const char *target)
{
    const userdb_user_t *u;
    char password[96];
    int rc;

    u = userdb_by_name(target);
    if (u == NULL) {
        shell_printf("login: user %s does not exist\n", target);
        return 1;
    }
    if (userdb_pwstate(target) == USERDB_PW_LOCKED) {
        shell_printf("login: account %s is locked (! in /etc/shadow)\n", target);
        return 1;
    }

    /* A root session may adopt any account without a password (an installer
     * booting straight into the root console must be able to set users up). */
    if (cred_uid() != 0 && login_requires_password(target)) {
        password[0] = '\0';
        if (login_read_line("Password: ", password, sizeof(password), 1) < 0) {
            shell_puts("login: aborted\n");
            return 1;
        }
        rc = login_authenticate(target, password);
        memset(password, 0, sizeof(password));
        if (rc != LOGIN_OK) {
            /* Deliberately generic, and the previous identity is untouched. */
            shell_puts("Login incorrect\n");
            console_printf("[login] failed login for %s (%s)\n", target,
                           login_error_text(rc));
            return 1;
        }
    }

    if (login_switch(target) != LOGIN_OK) {
        shell_printf("login: cannot adopt %s\n", target);
        return 1;
    }
    shell_printf("Welcome to SutureOS, %s (uid=%u, home %s)\n", target,
                 (uint64_t)cred_uid(), u->home);
    console_printf("[login] %s logged in (uid=%u)\n", target, (uint64_t)cred_uid());
    return 0;
}

int login_main(int argc, char **argv)
{
    char who[USERDB_NAME_LEN];

    if (argc >= 2) return do_login(argv[1]);

    who[0] = '\0';
    if (login_read_line("login: ", who, sizeof(who), 0) < 0) {
        shell_puts("login: aborted\n");
        return 1;
    }
    if (who[0] == '\0') return 1;            /* empty user name: do nothing */
    return do_login(who);
}

/* ============================ shell: su ============================ */

int login_su(int argc, char **argv)
{
    const char *target = "root";
    int login_shell = 0;
    int argi = 1;
    const userdb_user_t *u;
    char password[96];
    int rc;

    /* `su -` / `su -l` = login shell; `su - alice` also works. */
    if (argi < argc && argv[argi][0] == '-' &&
        (argv[argi][1] == '\0' || strcmp(argv[argi], "-l") == 0)) {
        login_shell = 1;
        argi++;
    }
    if (argi < argc) target = argv[argi];

    u = userdb_by_name(target);
    if (u == NULL) {
        shell_printf("su: user %s does not exist\n", target);
        return 1;
    }

    if (userdb_pwstate(target) == USERDB_PW_LOCKED) {
        shell_printf("su: account %s is locked (! in /etc/shadow)\n", target);
        return 1;
    }

    /* Linux: a root session never needs a password for su. */
    if (cred_uid() != 0 && login_requires_password(target)) {
        shell_puts("Password: ");
        if (login_read_line(NULL, password, sizeof(password), 1) < 0) {
            shell_puts("su: Authentication failure\n");
            return 1;
        }
        rc = login_authenticate(target, password);
        memset(password, 0, sizeof(password));
        if (rc != LOGIN_OK) {
            shell_puts("su: Authentication failure\n");
            console_printf("[login] su %s failed (%s)\n", target,
                           login_error_text(rc));
            return 1;
        }
    }

    if (login_switch(target) != LOGIN_OK) {
        shell_printf("su: cannot adopt %s\n", target);
        return 1;
    }
    if (login_shell) {
        vfs_set_cwd(u->home);                /* `su -`: start in the home dir */
        shell_printf("su: login shell for %s, cwd %s\n", target, u->home);
    } else {
        shell_printf("su: switched to %s (uid=%u)\n", target, (uint64_t)cred_uid());
    }
    console_printf("[login] su -> %s (uid=%u, login_shell=%d)\n", target,
                   (uint64_t)cred_uid(), login_shell);
    return 0;
}
