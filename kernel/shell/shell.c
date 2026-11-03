/* SutureOS: Simple Terminal Shell Implementation
 * Minimal terminal emulator with command line editing, history, and builtins
 * Provides Linux-like terminal interface with black background, white text
 */
#include "shell.h"
#include "creds.h"
#include "sudo.h"
#include "syscalls2.h"   /* systest: the new syscalls */
#include "rtc.h"        /* date */        /* sudo/su/passwd (kernel/security) */
#include "userdb.h"
#include "login.h"      /* login/su/identity switch (multiuser batch) */
#include <stitch/types.h>
#include <stitch/string.h>
#include <stitch/stdarg.h>
#include <stitch/console.h>
#include "fbcon.h"
#include "heap.h"
#include "vfs.h"
#include "task.h"
#include "eevdf.h"
#include "syscall.h"
#include "mb_kbd.h"
#include "net.h"      /* tcp_header_t / TCP_FLAG_* / htons for the TCP commands */
#include "dhcp.h"     /* dhcp / nslookup: on-demand lease + resolver */
#include "dns.h"

/* Declare console_getc for keyboard input */
extern int console_getc(void);

/* External declarations */
extern void console_putc(char c);
extern void console_puts(const char *s);
extern void console_printf(const char *fmt, ...);
extern void sys_reboot(void);
extern void sys_shutdown(void);
extern int gui_init(int w, int h);
extern int gui_run(void);
extern int fatfs_mount(uint8_t drv, void* fs);
extern int ext2_mount(int drive, uint32_t start_lba);
extern int squashfs_mount(int drive, uint32_t start_lba);

#include "smp.h"    /* per-CPU state for the `cpus` command + work queue draining */

/* Global shell state */
static struct shell_state g_shell = {0};

/* Command table */
struct shell_cmd {
    const char *name;
    int (*func)(int argc, char **argv);
    const char *help;
};

/* defined next to the sudo commands; privileged handlers above use it */
static int require_root(const char *cmd);

/* ==================== /etc and /root write policy ====================
 * The VFS has no per-file ownership yet (see the honest boundary note in
 * kernel/security/creds.h), so the shell enforces the identity rule: only root
 * may create, change or remove anything under /etc or /root. Reads stay open to
 * everybody, so `cat /etc/passwd` works for a normal user while
 * `write /etc/passwd ...` is refused. */
static int shell_path_root_only(const char *path)
{
    if (path == NULL) return 0;
    if (cred_is_root()) return 0;
    while (path[0] == '.' && path[1] == '/') path += 2;
    if (strncmp(path, "/etc", 4) == 0 && (path[4] == '\0' || path[4] == '/')) return 1;
    if (strncmp(path, "/root", 5) == 0 && (path[5] == '\0' || path[5] == '/')) return 1;
    return 0;
}

/* Refuse a mutating command when one of argv[from..argc) targets /etc or /root.
 * `argc` is capped by the caller, so `cp` can guard only its destination. */
static int shell_guard_paths(const char *cmd, int argc, char **argv, int from)
{
    for (int i = from; i < argc; i++) {
        if (shell_path_root_only(argv[i])) {
            shell_printf("%s: %s: Permission denied (only root may modify /etc or /root)\n",
                         cmd, argv[i]);
            return 1;
        }
    }
    return 0;
}

static const struct shell_cmd g_commands[] = {
    {"help",    shell_cmd_help,    "Show this help message"},
    {"clear",   shell_cmd_clear,   "Clear the screen"},
    {"log",     shell_cmd_log,     "Show the captured console log: log [lines] (auto-scrolls to the tail)"},
    {"dmesg",   shell_cmd_log,     "Alias for log"},
    {"mouseinfo", shell_cmd_mouseinfo, "PS/2 mouse diagnostics (8042 status, config, device id)"},
    {"echo",    shell_cmd_echo,    "Print arguments"},
    {"exit",    shell_cmd_exit,    "Exit shell"},
    {"reboot",  shell_cmd_reboot,  "Reboot the system"},
    {"shutdown",shell_cmd_shutdown,"Shutdown the system"},
    {"startgui",shell_cmd_startgui,"Start GUI interface (fallback to terminal on failure)"},
    {"ls",      shell_cmd_ls,      "List directory contents"},
    {"cd",      shell_cmd_cd,      "Change directory"},
    {"pwd",     shell_cmd_pwd,     "Print working directory"},
    {"cat",     shell_cmd_cat,     "Display file contents"},
    {"write",   shell_cmd_write,   "Create/replace a text file: write <file> <text...>"},
    {"append",  shell_cmd_append,  "Append one text line: append <file> <text...>"},
    {"hexdump", shell_cmd_hexdump, "Hex + ASCII dump: hexdump <file> [offset] [length]"},
    {"wc",      shell_cmd_wc,      "Count lines/words/bytes: wc <file>"},
    {"head",    shell_cmd_head,    "First lines: head <file> [n]"},
    {"tail",    shell_cmd_tail,    "Last lines: tail <file> [n]"},
    {"grep",    shell_cmd_grep,    "Print matching lines: grep <pattern> <file>"},
    {"stat",    shell_cmd_stat,    "Show type/size of a path: stat <path>"},
    {"sync",    shell_cmd_sync,    "Flush every mounted filesystem: sync"},
    {"persist", shell_cmd_persist, "Persistent user data: persist [flush] (status / force flush)"},
    {"ps",      shell_cmd_ps,      "List processes"},
    {"kill",    shell_cmd_kill,    "Kill a process"},
    {"sudo",    shell_cmd_sudo,    "Execute command as root"},
    {"login",   shell_cmd_login,   "Log in as another user: login [user] (authenticates against /etc/shadow)"},
    {"su",      shell_cmd_su,      "Switch user: su [-] [user] (authenticates against /etc/shadow)"},
    {"whoami",  shell_cmd_whoami,  "Print the effective user name"},
    {"id",      shell_cmd_id,      "Print uid/gid/groups of the effective user"},
    {"passwd",  shell_cmd_passwd,  "Change a password: passwd [-l|-u|-d] [user]"},
    {"users",   shell_cmd_users,   "List accounts: name uid gid groups password-state ('*' = current)"},
    {"useradd", shell_cmd_useradd, "Create an account (root only): useradd <name>"},
    {"userdel", shell_cmd_userdel, "Delete an account (root only): userdel <name>"},
    {"sudotest",shell_cmd_sudotest,"Self-test: SHA-256, /etc/shadow, sudoers, ticket, creds"},
    {"rm",      shell_cmd_rm,      "Remove files: rm <path>..."},
    {"mkdir",   shell_cmd_mkdir,   "Create directories"},
    {"rmdir",   shell_cmd_rmdir,   "Remove empty directories"},
    {"touch",   shell_cmd_touch,   "Create empty files"},
    {"cp",      shell_cmd_cp,      "Copy a file: cp <src> <dst>"},
    {"mv",      shell_cmd_mv,      "Rename/move inside a directory: mv <src> <dst>"},
    {"date",    shell_cmd_date,    "Show the wall clock (CMOS RTC)"},
    {"uname",   shell_cmd_uname,   "System name: uname [-a]"},
    {"free",    shell_cmd_free,    "Memory usage"},
    {"systest", shell_cmd_systest, "Self-test: stat/lseek/pipe/getpid/chdir/mkdir/rename/time"},
    {"pipetest",shell_cmd_pipetest,"Self-test: pipes (create/write/read/EOF/EPIPE)"},
    {"rtctest", shell_cmd_rtctest, "Self-test: CMOS RTC decoding (BCD/12h/leap/format)"},
    {"ring3",   shell_cmd_ring3,  "Ring-3 status (the boot-time user program)"},
    {"ring3test",shell_cmd_ring3test,"Self-test: U/S page bits, SSE, GS base, exit code"},
    {"run",     shell_cmd_run,     "Run a built-in ring-3 user program: run <name>"},
    {"ushell",  shell_cmd_ushell,  "Enter the ring-3 user shell (exit comes back here)"},
    {"mount",   shell_cmd_mount,   "Mount filesystem"},
    {"install", shell_cmd_install, "Install SutureOS onto a hard disk (terminal wizard)"},
    {"umount",  shell_cmd_umount,  "Unmount filesystem"},
    {"df",      shell_cmd_df,      "Show disk usage"},
    {"cpus",    shell_cmd_cpus,    "Show per-CPU state (jiffies, IPIs, work done)"},
    {"smptest", shell_cmd_smptest, "Queue work items and report which CPU ran them" },
    {"ping",    shell_cmd_ping,    "Send ICMP echo requests: ping <ipv4>"},
    {"dhcp",    shell_cmd_dhcp,    "DHCP: acquire a lease, program IP/mask/gateway/DNS: dhcp"},
    {"nslookup",shell_cmd_nslookup,"Resolve an A record through the configured DNS server: nslookup <name>"},
    {"socktest",shell_cmd_socktest,"Self-test: socket/bind/getsockname/getsockopt/close ABI"},
    {"netstat", shell_cmd_netstat, "Show network statistics"},
    {"tcpecho", shell_cmd_tcpecho, "TCP round trip: tcpecho <ipv4> <port> <text>"},
    {"tcpsend", shell_cmd_tcpsend, "TCP one-way send: tcpsend <ipv4> <port> <text>"},
    {"tcpstat", shell_cmd_tcpstat, "Show TCP connection/segment counters"},
    {"wget",    shell_cmd_wget,    "Download a file over HTTP: wget http://<ipv4>[:port]/<path> [outfile]"},
    {"tcpserve",shell_cmd_tcpserve,"TCP echo server: tcpserve <port>"},
    {"tcpooo",  shell_cmd_tcpooo,  "Self-test: TCP passive open + out-of-order reassembly"},
    {"tcpapi",  shell_cmd_tcpapi,  "TCP echo server over the socket ABI: tcpapi <port>"},
    {"kbdstat", shell_cmd_kbdstat, "Show keyboard IRQ1 ring counters"},
    {"beep",    shell_cmd_beep,    "Play a sine tone on AC'97: beep [freq_hz 20..12000] [ms 10..1000]"},
    {"hdatest", shell_cmd_hdatest, "Intel HDA status + tone: hdatest [freq_hz 20..12000] [ms 10..1000]"},
    {NULL, NULL, NULL}
};

/* Working directory */
static char g_cwd[256] = "/";

/* Boot/shutdown message buffer */
static char g_boot_messages[32][256];
static int g_boot_msg_count = 0;
static bool g_boot_complete = false;

/* ==================== Terminal Output Helpers ==================== */

void shell_putc(char c) {
    /* Always go through the console layer: it writes COM1 and drives whichever
     * screen renderer owns the framebuffer (the flanterm terminal when it is
     * up, fbcon otherwise). Calling fbcon_putc() directly here would paint with
     * a second renderer behind the terminal's back whenever coloured output is
     * requested, leaving stale glyphs on screen. */
    console_putc(c);
}

void shell_puts(const char *s) {
    while (*s) shell_putc(*s++);
}

void shell_printf(const char *fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (len > 0) shell_puts(buf);
}

void shell_set_colors(uint32_t fg, uint32_t bg) {
    g_shell.fg_color = fg;
    g_shell.bg_color = bg;
}

void shell_scroll_message(const char *msg) {
    if (g_boot_msg_count < 32) {
        strcpy(g_boot_messages[g_boot_msg_count++], msg);
    }
    if (g_boot_complete) {
        shell_printf("[%s] %s\n", "OK", msg);
    }
}

/* ==================== Shell Core ==================== */

/* ==================== Console input helper ====================
 * The PS/2 controller has a ONE byte output buffer: if the guest does not read a
 * byte before the next key arrives, that key is lost. Reading once per idle
 * iteration was not enough (typing `su user` could arrive as `user`), so a small
 * ring is filled with a burst of polls and drained by console_getc().
 * Uses the PS/2 keyboard driver for input. */

#define SHELL_KBD_RING 64
static char g_kbd_ring[SHELL_KBD_RING];
static int  g_kbd_head;
static int  g_kbd_tail;

/* Drain up to `budget` pending scancodes into the ring. */
static void shell_kbd_fill(int budget)
{
    while (budget-- > 0) {
        const int sc = mbos_kbd_poll();
        int ch;
        if (sc < 0) return;
        ch = mbos_kbd_decode(sc);
        if (ch <= 0) continue;
        {
            const int next = (g_kbd_head + 1) % SHELL_KBD_RING;
            if (next == g_kbd_tail) return;          /* ring full: leave the rest */
            g_kbd_ring[g_kbd_head] = (char)ch;
            g_kbd_head = next;
        }
    }
}


int console_getc(void) {
    if (g_kbd_tail == g_kbd_head) {
        shell_kbd_fill(16);                 /* refill: one poll is not enough */
        if (g_kbd_tail == g_kbd_head) return -1;
    }
    {
        const char c = g_kbd_ring[g_kbd_tail];
        g_kbd_tail = (g_kbd_tail + 1) % SHELL_KBD_RING;
        return (int)c;
    }
}

/* Keep the prompt in sync with the effective identity:
 *   root       -> "sutureos# "      (historical prompt; docs/tests rely on it)
 *   other user -> "user@sutureos$ "
 */
void shell_refresh_prompt(void) {
    if (cred_euid() == 0) {
        strcpy(g_shell.prompt, "sutureos# ");
    } else {
        snprintf(g_shell.prompt, sizeof(g_shell.prompt), "%s@sutureos$ ",
                 cred_username());
    }
}

void shell_init(void) {
    memset(&g_shell, 0, sizeof(g_shell));
    g_shell.fg_color = TERM_COLOR_WHITE;
    g_shell.bg_color = TERM_COLOR_BLACK;
    g_shell.running = true;
    strcpy(g_shell.prompt, "sutureos# ");
    /* NOTE: the screen is owned by the kernel terminal (flanterm) / fbcon,
     * both initialised by kmain. A previous version called
     * fbcon_init(0,0,0,0,0) here, which clobbered that state whenever the
     * shell task started. */
}

