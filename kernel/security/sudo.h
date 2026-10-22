/* SutureOS: `sudo` - a port of Linux sudo's behaviour onto this kernel.
 *
 * WHAT THIS IS
 *   Linux sudo is a setuid userspace program built on PAM/crypt(3)/termios. None
 *   of those exist yet here (no userland, no libc, 22 syscalls), so a literal
 *   binary port cannot run. What is ported instead is sudo's *contract*, inside
 *   the kernel where the shell lives:
 *
 *     - identities from /etc/passwd + groups from /etc/group (userdb.c)
 *     - password verification against /etc/shadow (salted PBKDF2-HMAC-SHA256)
 *     - a policy language parsed from /etc/sudoers (sudoers.c):  user/%group
 *       HOST=(RUNAS:GROUP) [NOPASSWD:] CMND, Defaults timestamp_timeout /
 *       passwd_tries
 *     - sudo's ticket/timestamp cache: one authentication unlocks a window of
 *       `timestamp_timeout` minutes, `-k` invalidates it, `-v` refreshes it
 *     - sudo's UX strings, exit codes and failure accounting: 3 tries,
 *       "Sorry, try again.", "sudo: N incorrect password attempts",
 *       "user is not in the sudoers file.  This incident will be reported."
 *     - sudo's audit trail: a line per invocation appended to /var/log/sudo.log
 *     - euid raise/lower around the command (creds.c), restored afterwards so a
 *       failed command cannot leave the session privileged
 *
 * WHAT THIS IS NOT
 *   A security boundary. Every task still runs in ring 0 and the shell is the
 *   only "user", so sudo here enforces *policy* and produces an audit trail; it
 *   cannot stop kernel code from doing whatever it likes. It becomes a real
 *   boundary once ring-3 userland lands (the cred API already has Linux's
 *   uid/euid/suid shape so that switch is mechanical).
 */
#ifndef STITCH_SUDO_H
#define STITCH_SUDO_H

#include <stitch/types.h>

#define SUDO_HOST        "sutureos"
#define SUDO_LOG_PATH    "/var/log/sudo.log"
#define SUDO_MAX_CMDLINE 256

typedef struct sudo_ticket {
    char     user[32];
    uint32_t expiry_s;      /* uptime second at which the ticket dies, 0 = none */
    int      valid;
} sudo_ticket_t;

/* Boot-time init: sha256 self-test, userdb, sudoers, ticket table.
 * Returns 0 on success. Logs one line per component like the rest of the boot. */
int sudo_init(void);

/* The `sudo` command itself (shell_cmd_sudo forwards here). */
int sudo_main(int argc, char **argv);

/* `su` / login helper: authenticate `target` (root when NULL) and adopt its ids.
 * Password is read from the console when needed. Returns 0 on success. */
int sudo_su(int argc, char **argv);

/* Invalidate the cached ticket (used by `sudo -k` and by `su`). */
void sudo_kill_ticket(void);

/* 1 when `user` currently holds a valid timestamp ticket. */
int sudo_ticket_valid(const char *user);

/* Print `prompt`, then read a line with echo suppressed. Returns the number of
 * characters read, or -1 on Ctrl-C. Used by `passwd` and `su`. */
int sudo_prompt_password(const char *prompt, char *out, size_t outlen);

/* Non-interactive password check used by the self test and by `su`:
 * returns 1 = ok, 0 = wrong, -1 = no such user. */
int sudo_verify_password(const char *user, const char *password);

/* Automated acceptance: hashes, policy, tickets, credential switching,
 * audit log. Prints "sudo selftest: ... PASS/FAIL" lines and a final RESULT
 * line; returns 0 when everything passed. This is what CI asserts on. */
int sudo_selftest(void);

#endif /* STITCH_SUDO_H */
