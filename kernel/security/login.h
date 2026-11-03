/* SutureOS: login / identity switching (multiuser batch, v0.31.0).
 *
 * This is the session layer on top of kernel/security/userdb.c (accounts +
 * password hashes) and kernel/security/creds.c (per-task uid/euid). It defines
 * one place where "can this identity be adopted, and what does an empty or
 * locked password mean" is decided, so the shell `login`/`su` commands and the
 * GUI "Switch User" window cannot drift apart.
 *
 * PASSWORD SEMANTICS (documented in docs/multiuser.md, mirrored from userdb.h)
 *   shadow field `$sutureos$pbkdf2-sha256$...` -> a password is required
 *   shadow field empty ("name::0")             -> NO password required; login,
 *                                                 su and the GUI switch accept it
 *                                                 without a prompt. This is the
 *                                                 state an installer leaves when
 *                                                 it does not set a root password.
 *   shadow field "!" / "*"                     -> locked: every password login
 *                                                 is refused (useradd's "no
 *                                                 password entered" state).
 *   no shadow line at all                      -> treated as locked.
 *
 * WHO MAY SWITCH
 *   - a root session may `su`/`login` to anyone without a password (Linux does
 *     the same for su; `login` as root also skips the password prompt),
 *   - a non-root session must authenticate unless the target is passwordless,
 *   - a failed authentication never changes the current identity (the shell keeps
 *     the previous user), which is what the acceptance script asserts.
 *
 * HONEST BOUNDARY: every task still runs in ring 0 (see creds.h), so this is the
 * policy layer, not a hardware boundary.
 */
#ifndef STITCH_LOGIN_H
#define STITCH_LOGIN_H

#include <stitch/types.h>

#define LOGIN_OK            0
#define LOGIN_ERR_NOUSER   -1   /* no such account                       */
#define LOGIN_ERR_LOCKED   -2   /* `!` in /etc/shadow: password refused  */
#define LOGIN_ERR_BADPASS  -3   /* wrong password                        */
#define LOGIN_ERR_INVALID  -4   /* bad arguments / cancelled by the user */

/* Can `user` be adopted with this password? LOGIN_OK / LOGIN_ERR_*.
 * Honors the three password states above (passwordless accounts accept any
 * password argument, including NULL; locked accounts never succeed). */
int login_authenticate(const char *user, const char *password);

/* Adopt `user` as the session identity WITHOUT authenticating (the caller has
 * already done that, or is root). Refreshes the shell prompt and drops the sudo
 * ticket so the new user cannot inherit root's timestamp. */
int login_switch(const char *user);

/* authenticate + switch in one call. */
int login_switch_authenticated(const char *user, const char *password);

/* 1 = this account needs a password prompt, 0 = passwordless or unknown. */
int login_requires_password(const char *user);

/* Current session identity ("-" when the uid has no account). */
const char *login_current_user(void);
uint32_t    login_current_uid(void);

/* Human readable message for a LOGIN_ERR_* code. */
const char *login_error_text(int rc);

/* Console line reader used by the interactive commands: `hidden` masks the input
 * (no echo at all, like sudo/su). Returns the length, or -1 on Ctrl-C. */
int login_read_line(const char *prompt, char *out, size_t outlen, int hidden);

/* Shell commands. */
int login_main(int argc, char **argv);   /* login [user]  */
int login_su(int argc, char **argv);     /* su [-] [user] */

#endif /* STITCH_LOGIN_H */