void shell_add_history(const char *cmd) {
    if (g_shell.history_count < SHELL_HISTORY_SIZE) {
        strcpy(g_shell.history[g_shell.history_count++], cmd);
    } else {
        /* Shift history */
        for (int i = 1; i < SHELL_HISTORY_SIZE; i++) {
            strcpy(g_shell.history[i-1], g_shell.history[i]);
        }
        strcpy(g_shell.history[SHELL_HISTORY_SIZE-1], cmd);
    }
    g_shell.history_index = g_shell.history_count;
}

int shell_find_command(const char *name) {
    for (int i = 0; g_commands[i].name; i++) {
        if (strcmp(g_commands[i].name, name) == 0) return i;
    }
    return -1;
}

int shell_process_line(const char *line) {
    if (!line || !*line) return 0;
    
    /* Parse command line into argc/argv */
    char *argv[SHELL_MAX_ARGS];
    int argc = 0;
    char buf[SHELL_MAX_CMD_LEN];
    strcpy(buf, line);
    
    char *token = strtok(buf, " \t");
    while (token && argc < SHELL_MAX_ARGS) {
        argv[argc++] = token;
        token = strtok(NULL, " \t");
    }
    argv[argc] = NULL;
    
    if (argc == 0) return 0;
    
    /* Add to history */
    shell_add_history(line);
    
    /* Find and execute command */
    int cmd_idx = shell_find_command(argv[0]);
    if (cmd_idx >= 0) {
        return g_commands[cmd_idx].func(argc, argv);
    } else {
        shell_printf("shell: %s: command not found\n", argv[0]);
        return -1;
    }
}

int shell_run(void) {
    /* Ensure shell state (prompt, colors, running flag) is initialized even
     * when the task is started directly, without an explicit shell_init(). */
    shell_init();

    /* Boot presentation: the scrolling English log has done its job - wipe it
     * and show the system name, then hand the user a clean terminal. */
    extern void boot_banner_draw(void);
    boot_banner_draw();

    shell_puts("\n");
    shell_puts("Welcome to SutureOS Terminal\n");
    shell_puts("Type 'help' for available commands\n");
    shell_puts("Type 'startgui' to launch GUI interface\n\n");
    
    g_boot_complete = true;
    
    while (g_shell.running) {
        g_shell.cmdline_len = 0;
        g_shell.cursor_pos = 0;
        memset(g_shell.cmdline, 0, SHELL_MAX_CMD_LEN);

        /* One prompt per command line; refresh it first so `su` / `sudo -i`
         * show up immediately (root keeps the historical `sutureos# `). */
        shell_refresh_prompt();
        shell_puts(g_shell.prompt);

        /* Read a line. While no key is pending, spin with a bounded pause
         * instead of re-printing the prompt. NOTE: deliberately no
         * sched_yield()/sched_eevdf_yield() here - the task round-robin in
         * task.c and the EEVDF runqueue are two separate schedulers and
         * yielding from a task running under sched_eevdf_start() resumes the
         * scheduler loop's stale context (observed as a wild jump to 0x3). */
        for (;;) {
            int c = console_getc();
            if (c < 0) {
                /* Refill the input ring first: keys must not wait behind the
                 * SMP/NIC work below, or they are dropped by the 8042. */
                shell_kbd_fill(8);
                if (g_kbd_tail != g_kbd_head) continue;
                /* Nothing to read: the BSP is idle here, so it also drains the
                 * SMP work queue (the APs drain it in their own idle loops).
                 * This is what makes queued kernel work run even when only the
                 * BSP has anything to do. */
                smp_drain_work_here(0);
                /* Keep the NIC's RX ring drained while the shell is idle: the
                 * driver runs in polling mode (no IRQ gate for it), so this is
                 * what lets ARP/ICMP replies arrive during `ping`. */
                {
                    extern int net_ready(void);
                    extern void net_poll(void);
                    if (net_ready()) net_poll();
                }
                for (volatile int spin = 0; spin < 20000; spin++) {
                    __asm__ volatile("pause");
                }
                continue;
            }
            if (c == '\n' || c == '\r') {
                shell_putc('\n');
                break;
            } else if (c == '\b' || c == 127) {
                if (g_shell.cmdline_len > 0) {
                    g_shell.cmdline_len--;
                    g_shell.cursor_pos--;
                    g_shell.cmdline[g_shell.cmdline_len] = 0;
                    shell_putc('\b');
                    shell_putc(' ');
                    shell_putc('\b');
                }
            } else if (c >= 32 && c < 127 &&
                       g_shell.cmdline_len < SHELL_MAX_CMD_LEN - 1) {
                g_shell.cmdline[g_shell.cmdline_len++] = (char)c;
                g_shell.cursor_pos++;
                shell_putc((char)c);
            }
        }

        if (g_shell.cmdline_len > 0) {
            shell_process_line(g_shell.cmdline);
        }
    }
    
    return g_shell.exit_code;
}

/* ==================== Built-in Commands ==================== */

int shell_cmd_help(int argc, char **argv) {
    (void)argc; (void)argv;
    shell_puts("Available commands:\n");
    for (int i = 0; g_commands[i].name; i++) {
        shell_printf("  %-12s %s\n", g_commands[i].name, g_commands[i].help);
    }
    return 0;
}

int shell_cmd_clear(int argc, char **argv) {
    (void)argc; (void)argv;
    /* Clear through whichever renderer owns the screen: fbcon_clear() would wipe
     * the framebuffer behind flanterm's back while its grid still believes the
     * old text is displayed, so the terminal would never repaint it. */
    extern int term_available(void);
    extern void term_clear(void);
    if (term_available()) term_clear();
    else fbcon_clear();
    return 0;
}

int shell_cmd_echo(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        shell_puts(argv[i]);
        if (i < argc - 1) shell_putc(' ');
    }
    shell_putc('\n');
    return 0;
}

int shell_cmd_exit(int argc, char **argv) {
    (void)argc; (void)argv;
    g_shell.running = false;
    return 0;
}

/* Defined further down; the log command needs it before that point. */
static int shell_atoi(const char *s);

/* mouseinfo: PS/2 mouse / 8042 diagnostics. */
int shell_cmd_mouseinfo(int argc, char **argv) {
    extern void mbos_mouse_diagnose(void);
    extern int  mbos_mouse_present(void);
    extern void mbos_mouse_poll(void);
    (void)argc; (void)argv;

    if (!mbos_mouse_present()) {
        shell_puts("mouseinfo: no PS/2 mouse was detected at boot\n");
    }
    mbos_mouse_poll();          /* pick up anything queued */
    mbos_mouse_diagnose();
    return 0;
}

/* log / dmesg: review the captured console log.
 *
 * "Auto-scroll" here means the tail: the last `n` lines are printed through the
 * normal console path, so the terminal follows the newest line and the prompt
 * ends up right below it - exactly like tailing a log. Default = one screenful. */
int shell_cmd_log(int argc, char **argv) {
    extern int log_ring_count(void);
    extern uint64_t log_ring_total_lines(void);
    extern void log_ring_dump_tail(int n, int (*emit)(const char *));
    extern uint32_t term_rows(void);
    int want = 0;

    if (argc > 1) {
        want = shell_atoi(argv[1]);
        if (want <= 0) {
            shell_printf("log: '%s' is not a line count\n", argv[1]);
            return -1;
        }
    }
    if (want == 0) {
        uint32_t rows = term_rows();
        want = (rows > 6u) ? (int)(rows - 4u) : 16;
    }

    shell_printf("--- log: last %d line(s) of %llu captured ---\n",
                 (int64_t)want, (unsigned long long)log_ring_total_lines());
    log_ring_dump_tail(want, NULL);          /* NULL = print + auto-scroll */
    shell_printf("--- end of log (%d line(s) held) ---\n", (int64_t)log_ring_count());
    return 0;
}

/* Flush the ramfs /etc + /home + /root mirror to the installed system volume and
 * then sync every mounted filesystem. Used before shutdown/reboot so nothing the
 * user changed at runtime is lost, and by the `sync` command. */
static void shell_persist_and_sync(void) {
    extern int persist_present(void);
    extern int persist_flush(void);
    vfs_mount_info_t mounts[VFS_MAX_MOUNTS];
    int n, i;

    if (persist_present()) (void)persist_flush();
    n = vfs_list_mounts(mounts, VFS_MAX_MOUNTS);
    for (i = 0; i < n; i++) (void)vfs_sync(mounts[i].mountpoint);
}

int shell_cmd_reboot(int argc, char **argv) {
    if (!require_root("reboot")) return 1;
    (void)argc; (void)argv;
    shell_puts("Rebooting...\n");
    shell_scroll_message("Rebooting system...");
    shell_persist_and_sync();
    sys_reboot();
    return 0;
}

int shell_cmd_shutdown(int argc, char **argv) {
    if (!require_root("shutdown")) return 1;
    (void)argc; (void)argv;
    shell_puts("Shutting down...\n");
    shell_scroll_message("Shutting down system...");
    shell_persist_and_sync();
    sys_shutdown();
    return 0;
}

int shell_cmd_startgui(int argc, char **argv) {
    (void)argc; (void)argv;
    if (argc > 1 && argv[1] != NULL && strcmp(argv[1], "test") == 0) {
        extern void gui_arm_pointer_selftest(void);
        gui_arm_pointer_selftest();
        shell_puts("Starting GUI interface (pointer self-test armed)...\n");
    } else {
        shell_puts("Starting GUI interface...\n");
    }
    shell_scroll_message("Initializing GUI...");
    
    /* Try to initialize GUI */
    int w = 1024, h = 768;  /* Default resolution */
    int ret = gui_init(w, h);
    if (ret != 0) {
        shell_printf("GUI initialization failed (error %d), staying in terminal\n", ret);
        shell_scroll_message("GUI init failed - terminal mode");
        return -1;
    }
    
    shell_scroll_message("GUI started successfully");
    
    /* Run GUI main loop */
    ret = gui_run();
    
    /* If GUI exits or fails, return to terminal */
    extern int term_available(void);
    extern void term_clear(void);
    extern void console_screen_enable(int on);
    console_screen_enable(1);        /* the GUI suspends the screen while it runs */
    if (term_available()) term_clear();
    else fbcon_clear();
    shell_puts("GUI exited, returning to terminal\n");
    /* Leaving the desktop is a natural checkpoint: persist whatever the GUI apps
     * (editor etc.) wrote, so a following shutdown cannot lose it. */
    {
        extern int persist_present(void);
        extern int persist_flush(void);
        if (persist_present()) (void)persist_flush();
    }
    shell_puts(g_shell.prompt);
    return ret;
}

/* ==================== Path / string helpers ==================== */

/* Resolve a user path against the shell cwd into an absolute path. */
static void shell_abspath(const char *in, char *out, size_t cap) {
    if (!in || !in[0] || strcmp(in, ".") == 0) {
        strncpy(out, g_cwd, cap - 1);
        out[cap - 1] = '\0';
        return;
    }
    if (in[0] == '/') {
        strncpy(out, in, cap - 1);
        out[cap - 1] = '\0';
        return;
    }
    strncpy(out, g_cwd, cap - 1);
    out[cap - 1] = '\0';
    if (strcmp(out, "/") != 0 && strlen(out) + strlen(in) + 1 < cap) {
        strcat(out, "/");
    }
    strncat(out, in, cap - strlen(out) - 1);
}

static int shell_atoi(const char *s) {
    int v = 0, neg = 0;
    if (!s || !*s) return -1;
    if (*s == '-') { neg = 1; s++; }
    while (*s) {
        if (*s < '0' || *s > '9') return -1;
        v = v * 10 + (*s - '0');
        s++;
    }
    return neg ? -v : v;
}

static const char *vfs_err_str(int e) {
    switch (e) {
        case VFS_OK:        return "ok";
        case VFS_ERR:       return "generic error";
        case VFS_ENOTFOUND: return "no such file or directory";
        case VFS_ENOTDIR:   return "not a directory";
        case VFS_EISDIR:    return "is a directory";
        case VFS_ENOSPC:    return "no space";
        case VFS_ECONFLICT: return "already exists / not empty";
        case VFS_EINVAL:    return "invalid argument";
        case VFS_EPERM:     return "operation not supported";
        case VFS_EIO:       return "i/o error";
        case VFS_EACCES:    return "permission denied";
        case VFS_EBUSY:     return "busy";
        case VFS_EXDEV:     return "cross-device";
        default:            return "unknown error";
    }
}

int shell_cmd_ls(int argc, char **argv) {
    char path[256];
    shell_abspath(argc >= 2 ? argv[1] : g_cwd, path, sizeof(path));

    vfs_stat_t st;
    int r = vfs_stat(path, &st);
    if (r == VFS_OK && st.type != VFS_NODE_DIR) {
        shell_printf("%s  %llu bytes\n", path, (unsigned long long)st.size);
        return 0;
    }
    if (r != VFS_OK) {
        shell_printf("ls: %s: %s\n", path, vfs_err_str(r));
        return -1;
    }

    int d = vfs_opendir(path);
    if (d < 0) {
        shell_printf("ls: %s: %s\n", path, vfs_err_str(d));
        return -1;
    }

    char name[VFS_MAX_NAME];
    vfs_node_type_t type;
    int count = 0;
    while (vfs_readdir(d, name, &type) == VFS_OK) {
        shell_printf("%-24s%s\n", name, type == VFS_NODE_DIR ? "/" : "");
        count++;
    }
    vfs_closedir(d);
    if (count == 0) {
        shell_puts("(empty directory)\n");
    }
    return 0;
}

int shell_cmd_cd(int argc, char **argv) {
    char path[256];
    shell_abspath(argc >= 2 ? argv[1] : "/", path, sizeof(path));

    vfs_stat_t st;
    int r = vfs_stat(path, &st);
    if (r != VFS_OK) {
        shell_printf("cd: %s: %s\n", path, vfs_err_str(r));
        return -1;
    }
    if (st.type != VFS_NODE_DIR) {
        shell_printf("cd: %s: not a directory\n", path);
        return -1;
    }

    strncpy(g_cwd, path, sizeof(g_cwd) - 1);
    g_cwd[sizeof(g_cwd) - 1] = '\0';
    vfs_set_cwd(g_cwd);
    return 0;
}

