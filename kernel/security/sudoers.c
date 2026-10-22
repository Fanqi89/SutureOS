/* SutureOS: /etc/sudoers parser. See sudoers.h for the supported syntax. */
#include "sudoers.h"
#include "userdb.h"

#include <stitch/console.h>
#include <stitch/string.h>
#include "heap.h"
#include "vfs.h"

static sudoers_policy_t g_policy;
static char g_summary[160];

/* ---- string helpers (no libc: keep everything local & explicit) ---- */

static void trim(char *s)
{
    char *p = s;
    size_t len;

    while (*p == ' ' || *p == '\t') p++;
    if (p != s) {
        size_t n = strlen(p);
        memmove(s, p, n + 1);
    }
    len = strlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' ||
                       s[len - 1] == '\r')) {
        s[--len] = '\0';
    }
}

static int starts_with(const char *s, const char *prefix)
{
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

/* Split "a,b , c" into up to max items, trimming each. */
static int split_list(char *s, char items[][SUDOERS_CMD_LEN], int max)
{
    int n = 0;
    while (s != NULL && *s != '\0' && n < max) {
        char *comma = s;
        char *dst = items[n];
        size_t len;

        while (*comma != '\0' && *comma != ',') comma++;
        len = (size_t)(comma - s);
        if (len >= SUDOERS_CMD_LEN) len = SUDOERS_CMD_LEN - 1;
        memcpy(dst, s, len);
        dst[len] = '\0';
        trim(dst);
        if (dst[0] != '\0') n++;
        if (*comma == ',') comma++;
        s = (*comma == '\0') ? NULL : comma;
    }
    return n;
}

/* Extract the inside of "(...)" and leave the remainder of the line in *rest. */
static int parse_runas(char *line, char *runas_out, size_t runas_len, char **rest)
{
    char *open = line;
    char *close;

    while (*open != '\0' && *open != '(') open++;
    if (*open != '(') return -1;
    close = open + 1;
    while (*close != '\0' && *close != ')') close++;
    if (*close != ')') return -1;

    {
        char inner[SUDOERS_WHO_LEN * 2];
        size_t n = (size_t)(close - (open + 1));
        if (n >= sizeof(inner)) n = sizeof(inner) - 1;
        memcpy(inner, open + 1, n);
        inner[n] = '\0';
        /* "ALL:ALL" / "root:wheel" / "ALL (root)" -- keep the first field */
        for (size_t i = 0; inner[i] != '\0'; i++) {
            if (inner[i] == ':') { inner[i] = '\0'; break; }
        }
        trim(inner);
        if (n == 0 || inner[0] == '\0') {
            memcpy(runas_out, "ALL", 4);
            if (runas_len < 4) runas_out[runas_len - 1] = '\0';
        } else {
            size_t copy = strlen(inner);
            if (copy >= runas_len) copy = runas_len - 1;
            memcpy(runas_out, inner, copy);
            runas_out[copy] = '\0';
        }
    }
    *rest = close + 1;
    return 0;
}

/* ---- Defaults handling ---- */

static void handle_defaults(char *line)
{
    char *p = line + strlen("Defaults");
    char key[48];
    char *eq;
    size_t klen;

    while (*p == ' ' || *p == '\t') p++;
    trim(p);

    eq = p;
    while (*eq != '\0' && *eq != '=') eq++;
    if (*eq != '=') {
        /* flags like !requiretty are accepted and ignored */
        return;
    }
    klen = (size_t)(eq - p);
    if (klen >= sizeof(key)) klen = sizeof(key) - 1;
    memcpy(key, p, klen);
    key[klen] = '\0';
    trim(key);
    eq++;
    trim(eq);

    if (strcmp(key, "timestamp_timeout") == 0) {
        uint32_t v = 0;
        int neg = (*eq == '-');
        const char *d = neg ? eq + 1 : eq;
        while (*d >= '0' && *d <= '9') { v = v * 10u + (uint32_t)(*d - '0'); d++; }
        g_policy.timestamp_timeout = neg ? 0u : v;   /* negative = always ask */
    } else if (strcmp(key, "passwd_tries") == 0) {
        uint32_t v = 0;
        while (*eq >= '0' && *eq <= '9') { v = v * 10u + (uint32_t)(*eq - '0'); eq++; }
        if (v > 0) g_policy.passwd_tries = v;
    }
    /* other Defaults are accepted and ignored on purpose */
}

/* ---- one policy line ---- */

static void parse_rule_line(char *line)
{
    char who[SUDOERS_WHO_LEN];
    char runas[SUDOERS_WHO_LEN];
    char *rest = NULL;
    char *cmnd_part;
    char *p;
    sudoers_rule_t *rule;
    int nopasswd = 0;
    int count;

    if (g_policy.rule_count >= SUDOERS_MAX_RULES) {
        g_policy.lines_bad++;
        return;
    }

    /* who (field 1) */
    p = line;
    {
        char *sp = p;
        while (*sp != '\0' && *sp != ' ' && *sp != '\t') sp++;
        if (*sp == '\0') { g_policy.lines_bad++; return; }
        *sp = '\0';
        snprintf(who, sizeof(who), "%s", p);
        p = sp + 1;
    }

    /* skip the host field ("ALL") */
    while (*p == ' ' || *p == '\t') p++;
    {
        char *sp = p;
        while (*sp != '\0' && *sp != ' ' && *sp != '\t') sp++;
        if (*sp != '\0') { *sp = '\0'; p = sp + 1; }
        else { p = sp; }
    }
    while (*p == ' ' || *p == '\t') p++;

    /* optional "(runas:group)" */
    if (*p == '(') {
        if (parse_runas(p, runas, sizeof(runas), &rest) != 0) {
            g_policy.lines_bad++;
            return;
        }
        p = rest;
    } else {
        snprintf(runas, sizeof(runas), "ALL");
    }
    while (*p == ' ' || *p == '\t') p++;

    /* optional NOPASSWD: / PASSWD: tag */
    if (starts_with(p, "NOPASSWD:")) {
        nopasswd = 1;
        p += strlen("NOPASSWD:");
    } else if (starts_with(p, "PASSWD:")) {
        p += strlen("PASSWD:");
    }
    while (*p == ' ' || *p == '\t') p++;
    cmnd_part = p;
    if (*cmnd_part == '\0') { g_policy.lines_bad++; return; }

    rule = &g_policy.rules[g_policy.rule_count];
    memset(rule, 0, sizeof(*rule));
    snprintf(rule->who, sizeof(rule->who), "%s", who);
    rule->is_group = (who[0] == '%');
    if (rule->is_group) {
        /* strip '%' for matching */
        memmove(rule->who, rule->who + 1, strlen(rule->who));
    }
    snprintf(rule->runas, sizeof(rule->runas), "%s", runas);
    rule->nopasswd = nopasswd;

    count = split_list(cmnd_part, rule->cmnds, SUDOERS_MAX_CMNDS);
    rule->cmnd_count = count;
    if (count == 0) { g_policy.lines_bad++; return; }

    g_policy.rule_count++;
}

int sudoers_init(void)
{
    size_t len = 0;
    void *buf;
    char *cursor;

    memset(&g_policy, 0, sizeof(g_policy));
    g_policy.timestamp_timeout = 15;      /* sudo's default */
    g_policy.passwd_tries = 3;            /* sudo's default */

    buf = vfs_read_file(SUDOERS_PATH, &len);
    if (buf == NULL) {
        console_printf("[sudoers] %s missing - default deny for everyone\n",
                       SUDOERS_PATH);
        snprintf(g_summary, sizeof(g_summary), "no policy file");
        return -1;
    }

    cursor = (char *)buf;
    while (cursor != NULL && *cursor != '\0') {
        char *line = cursor;
        char *nl = cursor;
        char *probe;

        while (*nl != '\0' && *nl != '\n') nl++;
        if (*nl == '\n') {
            *nl = '\0';
            cursor = nl + 1;
        } else {
            cursor = NULL;
        }
        g_policy.lines_seen++;

        /* strip trailing '\r' and leading whitespace; skip blanks/comments */
        trim(line);
        probe = line;
        while (*probe == ' ' || *probe == '\t') probe++;
        if (*probe == '\0' || *probe == '#') continue;

        if (starts_with(probe, "Defaults")) {
            handle_defaults(probe);
        } else if (starts_with(probe, "User_Alias") ||
                   starts_with(probe, "Cmnd_Alias") ||
                   starts_with(probe, "Host_Alias") ||
                   starts_with(probe, "Runas_Alias")) {
            /* aliases are not supported: reported once via lines_bad */
            g_policy.lines_bad++;
        } else {
            parse_rule_line(probe);
        }
    }
    free(buf);

    snprintf(g_summary, sizeof(g_summary),
             "%u rules, timeout=%umin, tries=%u",
             (unsigned)g_policy.rule_count,
             (unsigned)g_policy.timestamp_timeout,
             (unsigned)g_policy.passwd_tries);
    return 0;
}

const sudoers_policy_t *sudoers_policy(void) { return &g_policy; }

const char *sudoers_summary(void) { return g_summary; }

/* ---- matching ---- */

/* Compare one cmnd entry: "ALL", an exact path, a directory prefix
 * ("/usr/bin/" + "*"), or a bare command name. */
static int cmnd_matches(const char *spec, const char *cmd)
{
    size_t slen;

    if (spec[0] == '\0') return 0;
    if (strcmp(spec, "ALL") == 0) return 1;

    slen = strlen(spec);
    if (slen > 2 && spec[slen - 1] == '*' && spec[slen - 2] == '/') {
        /* directory prefix rule: "/usr/bin/mount/" + "*" */
        return strncmp(cmd, spec, slen - 1) == 0;
    }
    if (strcmp(spec, cmd) == 0) return 1;

    /* also match on the basename so a sudoers written for Linux paths
     * ("/usr/bin/mount") still permits the shell's "mount" */
    {
        const char *base = strrchr(cmd, '/');
        const char *specbase = strrchr(spec, '/');
        base = (base != NULL) ? base + 1 : cmd;
        specbase = (specbase != NULL) ? specbase + 1 : spec;
        if (strcmp(base, specbase) == 0) return 1;
    }
    return 0;
}

int sudoers_check(const char *user, const char *runas, const char *cmd,
                  int *nopasswd_out, int *matched_rule)
{
    int allowed = 0;
    int nopasswd = 0;
    int first_rule = -1;

    if (nopasswd_out) *nopasswd_out = 0;
    if (matched_rule) *matched_rule = -1;
    if (user == NULL || cmd == NULL) return 0;

    for (uint32_t i = 0; i < g_policy.rule_count; i++) {
        const sudoers_rule_t *r = &g_policy.rules[i];
        int who_ok;

        if (r->is_group) {
            who_ok = userdb_in_group(user, r->who);
        } else {
            who_ok = (strcmp(r->who, user) == 0);
        }
        if (!who_ok) continue;
        if (strcmp(r->runas, "ALL") != 0 && runas != NULL &&
            strcmp(r->runas, runas) != 0) {
            continue;
        }
        for (int c = 0; c < r->cmnd_count; c++) {
            if (cmnd_matches(r->cmnds[c], cmd)) {
                allowed = 1;
                if (r->nopasswd) nopasswd = 1;
                if (first_rule < 0) first_rule = (int)i;
                break;
            }
        }
    }

    if (allowed) {
        if (nopasswd_out) *nopasswd_out = nopasswd;
        if (matched_rule) *matched_rule = first_rule;
        return 1;
    }
    return 0;
}
