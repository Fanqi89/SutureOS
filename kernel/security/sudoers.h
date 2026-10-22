/* SutureOS: /etc/sudoers policy parser (Linux sudo syntax subset).
 *
 * Supported (this is what makes `sudo` behave like the real thing):
 *
 *     user    ALL=(ALL:ALL) ALL
 *     user    ALL=(root) /sbin/mount
 *     %group  ALL=(ALL:ALL) NOPASSWD: ALL
 *     user    ALL=(ALL:ALL) NOPASSWD: /usr/bin/mount, /usr/bin/umount
 *     Defaults timestamp_timeout=15      (minutes, 0 = always ask)
 *     Defaults passwd_tries=3
 *     # comment
 *
 * Host part and the runas-group part are parsed and ignored (there is one host
 * and groups only gate the %group match). Command matching is:
 *   - "ALL"                          any command
 *   - "/path" or "name"              exact match against the command name or the
 *                                    full argv[0] (both forms are accepted so a
 *                                    sudoers written for Linux paths still works)
 *   - "/path/" + "*"                  prefix match
 *
 * Anything the parser does not understand is reported and ignored; a policy with
 * zero rules for a user simply denies, which is sudo's default-deny behaviour.
 */
#ifndef STITCH_SUDOERS_H
#define STITCH_SUDOERS_H

#include <stitch/types.h>

#define SUDOERS_PATH        "/etc/sudoers"
#define SUDOERS_MAX_RULES   16
#define SUDOERS_MAX_CMNDS   4
#define SUDOERS_WHO_LEN     32
#define SUDOERS_CMD_LEN     64

typedef struct sudoers_rule {
    char who[SUDOERS_WHO_LEN];      /* username, or group name when is_group   */
    int  is_group;
    char runas[SUDOERS_WHO_LEN];    /* target user, "ALL" = any                */
    int  nopasswd;
    char cmnds[SUDOERS_MAX_CMNDS][SUDOERS_CMD_LEN];
    int  cmnd_count;
} sudoers_rule_t;

typedef struct sudoers_policy {
    sudoers_rule_t rules[SUDOERS_MAX_RULES];
    uint32_t       rule_count;
    uint32_t       timestamp_timeout;   /* minutes                                  */
    uint32_t       passwd_tries;        /* password attempts                        */
    /* parse diagnostics, shown by `sudo -l -v` and the boot log */
    uint32_t       lines_seen;
    uint32_t       lines_bad;
} sudoers_policy_t;

/* Parse /etc/sudoers (seeded by userdb when missing). Returns 0 on success. */
int sudoers_init(void);

const sudoers_policy_t *sudoers_policy(void);

/* Does `user` (a member of any of its groups) have a rule permitting `cmd`
 * (argv[0] basename or full path) when running as `runas`?
 *   returns 1 = allowed, 0 = denied
 *   *nopasswd_out = rule carried NOPASSWD:, or the command is allowed for root
 *   *matched_rule = index of the matching rule (or -1)                        */
int sudoers_check(const char *user, const char *runas, const char *cmd,
                  int *nopasswd_out, int *matched_rule);

/* Human readable dump for `sudo -l` (prints through shell_puts by the caller). */
const char *sudoers_summary(void);

#endif /* STITCH_SUDOERS_H */