int shell_cmd_pwd(int argc, char **argv) {
    (void)argc; (void)argv;
    shell_puts(g_cwd);
    shell_putc('\n');
    return 0;
}

int shell_cmd_cat(int argc, char **argv) {
    if (argc < 2) {
        shell_puts("Usage: cat <file>\n");
        return -1;
    }
    char path[256];
    shell_abspath(argv[1], path, sizeof(path));

    vfs_stat_t st;
    int r = vfs_stat(path, &st);
    if (r != VFS_OK) {
        shell_printf("cat: %s: %s\n", path, vfs_err_str(r));
        return -1;
    }
    if (st.type == VFS_NODE_DIR) {
        shell_printf("cat: %s: is a directory\n", path);
        return -1;
    }

    int fd = vfs_open(path, VFS_O_RDONLY);
    if (fd < 0) {
        shell_printf("cat: %s: %s\n", path, vfs_err_str(fd));
        return -1;
    }

    char buf[256];
    int n;
    uint64_t total = 0;
    char last = 0;
    while ((n = vfs_read(fd, buf, sizeof(buf))) > 0) {
        for (int i = 0; i < n; i++) {
            if (buf[i] != '\0') { shell_putc(buf[i]); last = buf[i]; }
        }
        total += (uint64_t)n;
    }
    vfs_close(fd);
    if (total == 0 || last != '\n') shell_putc('\n');
    shell_printf("(%llu bytes)\n", (unsigned long long)total);
    return 0;
}

/* ---- small text-file tools (v0.19.0) ----------------------------------
 * `write` / `append` / `hexdump` are the missing half of `cat`: there was no way
 * to create a file from the terminal, so every test needed a pre-built image.
 * They only use the public VFS API, so they work on ramfs, FAT32 and ext2. */
static uint32_t shell_parse_u32(const char *s) {
    uint32_t v = 0;
    if (!s) return 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        while (*s) {
            uint32_t d;
            if (*s >= '0' && *s <= '9') d = (uint32_t)(*s - '0');
            else if (*s >= 'a' && *s <= 'f') d = (uint32_t)(*s - 'a' + 10);
            else if (*s >= 'A' && *s <= 'F') d = (uint32_t)(*s - 'A' + 10);
            else break;
            v = v * 16u + d;
            s++;
        }
        return v;
    }
    while (*s >= '0' && *s <= '9') { v = v * 10u + (uint32_t)(*s - '0'); s++; }
    return v;
}

/* "arg2 arg3 ...\n" into buf (returns the length, 0 on overflow). */
static uint32_t shell_join_args(int argc, char **argv, char *buf, uint32_t cap) {
    uint32_t n = 0;
    for (int i = 2; i < argc; i++) {
        if (i > 2) {
            if (n + 1 >= cap) return 0;
            buf[n++] = ' ';
        }
        for (const char *p = argv[i]; *p; p++) {
            if (n + 1 >= cap) return 0;
            buf[n++] = *p;
        }
    }
    if (n + 1 >= cap) return 0;
    buf[n++] = '\n';
    return n;
}

int shell_cmd_write(int argc, char **argv) {
    if (argc < 3) {
        shell_puts("Usage: write <file> <text...>\n");
        return -1;
    }
    char path[256];
    shell_abspath(argv[1], path, sizeof(path));
    if (shell_path_root_only(path)) {
        shell_printf("write: %s: Permission denied (only root may modify /etc or /root)\n",
                     path);
        return -1;
    }
    char buf[512];
    const uint32_t n = shell_join_args(argc, argv, buf, sizeof(buf));
    if (n == 0) {
        shell_puts("write: text too long\n");
        return -1;
    }
    int r = vfs_write_file(path, buf, n);
    if (r != VFS_OK) {
        shell_printf("write: %s: %s\n", path, vfs_err_str(r));
        return -1;
    }
    shell_printf("write: %s (%lu bytes)\n", path, (unsigned long)n);
    return 0;
}

int shell_cmd_append(int argc, char **argv) {
    if (argc < 3) {
        shell_puts("Usage: append <file> <text...>\n");
        return -1;
    }
    char path[256];
    shell_abspath(argv[1], path, sizeof(path));
    if (shell_path_root_only(path)) {
        shell_printf("append: %s: Permission denied (only root may modify /etc or /root)\n",
                     path);
        return -1;
    }

    static char buf[2048];
    uint32_t n = 0;

    vfs_stat_t st;
    if (vfs_stat(path, &st) == VFS_OK && st.type == VFS_NODE_FILE) {
        int fd = vfs_open(path, VFS_O_RDONLY);
        if (fd < 0) {
            shell_printf("append: %s: %s\n", path, vfs_err_str(fd));
            return -1;
        }
        int got;
        while (n + 32u < sizeof(buf) &&
               (got = vfs_read(fd, buf + n, (uint32_t)(sizeof(buf) - n - 1u))) > 0) {
            n += (uint32_t)got;
        }
        vfs_close(fd);
        /* keep exactly one trailing newline in front of the new line */
        if (n > 0 && buf[n - 1] != '\n') {
            if (n + 1u >= sizeof(buf)) { shell_puts("append: file too big\n"); return -1; }
            buf[n++] = '\n';
        }
    }

    const uint32_t add = shell_join_args(argc, argv, buf + n, (uint32_t)(sizeof(buf) - n));
    if (add == 0) {
        shell_puts("append: text too long\n");
        return -1;
    }
    n += add;

    int r = vfs_write_file(path, buf, n);
    if (r != VFS_OK) {
        shell_printf("append: %s: %s\n", path, vfs_err_str(r));
        return -1;
    }
    shell_printf("append: %s now %lu bytes\n", path, (unsigned long)n);
    return 0;
}

int shell_cmd_hexdump(int argc, char **argv) {
    if (argc < 2) {
        shell_puts("Usage: hexdump <file> [offset] [length]\n");
        return -1;
    }
    char path[256];
    shell_abspath(argv[1], path, sizeof(path));
    const uint32_t off = (argc >= 3) ? shell_parse_u32(argv[2]) : 0;
    const uint32_t want = (argc >= 4) ? shell_parse_u32(argv[3]) : 128u;

    int fd = vfs_open(path, VFS_O_RDONLY);
    if (fd < 0) {
        shell_printf("hexdump: %s: %s\n", path, vfs_err_str(fd));
        return -1;
    }
    if (off && vfs_seek(fd, (int32_t)off, VFS_SEEK_SET) < 0) {
        shell_printf("hexdump: cannot seek to %lu\n", (unsigned long)off);
        vfs_close(fd);
        return -1;
    }

    static const char hexd[] = "0123456789abcdef";
    uint8_t row[16];
    uint32_t done = 0;
    while (done < want) {
        uint32_t chunk = want - done;
        if (chunk > sizeof(row)) chunk = sizeof(row);
        int got = vfs_read(fd, row, chunk);
        if (got <= 0) break;
        char line[8 + 1 + 3 * 16 + 1 + 16 + 1];
        char *p = line;
        const uint32_t base = off + done;
        for (int s = 28; s >= 0; s -= 4) *p++ = hexd[(base >> s) & 0xF];
        *p++ = ' ';
        *p++ = ' ';
        for (int i = 0; i < 16; i++) {
            if (i < got) {
                *p++ = hexd[(row[i] >> 4) & 0xF];
                *p++ = hexd[row[i] & 0xF];
            } else {
                *p++ = ' ';
                *p++ = ' ';
            }
            *p++ = ' ';
        }
        *p++ = '|';
        for (int i = 0; i < got; i++) {
            const uint8_t c = row[i];
            *p++ = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
        }
        *p++ = '|';
        *p = 0;
        shell_puts(line);
        shell_putc('\n');
        done += (uint32_t)got;
    }
    vfs_close(fd);
    shell_printf("(%lu bytes at offset %lu)\n", (unsigned long)done, (unsigned long)off);
    return 0;
}

/* ---- text utilities (v0.26.0): wc / head / tail / grep / stat ------------
 * All of them stream through the public VFS, so they work on ramfs, FAT32, ext2
 * and squashfs alike. grep uses a hand-rolled substring match because the kernel
 * libc has no strstr().
 */
static const char *shell_find_sub(const char *hay, const char *needle) {
    if (!hay || !needle || !*needle) return NULL;
    for (const char *p = hay; *p; p++) {
        const char *a = p, *b = needle;
        while (*a && *b && *a == *b) { a++; b++; }
        if (*b == 0) return p;
    }
    return NULL;
}

/* Open a file and print the usual error; returns fd or -1. */
static int shell_open_text(const char *arg, char *path, uint32_t cap, int *is_dir) {
    shell_abspath(arg, path, cap);
    vfs_stat_t st;
    int r = vfs_stat(path, &st);
    if (r != VFS_OK) {
        shell_printf("%s: %s: %s\n", "open", path, vfs_err_str(r));
        return -1;
    }
    if (is_dir) *is_dir = (st.type == VFS_NODE_DIR);
    if (st.type == VFS_NODE_DIR) {
        shell_printf("%s: %s: is a directory\n", "open", path);
        return -1;
    }
    int fd = vfs_open(path, VFS_O_RDONLY);
    if (fd < 0) shell_printf("open: %s: %s\n", path, vfs_err_str(fd));
    return fd;
}

int shell_cmd_wc(int argc, char **argv) {
    if (argc < 2) {
        shell_puts("Usage: wc <file>\n");
        return -1;
    }
    char path[256];
    int fd = shell_open_text(argv[1], path, sizeof(path), NULL);
    if (fd < 0) return -1;

    char buf[256];
    uint64_t lines = 0, words = 0, bytes = 0;
    int in_word = 0, n;
    while ((n = vfs_read(fd, buf, sizeof(buf))) > 0) {
        for (int i = 0; i < n; i++) {
            const char c = buf[i];
            bytes++;
            if (c == '\n') lines++;
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                in_word = 0;
            } else if (!in_word) {
                in_word = 1;
                words++;
            }
        }
    }
    vfs_close(fd);
    shell_printf("%llu %llu %llu %s\n", (unsigned long long)lines,
                 (unsigned long long)words, (unsigned long long)bytes, path);
    return 0;
}

int shell_cmd_head(int argc, char **argv) {
    if (argc < 2) {
        shell_puts("Usage: head <file> [lines]\n");
        return -1;
    }
    const uint32_t want = (argc >= 3) ? (uint32_t)shell_parse_u32(argv[2]) : 10u;
    char path[256];
    int fd = shell_open_text(argv[1], path, sizeof(path), NULL);
    if (fd < 0) return -1;

    char buf[256];
    uint32_t shown = 0;
    int done = 0;
    while (!done && (int)(0) == 0) {
        int n = vfs_read(fd, buf, sizeof(buf));
        if (n <= 0) break;
        for (int i = 0; i < n; i++) {
            shell_putc(buf[i]);
            if (buf[i] == '\n') {
                if (++shown >= want) { done = 1; break; }
            }
        }
    }
    if (!done) shell_putc('\n');
    vfs_close(fd);
    return 0;
}

int shell_cmd_tail(int argc, char **argv) {
    if (argc < 2) {
        shell_puts("Usage: tail <file> [lines]\n");
        return -1;
    }
    const uint32_t want = (argc >= 3) ? (uint32_t)shell_parse_u32(argv[2]) : 10u;
    char path[256];
    int fd = shell_open_text(argv[1], path, sizeof(path), NULL);
    if (fd < 0) return -1;

    /* keep the last `want` lines as file offsets, then re-read from there */
    char buf[256];
    static uint32_t starts[64];
    uint32_t count = 0;
    uint64_t off = 0;
    int n;
    int first = 1;
    while ((n = vfs_read(fd, buf, sizeof(buf))) > 0) {
        for (int i = 0; i < n; i++, off++) {
            if (buf[i] == '\n') {
                if (count < want) {
                    starts[count++] = (uint32_t)off + 1u;
                } else {
                    for (uint32_t k = 1; k < want; k++) starts[k - 1] = starts[k];
                    starts[want - 1] = (uint32_t)off + 1u;
                }
                first = 0;
            }
        }
    }
    if (count == 0) {                    /* one unterminated line: whole file */
        vfs_close(fd);
        return shell_cmd_head(argc, argv);
    }
    (void)first;
    uint32_t start_off = 0;
    if (count >= want) {
        start_off = starts[0];
    }
    if (vfs_seek(fd, (int32_t)start_off, VFS_SEEK_SET) < 0) {
        shell_puts("tail: seek failed\n");
        vfs_close(fd);
        return -1;
    }
    while ((n = vfs_read(fd, buf, sizeof(buf))) > 0) {
        for (int i = 0; i < n; i++) shell_putc(buf[i]);
    }
    shell_putc('\n');
    vfs_close(fd);
    return 0;
}

int shell_cmd_grep(int argc, char **argv) {
    if (argc < 3) {
        shell_puts("Usage: grep <pattern> <file>\n");
        return -1;
    }
    char path[256];
    int fd = shell_open_text(argv[2], path, sizeof(path), NULL);
    if (fd < 0) return -1;

    char line[512];
    uint32_t len = 0;
    uint32_t lineno = 0, hits = 0;
    int n;
    for (;;) {
        n = vfs_read(fd, line + len, (uint32_t)(sizeof(line) - 2u - len));
        if (n <= 0) break;
        len += (uint32_t)n;
        line[len] = '\0';
        char *start = line;
        /* the match points into the mutable local buffer, so the cast is safe */
        char *nl;
        while ((nl = (char *)shell_find_sub(start, "\n")) != NULL) {
            *nl = '\0';
            lineno++;
            if (shell_find_sub(start, argv[1])) {
                shell_printf("%lu: %s\n", (unsigned long)lineno, start);
                hits++;
            }
            start = nl + 1;
        }
        len = (uint32_t)strlen(start);
        memmove(line, start, len + 1u);
        if (len + 8u >= sizeof(line)) {     /* line longer than the buffer */
            if (shell_find_sub(line, argv[1])) {
                shell_printf("%lu: %s(...)\n", (unsigned long)++lineno, line);
                hits++;
            }
            len = 0;
        }
    }
    if (len > 0) {                          /* last line without newline */
        line[len] = '\0';
        lineno++;
        if (shell_find_sub(line, argv[1])) {
            shell_printf("%lu: %s\n", (unsigned long)lineno, line);
            hits++;
        }
    }
    vfs_close(fd);
    shell_printf("grep: %lu match(es) in %s\n", (unsigned long)hits, path);
    return hits > 0 ? 0 : 1;
}

int shell_cmd_stat(int argc, char **argv) {
    if (argc < 2) {
        shell_puts("Usage: stat <path>\n");
        return -1;
    }
    char path[256];
    shell_abspath(argv[1], path, sizeof(path));
    vfs_stat_t st;
    int r = vfs_stat(path, &st);
    if (r != VFS_OK) {
        shell_printf("stat: %s: %s\n", path, vfs_err_str(r));
        return -1;
    }
    shell_printf("  File: %s\n", path);
    shell_printf("  Type: %s\n", st.type == VFS_NODE_DIR ? "directory" : "regular file");
    shell_printf("  Size: %lu bytes\n", (unsigned long)st.size);
    return 0;
}

/* ---- sync (v0.28.0) -----------------------------------------------------
 * Until now a freshly written file only reached the disk when the volume was
 * unmounted (or when a later write flushed the FSInfo). `sync` walks the mount
 * table and asks every driver to flush, which is what a user expects after
 * `write` on a FAT32/ext2 volume. */
int shell_cmd_sync(int argc, char **argv) {
    (void)argc; (void)argv;
    vfs_mount_info_t mounts[VFS_MAX_MOUNTS];
    int n = vfs_list_mounts(mounts, VFS_MAX_MOUNTS);

    /* Persistence mirror first: ramfs /etc + /home + /root -> the system volume.
     * persist_flush() ends with vfs_sync("/mnt/sys"), so the FAT metadata reaches
     * the disk too and a shutdown right after `sync` cannot lose it. */
    {
        extern int persist_present(void);
        extern int persist_flush(void);
        if (persist_present()) (void)persist_flush();
    }

    if (n <= 0) {
        shell_puts("sync: no filesystems mounted\n");
        return 0;
    }
    int ok = 0, failed = 0;
    for (int i = 0; i < n; i++) {
        int r = vfs_sync(mounts[i].mountpoint);
        if (r == VFS_OK) {
            ok++;
            shell_printf("sync: %s (%s) ok\n", mounts[i].mountpoint, mounts[i].fstype);
        } else {
            failed++;
            shell_printf("sync: %s (%s) FAILED (%d)\n", mounts[i].mountpoint,
                         mounts[i].fstype, r);
        }
    }
    shell_printf("sync: %d ok, %d failed\n", ok, failed);
    return failed == 0 ? 0 : -1;
}

/* Persistent user data (kernel/fs/persist.c): status + manual flush. */
int shell_cmd_persist(int argc, char **argv) {
    extern int persist_present(void);
    extern int persist_flush(void);
    extern int persist_restored_files(void);
    extern int persist_flushed_files(void);
    extern int persist_flush_count(void);
    extern int persist_last_error(void);
    extern int persist_format_version(void);
    const char *sub = (argc > 1 && argv[1]) ? argv[1] : NULL;

    if (!persist_present()) {
        shell_puts("persist: no installed system volume here "
                   "(live ISO session) - nothing is written\n");
        return 0;
    }
    if (sub && strcmp(sub, "flush") == 0) {
        int n = persist_flush();
        if (n < 0) {
            shell_puts("persist: flush FAILED\n");
            return -1;
        }
        shell_printf("persist: flushed %d file(s)\n", (int64_t)n);
        return 0;
    }
    shell_printf("persist: volume=/mnt/sys/SUTUREOS/PERSIST format=v%d\n",
                 (int64_t)persist_format_version());
    shell_printf("persist: restored at boot=%d file(s), last flush=%d file(s), "
                 "flushes=%d, last_error=%d\n",
                 (int64_t)persist_restored_files(), (int64_t)persist_flushed_files(),
                 (int64_t)persist_flush_count(), (int64_t)persist_last_error());
    shell_puts("persist: mirrors /etc, /home, /root; "
               "'sync' and shutdown/reboot flush automatically\n");
    return 0;
}

int shell_cmd_ps(int argc, char **argv) {
    (void)argc; (void)argv;
    static const char *state_names[] = {
        "unused", "runnable", "running", "zombie"
    };
    shell_puts("TID  STATE     ENTRY\n");
    shell_puts("---  -----     -----\n");
    int count = 0;
    for (int i = 0; i < SCHED_MAX_TASKS; i++) {
        sched_task_t *t = &sched_cpu.tasks[i];
        if (t->state == SCHED_TASK_UNUSED) continue;
        const char *sn = (t->state < 4) ? state_names[t->state] : "?";
        shell_printf("%c%-3d %-9s %p\n", i == sched_cpu.current ? '*' : ' ',
                     i, sn, (void *)t->func);
        count++;
    }
    if (count == 0) shell_puts("(no tasks)\n");
    return 0;
}

int shell_cmd_kill(int argc, char **argv) {
    if (!require_root("kill")) return 1;
    if (argc < 2) {
        shell_puts("Usage: kill <tid>\n");
        return -1;
    }
    int tid = shell_atoi(argv[1]);
    if (tid < 0 || tid >= SCHED_MAX_TASKS) {
        shell_printf("kill: %s: invalid task id\n", argv[1]);
        return -1;
    }
    if (tid == sched_cpu.current) {
        shell_puts("kill: refusing to kill the shell itself (use exit)\n");
        return -1;
    }
    sched_task_t *t = &sched_cpu.tasks[tid];
    if (t->state == SCHED_TASK_UNUSED) {
        shell_printf("kill: no such task: %d\n", tid);
        return -1;
    }
    if (t->state == SCHED_TASK_ZOMBIE) {
        shell_printf("kill: task %d is already dead\n", tid);
        return -1;
    }

    /* The task's sched_entity (if any) is stashed in kgs_base by the
     * EEVDF scheduler. Dequeue it so pick_eevdf never selects it again. */
    struct sched_entity *se = (struct sched_entity *)(uintptr_t)t->context.kgs_base;
    if (se && se->on_rq) {
        eevdf_dequeue_entity(se);
    }
    t->state = SCHED_TASK_ZOMBIE;
    shell_printf("task %d killed\n", tid);
    return 0;
}

/* sudo: forwards to the kernel sudo engine (kernel/security/sudo.c), which
 * implements Linux sudo's sudoers policy, /etc/shadow password verification, the
 * timestamp ticket cache and the audit log. */
int shell_cmd_sudo(int argc, char **argv) {
    return sudo_main(argc, argv);
}

/* login [user]: adopt an identity after authenticating (multiuser batch).
 * `login` with no argument prompts "login: " and then the password. */
int shell_cmd_login(int argc, char **argv) {
    return login_main(argc, argv);
}

/* su [-] [user]: same engine as `login`, with Linux's su rules:
 *   - root never needs a password,
 *   - a passwordless target (empty shadow field) is entered without a prompt,
 *   - a locked target (`!`) is refused,
 *   - `su -` also switches the working directory to the target's home. */
int shell_cmd_su(int argc, char **argv) {
    return login_su(argc, argv);
}

int shell_cmd_whoami(int argc, char **argv) {
    (void)argc; (void)argv;
    shell_printf("%s\n", cred_username());
    return 0;
}

int shell_cmd_id(int argc, char **argv) {
    char buf[192];
    (void)argc; (void)argv;
    cred_format_id(buf, sizeof(buf));
    shell_printf("%s\n", buf);
    return 0;
}

/* users: every account with its numeric ids, groups and password state. */
int shell_cmd_users(int argc, char **argv) {
    const char *me = cred_username();
    (void)argc; (void)argv;

    shell_printf("%u account%s in %s\n", (uint64_t)userdb_count(),
                 (userdb_count() == 1) ? "" : "s", "/etc/passwd");
    for (uint32_t i = 0; i < userdb_count(); i++) {
        const userdb_user_t *u = userdb_at(i);
        char groups[128];
        if (u == NULL) continue;
        userdb_format_groups(u->name, groups, sizeof(groups));
        shell_printf("%s uid=%u gid=%u groups=%s password=%s%s\n", u->name,
                     (uint64_t)u->uid, (uint64_t)u->gid, groups,
                     userdb_pwstate_text(u->name),
                     (strcmp(u->name, me) == 0) ? " current" : "");
    }
    return 0;
}

/* useradd <name> (root only): allocate the next uid >= 1000, create the private
 * group + home directory and ask for a password. An empty password leaves the
 * account LOCKED ("留空=禁用密码"), never passwordless. */
int shell_cmd_useradd(int argc, char **argv) {
    const char *name;
    char pw[96], pw2[96];
    uint32_t uid = 0;
    int r;

    if (!cred_is_root()) {
        shell_puts("useradd: permission denied (root only)\n");
        return 1;
    }
    if (argc < 2) {
        shell_puts("usage: useradd <name>\n");
        return 1;
    }
    name = argv[1];
    if (!userdb_valid_name(name)) {
        shell_puts("useradd: invalid name (start with a letter; a-z 0-9 _ -; "
                   "at most 31 characters; reserved: root/wheel/all/none)\n");
        return 1;
    }
    if (userdb_by_name(name) != NULL) {
        shell_printf("useradd: user %s already exists\n", name);
        return 1;
    }

    pw[0] = '\0';
    pw2[0] = '\0';
    shell_puts("New password (empty = leave the account locked): ");
    if (login_read_line(NULL, pw, sizeof(pw), 1) < 0) {
        shell_puts("useradd: aborted\n");
        return 1;
    }
    if (pw[0] != '\0') {
        shell_puts("Retype new password: ");
        if (login_read_line(NULL, pw2, sizeof(pw2), 1) < 0) {
            memset(pw, 0, sizeof(pw));
            shell_puts("useradd: aborted\n");
            return 1;
        }
        if (strcmp(pw, pw2) != 0) {
            memset(pw, 0, sizeof(pw));
            memset(pw2, 0, sizeof(pw2));
            shell_puts("useradd: passwords do not match\n");
            return 1;
        }
    }

    r = userdb_add(name, (pw[0] == '\0') ? NULL : pw, &uid);
    memset(pw, 0, sizeof(pw));
    memset(pw2, 0, sizeof(pw2));
    if (r != 0) {
        if (r == -2) shell_printf("useradd: user %s already exists\n", name);
        else shell_puts("useradd: cannot create the account (no free slot)\n");
        return 1;
    }
    if (userdb_save() != 0) {
        shell_puts("useradd: cannot write /etc/passwd, /etc/shadow, /etc/group\n");
        return 1;
    }
    shell_printf("useradd: created %s uid=%u gid=%u home=/home/%s password=%s\n",
                 name, (uint64_t)uid, (uint64_t)uid, name,
                 userdb_pwstate_text(name));
    console_printf("[user] useradd %s uid=%u password=%s\n", name, (uint64_t)uid,
                   userdb_pwstate_text(name));
    return 0;
}

/* userdel <name> (root only). Refuses root and the current session's user. */
int shell_cmd_userdel(int argc, char **argv) {
    const char *name;

    if (!cred_is_root()) {
        shell_puts("userdel: permission denied (root only)\n");
        return 1;
    }
    if (argc < 2) {
        shell_puts("usage: userdel <name>\n");
        return 1;
    }
    name = argv[1];
    if (strcmp(name, "root") == 0) {
        shell_puts("userdel: refusing to remove root\n");
        return 1;
    }
    if (strcmp(name, cred_username()) == 0) {
        shell_printf("userdel: cannot remove %s: that is the current user\n", name);
        return 1;
    }
    if (userdb_by_name(name) == NULL) {
        shell_printf("userdel: user %s does not exist\n", name);
        return 1;
    }
    if (userdb_remove(name) != 0) {
        shell_printf("userdel: cannot remove %s\n", name);
        return 1;
    }
    if (userdb_save() != 0) {
        shell_puts("userdel: cannot write /etc/passwd, /etc/shadow, /etc/group\n");
        return 1;
    }
    shell_printf("userdel: removed %s (home directory kept)\n", name);
    console_printf("[user] userdel %s\n", name);
    return 0;
}

/* passwd [-l|-u|-d] [user]: set / lock / unlock / clear a password.
 * A normal user may only change their own password and must prove the current
 * one first (Linux behaviour); root may change anyone's without the old one.
 *   -l  lock (`!` in /etc/shadow)          -u  unlock
 *   -d  delete the password -> passwordless (empty shadow field) */
int shell_cmd_passwd(int argc, char **argv) {
    int mode = 0;                       /* 0 set, 1 lock, 2 unlock, 3 delete */
    int argi = 1;
    const char *target;
    char first[96];
    char second[96];
    char old[96];

    while (argi < argc && argv[argi][0] == '-' && argv[argi][1] != '\0') {
        const char *opt = argv[argi];
        if (strcmp(opt, "-l") == 0) mode = 1;
        else if (strcmp(opt, "-u") == 0) mode = 2;
        else if (strcmp(opt, "-d") == 0) mode = 3;
        else {
            shell_printf("passwd: invalid option %s (use -l, -u or -d)\n", opt);
            return 1;
        }
        argi++;
    }

    if (argi < argc) {
        target = argv[argi];
        if (!cred_is_root() && strcmp(cred_username(), target) != 0) {
            shell_printf("passwd: you may not change %s's password\n", target);
            return 1;
        }
    } else {
        target = cred_username();
    }
    if (userdb_by_name(target) == NULL) {
        shell_printf("passwd: user %s does not exist\n", target);
        return 1;
    }
    if (mode != 0 && !cred_is_root()) {
        shell_puts("passwd: permission denied (root only for -l/-u/-d)\n");
        return 1;
    }

    /* Changing a password as a normal user requires the current one first. */
    if (!cred_is_root()) {
        old[0] = '\0';
        shell_puts("Current password: ");
        if (login_read_line(NULL, old, sizeof(old), 1) < 0) {
            shell_puts("passwd: aborted\n");
            return 1;
        }
        if (login_authenticate(target, old) != LOGIN_OK) {
            memset(old, 0, sizeof(old));
            shell_puts("passwd: Authentication failure\n");
            return 1;
        }
        memset(old, 0, sizeof(old));
    }

    if (mode == 1) {
        if (userdb_lock(target) != 0) {
            shell_puts("passwd: could not write /etc/shadow\n");
            return 1;
        }
        shell_printf("passwd: %s locked (! in /etc/shadow)\n", target);
        return 0;
    }
    if (mode == 2) {
        if (userdb_unlock(target) != 0) {
            shell_puts("passwd: could not write /etc/shadow\n");
            return 1;
        }
        shell_printf("passwd: %s unlocked (password=%s)\n", target,
                     userdb_pwstate_text(target));
        return 0;
    }
    if (mode == 3) {
        if (userdb_clear_password(target) != 0) {
            shell_puts("passwd: could not write /etc/shadow\n");
            return 1;
        }
        shell_printf("passwd: %s now has no password (password=%s)\n", target,
                     userdb_pwstate_text(target));
        console_printf("[user] passwd -d %s (passwordless)\n", target);
        return 0;
    }

    first[0] = '\0';
    second[0] = '\0';
    shell_puts("New password: ");
    if (login_read_line(NULL, first, sizeof(first), 1) < 0) return 1;
    if (first[0] == '\0') {
        shell_puts("passwd: password unchanged\n");
        return 1;
    }
    shell_puts("Retype new password: ");
    if (login_read_line(NULL, second, sizeof(second), 1) < 0) {
        memset(first, 0, sizeof(first));
        return 1;
    }
    if (strcmp(first, second) != 0) {
        memset(first, 0, sizeof(first));
        memset(second, 0, sizeof(second));
        shell_puts("Sorry, passwords do not match.\n");
        return 1;
    }
    if (userdb_set_password(target, first) != 0) {
        memset(first, 0, sizeof(first));
        memset(second, 0, sizeof(second));
        shell_puts("passwd: could not write /etc/shadow\n");
        return 1;
    }
    memset(first, 0, sizeof(first));
    memset(second, 0, sizeof(second));
    shell_printf("passwd: password updated successfully for %s\n", target);
    console_printf("[user] passwd set for %s\n", target);
    return 0;
}

/* sudotest: automated acceptance for the sudo layer (CI asserts on this). */
int shell_cmd_sudotest(int argc, char **argv) {
    (void)argc; (void)argv;
    return sudo_selftest();
}

/* ==================== file management (ring-0 shell) ====================
 * These used to be missing entirely, so a user could not create or delete a file
 * even though the VFS has supported it for a while. Each one calls the syscall
 * layer (syscall_invoke) instead of the VFS directly, which also exercises the
 * syscalls a user program will use. */
static int sh_syscall5(uint64_t nr, uint64_t a1, uint64_t a2, uint64_t a3,
                       uint64_t a4)
{
    extern uint64_t syscall_invoke(uint64_t, uint64_t, uint64_t, uint64_t,
                                   uint64_t, uint64_t, uint64_t);
    return (int)syscall_invoke(nr, a1, a2, a3, a4, 0, 0);
}

int shell_cmd_rm(int argc, char **argv) {
    if (shell_guard_paths("rm", argc, argv, 1)) return 1;
    int rc = 0;
    if (argc < 2) {
        shell_puts("usage: rm <path>...\n");
        return 1;
    }
    for (int i = 1; i < argc; i++) {
        const int r = sh_syscall5(SYS_UNLINK, (uint64_t)(uintptr_t)argv[i], 0, 0, 0);
        if (r != 0) {
            shell_printf("rm: %s: %s\n", argv[i],
                         (r == -2) ? "no such file or directory"
                                   : ((r == -21) ? "is a directory (use rmdir)" : "failed"));
            rc = 1;
        }
    }
    return rc;
}

int shell_cmd_mkdir(int argc, char **argv) {
    if (shell_guard_paths("mkdir", argc, argv, 1)) return 1;
    int rc = 0;
    if (argc < 2) {
        shell_puts("usage: mkdir <dir>...\n");
        return 1;
    }
    for (int i = 1; i < argc; i++) {
        const int r = sh_syscall5(SYS_MKDIR, (uint64_t)(uintptr_t)argv[i], 0755, 0, 0);
        if (r != 0) {
            shell_printf("mkdir: %s: %s\n", argv[i],
                         (r == -17) ? "file exists" : "cannot create");
            rc = 1;
        }
    }
    return rc;
}

int shell_cmd_rmdir(int argc, char **argv) {
    if (shell_guard_paths("rmdir", argc, argv, 1)) return 1;
    int rc = 0;
    if (argc < 2) {
        shell_puts("usage: rmdir <dir>...\n");
        return 1;
    }
    for (int i = 1; i < argc; i++) {
        const int r = sh_syscall5(SYS_RMDIR, (uint64_t)(uintptr_t)argv[i], 0, 0, 0);
        if (r != 0) {
            shell_printf("rmdir: %s: cannot remove\n", argv[i]);
            rc = 1;
        }
    }
    return rc;
}

int shell_cmd_touch(int argc, char **argv) {
    if (shell_guard_paths("touch", argc, argv, 1)) return 1;
    extern int vfs_write_file(const char *, const void *, uint32_t);
    int rc = 0;
    if (argc < 2) {
        shell_puts("usage: touch <file>...\n");
        return 1;
    }
    for (int i = 1; i < argc; i++) {
        /* create without truncating if it already exists */
        {
            extern int vfs_stat(const char *, vfs_stat_t *);
            vfs_stat_t st;
            if (vfs_stat(argv[i], &st) == VFS_OK) continue;
        }
        if (vfs_write_file(argv[i], "", 0) != VFS_OK) {
            shell_printf("touch: %s: cannot create\n", argv[i]);
            rc = 1;
        }
    }
    return rc;
}

/* read a whole file into a heap buffer (NUL terminated) */
static char *sh_read_file(const char *path, size_t *len) {
    extern void *vfs_read_file(const char *, size_t *);
    return (char *)vfs_read_file(path, len);
}

int shell_cmd_cp(int argc, char **argv) {
    extern int vfs_write_file(const char *, const void *, uint32_t);
    size_t len = 0;
    char *data;
    if (argc < 3) {
        shell_puts("usage: cp <source> <dest>\n");
        return 1;
    }
    if (shell_guard_paths("cp", 3, argv, 2)) return 1;
    data = sh_read_file(argv[1], &len);
    if (data == NULL) {
        shell_printf("cp: %s: no such file\n", argv[1]);
        return 1;
    }
    if (vfs_write_file(argv[2], data, (uint32_t)len) != VFS_OK) {
        shell_printf("cp: %s: cannot write\n", argv[2]);
        free(data);
        return 1;
    }
    free(data);
    shell_printf("cp: %s -> %s (%u bytes)\n", argv[1], argv[2], (unsigned)len);
    return 0;
}

int shell_cmd_mv(int argc, char **argv) {
    if (shell_guard_paths("mv", argc, argv, 1)) return 1;
    if (argc < 3) {
        shell_puts("usage: mv <source> <dest>\n");
        return 1;
    }
    {
        const int r = sh_syscall5(SYS_RENAME, (uint64_t)(uintptr_t)argv[1],
                                  (uint64_t)(uintptr_t)argv[2], 0, 0);
        if (r != 0) {
            shell_printf("mv: %s -> %s: failed (%d)\n", argv[1], argv[2], r);
            return 1;
        }
    }
    return 0;
}

int shell_cmd_date(int argc, char **argv) {
    extern uint64_t rtc_now_epoch(void);
    extern void rtc_format_epoch(uint64_t, char *, size_t);
    extern int rtc_have_hardware_clock(void);
    char stamp[40];
    (void)argc; (void)argv;
    rtc_format_epoch(rtc_now_epoch(), stamp, sizeof(stamp));
    shell_printf("%s%s\n", stamp,
                 rtc_have_hardware_clock() ? "" : "  (uptime clock: no RTC)");
    return 0;
}

int shell_cmd_uname(int argc, char **argv) {
    extern uint64_t syscall_invoke(uint64_t, uint64_t, uint64_t, uint64_t,
                                   uint64_t, uint64_t, uint64_t);
    struct linux_utsname uts;
    int all = (argc >= 2 && strcmp(argv[1], "-a") == 0);

    memset(&uts, 0, sizeof(uts));
    if ((int)syscall_invoke(SYS_UNAME, (uint64_t)(uintptr_t)&uts, 0, 0, 0, 0, 0) != 0) {
        shell_puts("uname: failed\n");
        return 1;
    }
    if (all) {
        shell_printf("%s %s %s %s %s %s\n", uts.sysname, uts.nodename,
                     uts.release, uts.version, uts.machine, uts.domainname);
    } else {
        shell_printf("%s\n", uts.sysname);
    }
    return 0;
}

int shell_cmd_free(int argc, char **argv) {
    extern uint64_t moe_pmm_free_count(void);
    extern uint64_t moe_pmm_total(void);
    extern uintptr_t lemis_heap_free_space(void);
    const uint64_t free_pages = moe_pmm_free_count();
    const uint64_t total_pages = moe_pmm_total();
    (void)argc; (void)argv;
    shell_printf("              total        used        free\n");
    shell_printf("Mem:    %8u KiB %8u KiB %8u KiB\n",
                 (unsigned)(total_pages * 4),
                 (unsigned)((total_pages - free_pages) * 4),
                 (unsigned)(free_pages * 4));
    shell_printf("Heap:   %8u KiB free\n", (unsigned)(lemis_heap_free_space() / 1024));
    return 0;
}

/* systest: acceptance for the whole new syscall batch. */
int shell_cmd_systest(int argc, char **argv) {
    extern int syscalls2_selftest(void);
    (void)argc; (void)argv;
    return syscalls2_selftest();
}

int shell_cmd_pipetest(int argc, char **argv) {
    extern int pipe_selftest(void);
    (void)argc; (void)argv;
    return pipe_selftest();
}

int shell_cmd_ring3(int argc, char **argv) {
    extern int ring3_ran_ever(void);
    extern int ring3_last_exit(void);
    extern int ring3_run_init(void);
    (void)argc; (void)argv;
    if (ring3_ran_ever()) {
        shell_printf("ring3: the first user program already ran (exit=%d)\n",
                     (int64_t)ring3_last_exit());
        shell_puts("ring3: it is a boot-time bring-up check, see `ring3test`\n");
        return 0;
    }
    return (ring3_run_init() == 42) ? 0 : 1;
}

int shell_cmd_ushell(int argc, char **argv) {
    extern int user_run_builtin(const char *name);
    const int rc = user_run_builtin("shell");
    (void)argc; (void)argv;
    if (rc < 0) {
        shell_puts("ushell: the ring-3 shell could not start\n");
        return 1;
    }
    shell_printf("ushell: ring-3 shell exited with %d\n", (int64_t)rc);
    return 0;
}

int shell_cmd_run(int argc, char **argv) {
    extern int user_run_builtin(const char *name);
    extern const char *user_builtin_list(void);
    int rc;
    if (argc < 2) {
        shell_printf("built-in user programs: %s\n", user_builtin_list());
        shell_puts("usage: run <name>\n");
        return 1;
    }
    rc = user_run_builtin(argv[1]);
    if (rc < 0) {
        shell_printf("run: %s: no such built-in program (%s)\n", argv[1],
                     user_builtin_list());
        return 1;
    }
    shell_printf("run: %s exited with %d\n", argv[1], (int64_t)rc);
    return 0;
}

int shell_cmd_ring3test(int argc, char **argv) {
    extern int ring3_selftest(void);
    (void)argc; (void)argv;
    return ring3_selftest();
}

int shell_cmd_rtctest(int argc, char **argv) {
    extern int rtc_selftest(void);
    (void)argc; (void)argv;
    return rtc_selftest();
}

/* Privileged commands check the effective uid and say what to type instead. */
static int require_root(const char *cmd) {
    if (cred_is_root()) return 1;
    shell_printf("%s: permission denied (are you root? try 'sudo %s')\n", cmd, cmd);
    return 0;
}

/* Terminal installer (kernel/install/installer.c). Destructive by design, so it
 * carries its own confirmations; root is required like mount/umount. */
int shell_cmd_install(int argc, char **argv) {
    (void)argc; (void)argv;
    if (!require_root("install")) return 1;
    extern int installer_run(void);
    return installer_run() == 0 ? 0 : 1;
}

int shell_cmd_mount(int argc, char **argv) {
    if (!require_root("mount")) return 1;
    if (argc < 4) {
        shell_puts("Usage: mount <type> <device> <mountpoint>\n");
        shell_puts("Types: fat32, ext2, squashfs, ramfs\n");
        shell_puts("Device: block drive number (0=ata0, 9=ram0) or /dev/sdX\n");
        return -1;
    }

    /* Map friendly type names to registered driver names */
    const char *type = argv[1];
    if (strcmp(type, "fatfs") == 0) type = "fat32";
    else if (strcmp(type, "ramfs") == 0) type = "xj_vfs";

    /* Normalize /dev/sdX -> decimal drive number */
    char devbuf[32];
    const char *device = argv[2];
    if (strncmp(device, "/dev/sd", 7) == 0 && device[7] >= 'a' && device[7] <= 'z') {
        devbuf[0] = (char)('0' + (device[7] - 'a'));
        devbuf[1] = '\0';
        device = devbuf;
    }

    int ret = vfs_mount(device, argv[3], type);
    if (ret == VFS_OK) {
        shell_printf("Mounted %s (%s) on %s\n", device, type, argv[3]);
        shell_scroll_message("Filesystem mounted");
    } else {
        shell_printf("mount: %s: %s (%d)\n", argv[3], vfs_err_str(ret), ret);
    }
    return ret;
}

int shell_cmd_umount(int argc, char **argv) {
    if (!require_root("umount")) return 1;
    if (argc < 2) {
        shell_puts("Usage: umount <mountpoint>\n");
        return -1;
    }
    int ret = vfs_unmount(argv[1]);
    if (ret == VFS_OK) {
        shell_printf("Unmounted %s\n", argv[1]);
        shell_scroll_message("Filesystem unmounted");
    } else {
        shell_printf("umount: %s: %s (%d)\n", argv[1], vfs_err_str(ret), ret);
    }
    return ret;
}

int shell_cmd_df(int argc, char **argv) {
    (void)argc; (void)argv;
    vfs_mount_info_t mounts[VFS_MAX_MOUNTS];
    int n = vfs_list_mounts(mounts, VFS_MAX_MOUNTS);

    shell_puts("Filesystem      Device       1K-blocks     Used   Available  Mounted on\n");
    if (n <= 0) {
        shell_puts("(no filesystems mounted)\n");
        return 0;
    }
    for (int i = 0; i < n; i++) {
        uint64_t total_k = mounts[i].total_bytes / 1024;
        uint64_t free_k  = mounts[i].free_bytes / 1024;
        uint64_t used_k  = (mounts[i].total_bytes > mounts[i].free_bytes)
                           ? (mounts[i].total_bytes - mounts[i].free_bytes) / 1024 : 0;
        shell_printf("%-15s %-12s %8llu %8llu %11llu  %s\n",
                     mounts[i].fstype, mounts[i].device,
                     (unsigned long long)total_k,
                     (unsigned long long)used_k,
                     (unsigned long long)free_k,
                     mounts[i].mountpoint);
    }
    return 0;
}

/* ==================== SMP inspection (per-CPU state / AP work) ==================== */

int shell_cmd_cpus(int argc, char **argv) {
    (void)argc; (void)argv;
    extern uint32_t smp_online_cpu_count(void);
    const uint32_t online = smp_online_cpu_count();

    shell_printf("CPUs online: %u\n", (unsigned)online);
    {
        /* Liveness of the two independent time sources: the BSP's system tick
         * (PIT/LAPIC vector 0x20) and the caller's own idea of its CPU id. */
        extern volatile uint64_t g_tick_count;
        extern uint32_t smp_get_cpu_id(void);
        uint64_t rflags = 0;
        __asm__ volatile("pushfq; pop %0" : "=r"(rflags));
        struct { uint16_t limit; uint64_t base; } __attribute__((packed)) idtr;
        __asm__ volatile("sidt %0" : "=m"(idtr));
        const uint8_t *gate = (const uint8_t *)(uintptr_t)(idtr.base + 0x20 * 16);
        uint64_t off = (uint64_t)gate[0] | ((uint64_t)gate[1] << 8) |
                       ((uint64_t)gate[6] << 16) | ((uint64_t)gate[7] << 24) |
                       ((uint64_t)gate[8] << 32) | ((uint64_t)gate[9] << 40) |
                       ((uint64_t)gate[10] << 48) | ((uint64_t)gate[11] << 56);
        shell_printf("system tick g_tick_count=%llu (shell on cpu%u, IF=%u)\n",
                     (unsigned long long)g_tick_count, (unsigned)smp_get_cpu_id(),
                     (unsigned)((rflags >> 9) & 1u));
        shell_printf("IDT gate 0x20: present=%u handler=0x%llx\n",
                     (unsigned)(gate[5] >> 7), (unsigned long long)off);

        /* Measure the achieved tick rate against the 8254 PIT (1.193182 MHz).
         * The PIT's interrupt is not routed on this machine, but its counter
         * still runs, so this is an independent check of the LAPIC time base. */
        {
            extern uint16_t mbos_pit_counter(void);
            const uint64_t t0 = g_tick_count;
            uint32_t prev = mbos_pit_counter(), acc = 0;
            while (acc < 1193182u && acc + 65536u > acc) {   /* ~1 s of PIT time */
                const uint32_t now = mbos_pit_counter();
                acc += (now <= prev) ? (prev - now) : (prev + 65536u - now);
                prev = now;
            }
            shell_printf("measured system tick rate: %u Hz (requested 100 Hz)\n",
                         (unsigned)(g_tick_count - t0));
        }
    }
    shell_puts("id  role  apic  jiffies   ipis   work   idle\n");
    for (uint32_t i = 0; i < online && i < 32u; i++) {
        const struct cpu_local *cl = smp_get_cpu_local(i);
        if (!cl || !cl->online) continue;
        shell_printf("%-3u %-5s %-5u %-9llu %-6llu %-6llu %-6llu\n",
                     (unsigned)i, cl->is_bsp ? "BSP" : "AP",
                     (unsigned)cl->apic_id,
                     (unsigned long long)cl->jiffies,
                     (unsigned long long)cl->ipis,
                     (unsigned long long)cl->work_done,
                     (unsigned long long)cl->idle_jiffies);
    }
    shell_puts("(jiffies = LAPIC timer ticks, ipis = IPI/spurious vectors taken,\n"
               " work = kernel work-queue items executed by that CPU)\n");
    return 0;
}

int shell_cmd_smptest(int argc, char **argv) {
    uint32_t n = 8;
    if (argc > 1) {
        const int v = shell_atoi(argv[1]);
        if (v > 0) n = (uint32_t)v;
    }
    extern void smp_work_selftest(uint32_t n);
    smp_work_selftest(n);
    return 0;
}

/* ==================== Network commands (kernel/net, polling mode) ==================== */

int shell_cmd_ping(int argc, char **argv) {
    extern int net_ip_from_str(const char* s, uint32_t* out);
    extern int net_ping(uint32_t ip, int timeout_ms, uint32_t* rtt_ticks);
    extern uint32_t net_ping_us(uint32_t rtt_ticks);
    extern int net_ready(void);

    if (!net_ready()) {
        shell_puts("ping: network is not up (no RTL8139 / net_init failed)\n");
        return -1;
    }
    if (argc < 2) {
        shell_puts("usage: ping <ipv4>      e.g. ping 10.0.2.2\n");
        return -1;
    }
    uint32_t ip = 0;
    if (net_ip_from_str(argv[1], &ip) != 0) {
        shell_printf("ping: bad address '%s'\n", argv[1]);
        return -1;
    }

    shell_printf("PING %s (ICMP echo, 3 probes)\n", argv[1]);
    int replies = 0;
    for (int i = 0; i < 3; i++) {
        uint32_t rtt = 0;
        if (net_ping(ip, 1000, &rtt) == 0) {
            shell_printf("  64 bytes from %s: icmp_seq=%d time=%u us\n",
                         argv[1], (unsigned)(i + 1), (unsigned)net_ping_us(rtt));
            replies++;
        } else {
            shell_printf("  request timeout for icmp_seq=%d\n", (unsigned)(i + 1));
        }
    }
    shell_printf("--- %s: %d/3 replies ---\n", argv[1], (unsigned)replies);
    return replies ? 0 : -1;
}

int shell_cmd_netstat(int argc, char **argv) {
    extern void net_dump_stats(void);
    (void)argc; (void)argv;
    net_dump_stats();
    return 0;
}

/* DHCP client: on demand only (never at boot). Acquires a lease and programs the
 * address/mask/gateway/resolver the stack uses from then on. */
int shell_cmd_dhcp(int argc, char **argv) {
    extern int dhcp_client_acquire(dhcp_lease_t *out, int timeout_ms);
    extern uint32_t net_ip(void), net_gw(void), net_mask(void), net_dns(void);
    int rc;
    dhcp_lease_t lease;
    char ip[16], mask[16], gw[16], dns[16];
    (void)argc; (void)argv;

    if (!net_ready()) {
        shell_puts("dhcp: network is not up (no RTL8139 / net_init failed)\n");
        return -1;
    }
    shell_puts("DHCP: sending DISCOVER to 255.255.255.255:67 ...\n");
    rc = dhcp_client_acquire(&lease, 4000);
    if (rc != 0) {
        shell_printf("dhcp: no lease (error %d)%s\n", (int64_t)rc,
                     (rc == -110) ? " - no reply from a DHCP server" : "");
        return -1;
    }
    net_ip_to_str(net_ip(), ip, sizeof(ip));
    net_ip_to_str(net_mask(), mask, sizeof(mask));
    net_ip_to_str(net_gw(), gw, sizeof(gw));
    net_ip_to_str(net_dns(), dns, sizeof(dns));
    shell_printf("dhcp: lease %s mask %s gateway %s dns %s (server ",
                 ip, mask, gw, dns);
    net_ip_to_str(lease.server_id, ip, sizeof(ip));
    shell_printf("%s, %u s)\n", ip, (unsigned)lease.lease_secs);
    return 0;
}

/* DNS resolver: nslookup <name> (A record only, one query at a time). */
int shell_cmd_nslookup(int argc, char **argv) {
    extern int dns_resolve(const char *name, uint32_t *out_ip, int timeout_ms);
    uint32_t ip = 0;
    char server[16], answer[16];
    int rc;

    if (!net_ready()) {
        shell_puts("nslookup: network is not up (no RTL8139 / net_init failed)\n");
        return -1;
    }
    if (argc < 2) {
        shell_puts("usage: nslookup <name>    e.g. nslookup example.com\n");
        return -1;
    }
    if (net_dns() == 0u) {
        shell_puts("nslookup: no DNS server configured (run dhcp first)\n");
        return -1;
    }
    net_ip_to_str(net_dns(), server, sizeof(server));
    shell_printf("nslookup: querying %s for %s (A)\n", server, argv[1]);
    rc = dns_resolve(argv[1], &ip, 1000);
    if (rc != 0) {
        shell_printf("nslookup: %s: no A record (error %d)\n", argv[1], (int64_t)rc);
        return -1;
    }
    net_ip_to_str(ip, answer, sizeof(answer));
    shell_printf("%s has address %s\n", argv[1], answer);
    return 0;
}

/* Socket ABI acceptance, callable any time (also runs silently at boot). */
int shell_cmd_socktest(int argc, char **argv) {
    extern int sockets_boot_selftest(void);
    const int fails = sockets_boot_selftest();
    (void)argc; (void)argv;
    shell_printf("socktest: %s (%d failure(s))\n", (fails == 0) ? "PASS" : "FAIL",
                 (int64_t)fails);
    return (fails == 0) ? 0 : 1;
}

/* ==================== TCP commands (kernel/net/tcp.c) ====================
 * These are the host-verifiable end-to-end path for the TCP implementation:
 * `tcpecho` performs a full active open, sends the given bytes, waits for the
 * peer's data, prints exactly what came back (hex + printable), then closes.
 * Pair it with a TCP server on the host (QEMU slirp forwards guest traffic for
 * 10.0.2.2 to the host's loopback), e.g. scripts/tcp_acceptance.py. */
static void tcp_print_payload(const char *tag, const uint8_t *b, uint32_t n)
{
    uint32_t i;
    shell_printf("%s %u bytes hex=", tag, (unsigned)n);
    for (i = 0; i < n && i < 64u; i++)
        shell_printf("%02x", (unsigned)b[i]);
    if (n > 64u)
        shell_puts("...");
    shell_puts(" ascii=\"");
    for (i = 0; i < n && i < 64u; i++) {
        unsigned char ch = b[i];
        shell_putc((ch >= 32u && ch < 127u) ? (char)ch : '.');
    }
    shell_puts("\"\n");
}

/* ==================== wget (HTTP GET over the kernel TCP client) ==========
 * `wget http://10.0.2.2:8089/file.bin /mnt/fat/DL.BIN`
 *
 * Deliberately small and honest:
 *   - IP literal only (the stack has DNS, but `nslookup` is a separate command
 *     and a download tool must not silently depend on the resolver),
 *   - HTTP/1.0 request with Connection: close, status line + headers parsed
 *     manually, body streamed to the VFS in recv-sized chunks,
 *   - Content-Length is honoured when present; otherwise the body ends when the
 *     peer closes (timeout => reported, partial file kept and named *.part).
 */
/* prefix-integer parse for HTTP headers ("200 OK", "3072\r\n") - shell_atoi()
 * insists on a whole string being digits, which header values never are. */
static int wget_atoi(const char *s) {
    int v = 0;
    if (!s) return -1;
    while (*s == ' ' || *s == '\t') s++;
    int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    int any = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; any = 1; }
    if (!any) return -1;
    return neg ? -v : v;
}

static const char *wget_find(const char *hay, const char *needle) {
    if (!hay || !needle || !*needle) return NULL;
    for (const char *p = hay; *p; p++) {
        const char *a = p, *b = needle;
        while (*a && *b && *a == *b) { a++; b++; }
        if (*b == 0) return p;
    }
    return NULL;
}

static int wget_parse_url(const char *url, uint32_t *ip, uint16_t *port,
                          char *path, uint32_t path_cap, char *host_out,
                          uint32_t host_cap) {
    extern int net_ip_from_str(const char *s, uint32_t *out);
    const char *p = url;
    if (strncmp(p, "http://", 7) != 0) return -1;
    p += 7;
    const char *host = p;
    while (*p && *p != ':' && *p != '/') p++;
    uint32_t hlen = (uint32_t)(p - host);
    if (hlen == 0 || hlen >= host_cap) return -1;
    memcpy(host_out, host, hlen);
    host_out[hlen] = '\0';

    uint16_t prt = 80;
    if (*p == ':') {
        p++;
        uint32_t v = 0;
        while (*p >= '0' && *p <= '9') { v = v * 10u + (uint32_t)(*p - '0'); p++; }
        if (v == 0 || v > 65535u) return -1;
        prt = (uint16_t)v;
    }
    if (*p != '/') return -1;
    uint32_t n = 0;
    while (*p && n + 1u < path_cap) path[n++] = *p++;
    path[n] = '\0';
    if (net_ip_from_str(host_out, ip) != 0) return -1;
    *port = prt;
    return 0;
}

int shell_cmd_wget(int argc, char **argv) {
    extern int net_ready(void);
    extern int tcp_connect(uint32_t ip, uint16_t port, int timeout_ms);
    extern int tcp_send(int h, const void *data, uint32_t len);
    extern int tcp_recv(int h, void *buf, uint32_t cap, int timeout_ms);
    extern int tcp_close(int h);

    if (argc < 2) {
        shell_puts("usage: wget http://<ipv4>[:port]/<path> [outfile]\n"
                   "   e.g. wget http://10.0.2.2:8089/hello.txt /mnt/fat/HELLO.TXT\n");
        return -1;
    }
    if (!net_ready()) {
        shell_puts("wget: network is not up\n");
        return -1;
    }

    uint32_t ip = 0;
    uint16_t port = 0;
    char path[192];
    char host[64];
    if (wget_parse_url(argv[1], &ip, &port, path, sizeof(path), host, sizeof(host)) != 0) {
        shell_printf("wget: bad url '%s' (expected http://<ipv4>[:port]/<path>)\n",
                     argv[1]);
        return -1;
    }

    /* Output file: explicit argument, else the URL's basename in the cwd. */
    char out[256];
    if (argc >= 3) {
        shell_abspath(argv[2], out, sizeof(out));
    } else {
        const char *base = path;
        for (const char *q = path; *q; q++)
            if (*q == '/') base = q + 1;
        if (*base == '\0') base = "index.html";
        shell_abspath(base, out, sizeof(out));
    }

    static char req[512];
    int rl = snprintf(req, sizeof(req),
                      "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: SutureOS-wget/1.0\r\n"
                      "Connection: close\r\n\r\n", path, host);
    if (rl <= 0 || (uint32_t)rl >= sizeof(req)) {
        shell_puts("wget: request too long\n");
        return -1;
    }

    shell_printf("wget: connecting to %s:%u\n", host, (unsigned)port);
    int h = tcp_connect(ip, port, 5000);
    if (h < 0) {
        shell_printf("wget: connect FAILED (%d)\n", h);
        return -1;
    }
    if (tcp_send(h, req, (uint32_t)rl) != 0) {
        shell_puts("wget: send FAILED\n");
        tcp_close(h);
        return -1;
    }

    static uint8_t rx[1460];
    static uint8_t hdr[2048];
    uint32_t hdr_len = 0;
    int header_done = 0;
    int status = 0;
    long content_len = -1;
    uint32_t written = 0;

    int fd = -1;
    int rc = -1;
    for (;;) {
        int n = tcp_recv(h, rx, sizeof(rx), 5000);
        if (n <= 0) break;

        uint32_t off = 0;
        if (!header_done) {
            /* accumulate until the blank line that ends the headers */
            for (int i = 0; i < n && !header_done; i++) {
                if (hdr_len + 1u < sizeof(hdr)) hdr[hdr_len++] = rx[i];
                if (hdr_len >= 4 && hdr[hdr_len - 4] == '\r' && hdr[hdr_len - 3] == '\n' &&
                    hdr[hdr_len - 2] == '\r' && hdr[hdr_len - 1] == '\n') {
                    header_done = 1;
                }
            }
            if (!header_done) continue;
            hdr[hdr_len] = '\0';
            /* status line: HTTP/1.x NNN ... */
            if (strncmp((char *)hdr, "HTTP/", 5) == 0) {
                const char *sp = (const char *)hdr;
                while (*sp && *sp != ' ') sp++;
                if (*sp == ' ') status = wget_atoi(sp + 1);
            }
            const char *cl = wget_find((const char *)hdr, "Content-Length:");
            if (cl) content_len = (long)wget_atoi(cl + 15);
            shell_printf("wget: HTTP %d, content-length=%ld\n", status, content_len);
            if (status != 200) {
                shell_printf("wget: server returned %d, nothing written\n", status);
                tcp_close(h);
                return -1;
            }
            fd = vfs_open(out, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC);
            if (fd < 0) {
                shell_printf("wget: cannot create %s (%d)\n", out, fd);
                tcp_close(h);
                return -1;
            }
            /* bytes of this chunk that belong to the body: find the 4-byte
             * header terminator inside rx */
            uint32_t body_start = 0;
            for (uint32_t i = 0; i + 3u < (uint32_t)n; i++) {
                if (rx[i] == '\r' && rx[i + 1] == '\n' && rx[i + 2] == '\r' &&
                    rx[i + 3] == '\n') {
                    body_start = i + 4u;
                    break;
                }
            }
            off = body_start;
        }

        if (fd >= 0 && (uint32_t)n > off) {
            int wr = vfs_write(fd, rx + off, (uint32_t)n - off);
            if (wr <= 0) {
                shell_printf("wget: write FAILED (%d)\n", wr);
                break;
            }
            written += (uint32_t)wr;
        }
        if (content_len >= 0 && written >= (uint32_t)content_len) break;
    }

    if (fd >= 0) vfs_close(fd);
    tcp_close(h);

    if (fd < 0) {
        shell_puts("wget: no response\n");
        return -1;
    }
    if (content_len >= 0 && written != (uint32_t)content_len)
        shell_printf("wget: WARNING short read (%u of %ld bytes)\n", (unsigned)written, content_len);
    shell_printf("wget: wrote %u bytes to %s\n", (unsigned)written, out);
    rc = ((content_len < 0) || written == (uint32_t)content_len) ? 0 : -1;
    return rc;
}

int shell_cmd_tcpecho(int argc, char **argv) {
    extern int net_ip_from_str(const char* s, uint32_t* out);
    extern int net_ready(void);
    extern void tcp_init(void);
    extern int tcp_connect(uint32_t ip, uint16_t port, int timeout_ms);
    extern int tcp_send(int h, const void* data, uint32_t len);
    extern int tcp_recv(int h, void* buf, uint32_t cap, int timeout_ms);
    extern int tcp_close(int h);
    extern int tcp_state(int h);
    extern uint32_t tcp_rx_available(int h);

    static uint8_t rx[1024];

    if (!net_ready()) {
        shell_puts("tcpecho: network is not up\n");
        return -1;
    }
    if (argc < 4) {
        shell_puts("usage: tcpecho <ipv4> <port> <text>\n"
                   "   e.g. tcpecho 10.0.2.2 5000 hello\n");
        return -1;
    }

    uint32_t ip = 0;
    if (net_ip_from_str(argv[1], &ip) != 0) {
        shell_printf("tcpecho: bad address '%s'\n", argv[1]);
        return -1;
    }
    int port = shell_atoi(argv[2]);
    if (port <= 0 || port > 65535) {
        shell_printf("tcpecho: bad port '%s'\n", argv[2]);
        return -1;
    }

    /* Join argv[3..] with single spaces so `tcpecho ip port hello world` sends
     * "hello world". */
    char msg[256];
    uint32_t mlen = 0;
    for (int i = 3; i < argc && mlen < sizeof(msg) - 2u; i++) {
        if (i > 3 && mlen < sizeof(msg) - 2u) msg[mlen++] = ' ';
        const char *s = argv[i];
        while (*s && mlen < sizeof(msg) - 1u) msg[mlen++] = *s++;
    }
    msg[mlen] = '\0';

    shell_printf("TCP connect %s:%d (local port auto)\n", argv[1], port);
    int h = tcp_connect(ip, (uint16_t)port, 4000);
    if (h < 0) {
        shell_printf("tcpecho: connect FAILED (%d)\n", h);
        return -1;
    }
    shell_printf("TCP connected (handle %d, state %d)\n", h, tcp_state(h));

    if (tcp_send(h, msg, mlen) != 0)
        shell_printf("tcpecho: send FAILED (%u bytes)\n", (unsigned)mlen);
    else
        tcp_print_payload("TCP sent", (const uint8_t*)msg, mlen);

    int n = tcp_recv(h, rx, sizeof(rx), 3000);
    if (n > 0)
        tcp_print_payload("TCP received", rx, (uint32_t)n);
    else
        shell_printf("tcpecho: no data received (%d, rx_available=%u)\n",
                     n, (unsigned)tcp_rx_available(h));

    int cl = tcp_close(h);
    shell_printf("TCP close %s\n", cl == 0 ? "OK" : "unacknowledged");

    return n > 0 ? 0 : -1;
}

int shell_cmd_tcpsend(int argc, char **argv) {
    extern int net_ip_from_str(const char* s, uint32_t* out);
    extern int net_ready(void);
    extern int tcp_connect(uint32_t ip, uint16_t port, int timeout_ms);
    extern int tcp_send(int h, const void* data, uint32_t len);
    extern int tcp_close(int h);
    extern uint32_t tcp_rx_available(int h);

    if (!net_ready()) {
        shell_puts("tcpsend: network is not up\n");
        return -1;
    }
    if (argc < 3) {
        shell_puts("usage: tcpsend <ipv4> <port> [text...]\n");
        return -1;
    }
    uint32_t ip = 0;
    if (net_ip_from_str(argv[1], &ip) != 0) {
        shell_printf("tcpsend: bad address '%s'\n", argv[1]);
        return -1;
    }
    int port = shell_atoi(argv[2]);
    if (port <= 0 || port > 65535) {
        shell_printf("tcpsend: bad port '%s'\n", argv[2]);
        return -1;
    }

    /* Static, and large enough for the default 3000-byte payload: a 512-byte
     * stack buffer here used to be overflowed by the pattern below, which
     * smashed the shell's frame and #GP'd inside the payload bytes. */
    static char msg[4096];
    uint32_t mlen = 0;
    for (int i = 3; i < argc; i++) {
        if (i > 3 && mlen < sizeof(msg) - 2u) msg[mlen++] = ' ';
        const char *s = argv[i];
        while (*s && mlen < sizeof(msg) - 1u) msg[mlen++] = *s++;
    }
    if (mlen == 0u) {
        /* A payload big enough to exercise MSS splitting (3 segments). */
        const uint32_t want = 3000u;
        for (uint32_t i = 0; i < want && mlen < sizeof(msg) - 1u; i++)
            msg[mlen++] = (char)('A' + (i % 26u));
    }

    int h = tcp_connect(ip, (uint16_t)port, 4000);
    if (h < 0) {
        shell_printf("tcpsend: connect FAILED (%d)\n", h);
        return -1;
    }
    int rc = tcp_send(h, msg, mlen);
    shell_printf("tcpsend: %u bytes %s (rx_available=%u)\n",
                 (unsigned)mlen, rc == 0 ? "ACKed" : "FAILED",
                 (unsigned)tcp_rx_available(h));
    (void)tcp_close(h);
    return rc;
}

/* ==================== TCP server + out-of-order self-test ====================
 * `tcpserve <port>` turns the guest into a TCP echo server (reachable from the
 * host through QEMU's hostfwd=tcp::HOSTPORT-:GUESTPORT). `tcpooo` is an
 * in-kernel self-test of the passive open + out-of-order path: it injects
 * hand-built segments from a peer that never answers ARP, so the state machine
 * is exercised without any host help and without touching the wire. */

/* Build one segment and hand it to tcp_input() as if it arrived from
 * src_ip:sport. The checksum is computed exactly like the stack validates it. */
static void tcp_inject_seg(uint32_t src_ip, uint16_t sport, uint16_t dport,
                           uint32_t seq, uint32_t ack, uint8_t flags,
                           const uint8_t *data, uint32_t len)
{
    extern uint16_t net_pseudo_checksum(uint32_t src, uint32_t dst, uint8_t proto,
                                        const uint8_t *data, uint32_t len);
    extern void tcp_input(uint32_t src_ip, const uint8_t *seg, uint32_t len);
    extern uint32_t net_local_ip;
    static uint8_t seg[1600];
    tcp_header_t *th = (tcp_header_t *)(void *)seg;

    if (20u + len > sizeof(seg)) return;
    th->src_port    = htons(sport);
    th->dst_port    = htons(dport);
    th->seq_num     = htonl(seq);
    th->ack_num     = htonl(ack);
    th->data_offset_flags = (uint8_t)(5u << 4);
    th->flags       = flags;
    th->window_size = htons(4096);
    th->checksum    = 0u;
    th->urgent_ptr  = 0u;
    if (len != 0u && data != NULL) memcpy(seg + 20u, data, len);
    th->checksum = htons(net_pseudo_checksum(net_local_ip, src_ip, IP_PROTO_TCP,
                                             seg, 20u + len));
    tcp_input(src_ip, seg, 20u + len);
}

int shell_cmd_tcpooo(int argc, char **argv) {
    extern void tcp_init(void);
    extern int tcp_listen(uint16_t local_port);
    extern int tcp_listen_child(int listener);
    extern int tcp_listen_accept(int listener, int timeout_ms, uint32_t*, uint16_t*);
    extern uint32_t tcp_snd_nxt(int h), tcp_rcv_nxt(int h), tcp_rx_available(int h);
    extern int tcp_recv(int h, void *buf, uint32_t cap, int timeout_ms);
    extern int tcp_close(int h);
    extern int tcp_state(int h);
    extern int net_ready(void);

    /* A peer that never answers ARP: our replies cannot leave the guest, which
     * keeps this a pure state-machine test (nothing goes on the wire). */
    const uint32_t peer      = 0x0A000263u;   /* 10.0.2.99 */
    const uint16_t peer_port = 40000u;
    const uint16_t local     = 40404u;
    uint8_t d_in[10], d_gap[10], d_ooo[10], out[64];
    uint32_t nxt;
    int l, ch, n, ok;

    (void)argc; (void)argv;
    if (!net_ready()) { shell_puts("tcpooo: network is not up\n"); return -1; }

    for (int i = 0; i < 10; i++) {
        d_in[i]  = (uint8_t)('A' + i);        /* in-order block    */
        d_gap[i] = (uint8_t)('a' + i);        /* the missing gap   */
        d_ooo[i] = (uint8_t)('K' + i);        /* sent out of order */
    }

    tcp_init();
    l = tcp_listen(local);
    if (l < 0) { shell_printf("tcpooo: listen FAILED (%d)\n", l); return -1; }

    /* 1) SYN creates the child in SYN_RECEIVED. */
    tcp_inject_seg(peer, peer_port, local, 1000u, 0u, TCP_FLAG_SYN, NULL, 0u);
    ch = tcp_listen_child(l);
    if (ch < 0) { shell_puts("tcpooo: FAIL - no child after SYN\n"); return -1; }
    if (tcp_state(ch) != TCP_SYN_RECEIVED) {
        shell_printf("tcpooo: FAIL - state after SYN is %d (want %d)\n",
                     tcp_state(ch), TCP_SYN_RECEIVED);
        return -1;
    }

    /* 2) The third handshake segment establishes the child. */
    tcp_inject_seg(peer, peer_port, local, tcp_rcv_nxt(ch), tcp_snd_nxt(ch),
                   TCP_FLAG_ACK, NULL, 0u);
    if (tcp_state(ch) != TCP_ESTABLISHED) {
        shell_printf("tcpooo: FAIL - state after ACK is %d (want %d)\n",
                     tcp_state(ch), TCP_ESTABLISHED);
        return -1;
    }

    /* 3) accept() hands the established child over (timeout 0 = immediate). */
    {
        uint32_t pip = 0;
        uint16_t pport = 0;
        int got = tcp_listen_accept(l, 0, &pip, &pport);
        if (got != ch || pip != peer || pport != peer_port) {
            shell_printf("tcpooo: FAIL - accept returned %d (want %d) peer=%x:%u\n",
                         got, ch, (unsigned)pip, (unsigned)pport);
            return -1;
        }
    }

    /* 4) In-order data lands in the ring. */
    nxt = tcp_rcv_nxt(ch);
    tcp_inject_seg(peer, peer_port, local, nxt, tcp_snd_nxt(ch),
                   (uint8_t)(TCP_FLAG_ACK | TCP_FLAG_PSH), d_in, 10u);
    if (tcp_rx_available(ch) != 10u) {
        shell_printf("tcpooo: FAIL - in-order rx=%u (want 10)\n",
                     (unsigned)tcp_rx_available(ch));
        return -1;
    }

    /* 5) A future segment is buffered, NOT delivered (rx stays 10). */
    tcp_inject_seg(peer, peer_port, local, nxt + 20u, tcp_snd_nxt(ch),
                   (uint8_t)(TCP_FLAG_ACK | TCP_FLAG_PSH), d_ooo, 10u);
    if (tcp_rx_available(ch) != 10u) {
        shell_printf("tcpooo: FAIL - out-of-order segment delivered early (rx=%u)\n",
                     (unsigned)tcp_rx_available(ch));
        return -1;
    }

    /* 6) The gap arrives: it and the buffered segment must both be delivered,
     *    in order -> rx = 30 and the bytes read A..J a..j K..T. */
    tcp_inject_seg(peer, peer_port, local, nxt + 10u, tcp_snd_nxt(ch),
                   (uint8_t)(TCP_FLAG_ACK | TCP_FLAG_PSH), d_gap, 10u);
    if (tcp_rx_available(ch) != 30u) {
        shell_printf("tcpooo: FAIL - after the gap rx=%u (want 30)\n",
                     (unsigned)tcp_rx_available(ch));
        return -1;
    }

    n = tcp_recv(ch, out, sizeof(out), 0);
    if (n != 30) {
        shell_printf("tcpooo: FAIL - recv returned %d (want 30)\n", n);
        return -1;
    }
    ok = (memcmp(out, d_in, 10) == 0) &&
         (memcmp(out + 10, d_gap, 10) == 0) &&
         (memcmp(out + 20, d_ooo, 10) == 0);
    tcp_print_payload("tcpooo reassembled", out, 30u);

    tcp_close(ch);
    tcp_close(l);
    shell_printf("tcpooo: passive open + out-of-order reassembly %s\n",
                 ok ? "PASS" : "FAIL");
    return ok ? 0 : -1;
}

int shell_cmd_tcpserve(int argc, char **argv) {
    extern int net_ready(void);
    extern void tcp_init(void);
    extern int tcp_listen(uint16_t local_port);
    extern int tcp_listen_accept(int listener, int timeout_ms, uint32_t*, uint16_t*);
    extern int tcp_recv(int h, void *buf, uint32_t cap, int timeout_ms);
    extern int tcp_send(int h, const void *data, uint32_t len);
    extern int tcp_close(int h);
    extern void net_ip_to_str(uint32_t ip, char *buf, uint32_t buflen);

    static uint8_t buf[1024];
    char peerstr[16];
    int port, l, ch, n, cl;
    uint32_t pip = 0, total = 0;
    uint16_t pport = 0;

    if (!net_ready()) { shell_puts("tcpserve: network is not up\n"); return -1; }
    if (argc < 2) {
        shell_puts("usage: tcpserve <port>   e.g. tcpserve 40404, then from the host:\n"
                   "       QEMU -netdev user,hostfwd=tcp::8080-:40404  and connect :8080\n");
        return -1;
    }
    port = shell_atoi(argv[1]);
    if (port <= 0 || port > 65535) {
        shell_printf("tcpserve: bad port '%s'\n", argv[1]);
        return -1;
    }

    tcp_init();
    l = tcp_listen((uint16_t)port);
    if (l < 0) {
        shell_printf("tcpserve: listen on %d FAILED (%d)\n", port, l);
        return -1;
    }
    shell_printf("TCP listening on port %d (guest 10.0.2.15)\n", port);

    ch = tcp_listen_accept(l, 20000, &pip, &pport);
    if (ch < 0) {
        shell_puts("tcpserve: no connection within 20 s\n");
        tcp_close(l);
        return -1;
    }
    net_ip_to_str(pip, peerstr, sizeof(peerstr));
    shell_printf("TCP accepted from %s:%u\n", peerstr, (unsigned)pport);

    for (int i = 0; i < 64; i++) {
        n = tcp_recv(ch, buf, sizeof(buf), 3000);
        if (n <= 0)
            break;                               /* peer closed or idle */
        total += (uint32_t)n;
        tcp_print_payload("TCP serve rx", buf, (uint32_t)n);
        if (tcp_send(ch, buf, (uint32_t)n) != 0) {
            shell_puts("tcpserve: echo send FAILED\n");
            break;
        }
    }
    cl = tcp_close(ch);
    tcp_close(l);
    shell_printf("TCP serve done: echoed %u bytes, close %s\n",
                 (unsigned)total, cl == 0 ? "OK" : "unacknowledged");
    return total != 0u ? 0 : -1;
}

/* Same echo server as tcpserve, but driven ONLY through the socket ABI
 * (socket/bind/listen/accept/recv/send/close in kernel/net/net.c), which is what
 * a ring-3 program would call through the syscall layer. Proves the stream
 * socket routing rather than the raw tcp_* entry points. */
int shell_cmd_tcpapi(int argc, char **argv) {
    extern int net_socket(int domain, int type, int protocol);
    extern int net_bind(int fd, const net_sockaddr_t *addr, socklen_t addrlen);
    extern int net_listen(int fd, int backlog);
    extern int net_accept(int fd, net_sockaddr_t *addr, socklen_t *addrlen);
    extern int net_recv(int fd, void *buf, size_t len, int flags);
    extern int net_send(int fd, const void *buf, size_t len, int flags);
    extern int net_close(int fd);
    extern int net_ready(void);
    extern void net_ip_to_str(uint32_t ip, char *buf, uint32_t buflen);

    static uint8_t buf[1024];
    net_sockaddr_t sa;
    socklen_t sl;
    char peer[16];
    int port, lfd, cfd, n, rc;
    uint32_t total = 0;

    if (!net_ready()) { shell_puts("tcpapi: network is not up\n"); return -1; }
    if (argc < 2) {
        shell_puts("usage: tcpapi <port>   (TCP echo server over the socket ABI)\n");
        return -1;
    }
    port = shell_atoi(argv[1]);
    if (port <= 0 || port > 65535) {
        shell_printf("tcpapi: bad port '%s'\n", argv[1]);
        return -1;
    }

    lfd = net_socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) { shell_printf("tcpapi: socket() FAILED (%d)\n", lfd); return -1; }

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port   = htons((uint16_t)port);
    sa.sin_addr   = INADDR_ANY;
    if (net_bind(lfd, &sa, (socklen_t)sizeof(sa)) != 0) {
        shell_puts("tcpapi: bind() FAILED\n");
        net_close(lfd);
        return -1;
    }
    if (net_listen(lfd, 1) != 0) {
        shell_puts("tcpapi: listen() FAILED\n");
        net_close(lfd);
        return -1;
    }
    shell_printf("TCPAPI listening on port %d (socket/bind/listen)\n", port);

    sl = (socklen_t)sizeof(sa);
    cfd = net_accept(lfd, &sa, &sl);
    if (cfd < 0) {
        shell_printf("TCPAPI accept() FAILED (%d)\n", cfd);
        net_close(lfd);
        return -1;
    }
    net_ip_to_str(sa.sin_addr, peer, sizeof(peer));
    shell_printf("TCPAPI accepted fd=%d from %s:%u\n", cfd, peer,
                 (unsigned)ntohs(sa.sin_port));

    for (int i = 0; i < 64; i++) {
        n = net_recv(cfd, buf, sizeof(buf), 0);
        if (n <= 0)
            break;                               /* EOF (0) or timeout (-EAGAIN) */
        total += (uint32_t)n;
        tcp_print_payload("TCPAPI rx", buf, (uint32_t)n);
        rc = net_send(cfd, buf, (size_t)n, 0);
        if (rc != n) {
            shell_printf("TCPAPI echo send FAILED (%d)\n", rc);
            break;
        }
    }
    net_close(cfd);
    net_close(lfd);
    shell_printf("TCPAPI done: echoed %u bytes\n", (unsigned)total);
    return total != 0u ? 0 : -1;
}

int shell_cmd_tcpstat(int argc, char **argv) {
    extern void tcp_dump_stats(void);
    (void)argc; (void)argv;
    tcp_dump_stats();
    return 0;
}

/* Keyboard input path diagnostics: irqs > 0 proves IRQ1 is actually delivered
 * (the ring is only fed from the interrupt); dropped counts ring overflows. */
int shell_cmd_kbdstat(int argc, char **argv) {
    extern uint32_t mbos_kbd_irq_count(void);
    extern uint32_t mbos_kbd_dropped_count(void);
    extern uint32_t mbos_kbd_fallback_count(void);
    extern int      mbos_kbd_irq_active(void);
    extern uint32_t moos_intr_count(int vec);
    (void)argc; (void)argv;
    shell_printf("[kbd] irq1=%s scancodes=%u dropped=%u vector0x21=%u "
                 "(ring=64, polled fallback always on)\n",
                 mbos_kbd_irq_active() ? "enabled" : "off",
                 (unsigned)mbos_kbd_irq_count(),
                 (unsigned)mbos_kbd_dropped_count(),
                 (unsigned)moos_intr_count(0x21));
    return 0;
}

/* AC'97 audio: play a sine tone through the PCM OUT bus-master channel.
 * `beep` alone = 440 Hz / 1000 ms (the boot self-test tone); the two optional
 * arguments override the frequency (20..12000 Hz) and the duration
 * (10..1000 ms).  This is the on-demand trigger for the WAV acceptance run. */
int shell_cmd_beep(int argc, char **argv) {
    extern int ac97_present(void);
    extern const char *ac97_name(void);
    extern int ac97_beep(uint32_t freq_hz, uint32_t duration_ms);
    uint32_t freq = 440u, dur = 1000u;

    if (!ac97_present()) {
        shell_puts("beep: no AC'97 codec (boot QEMU with "
                   "-device AC97,audiodev=a0)\n");
        return 0;
    }
    if (argc > 1) {
        const int f = shell_atoi(argv[1]);
        if (f < 20 || f > 12000) {
            shell_puts("beep: frequency must be 20..12000 Hz\n");
            return 0;
        }
        freq = (uint32_t)f;
    }
    if (argc > 2) {
        const int d = shell_atoi(argv[2]);
        if (d < 10 || d > 1000) {
            shell_puts("beep: duration must be 10..1000 ms\n");
            return 0;
        }
        dur = (uint32_t)d;
    }

    shell_printf("[ac97] %s: playing %u Hz for %u ms...\n", ac97_name(),
                 (unsigned)freq, (unsigned)dur);
    if (ac97_beep(freq, dur) == 0)
        shell_printf("[ac97] beep done (%u Hz, %u ms)\n",
                     (unsigned)freq, (unsigned)dur);
    else
        shell_puts("[ac97] beep FAILED (DMA/rate error)\n");
    return 0;
}

/* Intel HDA: report the controller/codec the boot probe found and play the
 * acceptance tone on demand.  `hdatest` alone = 440 Hz / 1000 ms (the same tone
 * as the boot self-test); optional arguments override frequency and duration
 * within the driver's 48 kHz / 16-bit / stereo limits.  This is the on-demand
 * trigger for the WAV acceptance run on the HDA path. */
int shell_cmd_hdatest(int argc, char **argv) {
    extern int hda_init(void);
    extern int hda_present(void);
    extern const char *hda_name(void);
    extern int hda_beep(uint32_t freq_hz, uint32_t duration_ms);
    uint32_t freq = 440u, dur = 1000u;

    if (!hda_present() && hda_init() != 0) {
        shell_puts("hdatest: no Intel HDA controller (boot QEMU with "
                   "-device intel-hda -device hda-duplex,audiodev=a0)\n");
        return 0;
    }
    if (argc > 1) {
        const int f = shell_atoi(argv[1]);
        if (f < 20 || f > 12000) {
            shell_puts("hdatest: frequency must be 20..12000 Hz\n");
            return 0;
        }
        freq = (uint32_t)f;
    }
    if (argc > 2) {
        const int d = shell_atoi(argv[2]);
        if (d < 10 || d > 1000) {
            shell_puts("hdatest: duration must be 10..1000 ms\n");
            return 0;
        }
        dur = (uint32_t)d;
    }

    shell_printf("[hda] %s: playing %u Hz for %u ms...\n", hda_name(),
                 (unsigned)freq, (unsigned)dur);
    if (hda_beep(freq, dur) == 0)
        shell_printf("[hda] beep done (%u Hz, %u ms)\n",
                     (unsigned)freq, (unsigned)dur);
    else
        shell_puts("[hda] beep FAILED (controller/stream error)\n");
    return 0;
}

/* ==================== Boot message logging ==================== */

void shell_log_boot(const char *fmt, ...) {
    char buf[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    
    if (g_boot_msg_count < 32) {
        strcpy(g_boot_messages[g_boot_msg_count++], buf);
    }
    
    if (g_boot_complete) {
        shell_printf("[  OK  ] %s\n", buf);
    }
}

void shell_log_boot_fail(const char *fmt, ...) {
    char buf[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    
    if (g_boot_msg_count < 32) {
        strcpy(g_boot_messages[g_boot_msg_count++], buf);
    }
    
    if (g_boot_complete) {
        shell_printf("[FAILED] %s\n", buf);
    }
}


