/* SutureOS: Simple Terminal Shell Implementation
 * Minimal terminal emulator with command line editing, history, and builtins
 * Provides Linux-like terminal interface with black background, white text
 */
#include "shell.h"
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
    {"ps",      shell_cmd_ps,      "List processes"},
    {"kill",    shell_cmd_kill,    "Kill a process"},
    {"sudo",    shell_cmd_sudo,    "Execute command as root"},
    {"mount",   shell_cmd_mount,   "Mount filesystem"},
    {"umount",  shell_cmd_umount,  "Unmount filesystem"},
    {"df",      shell_cmd_df,      "Show disk usage"},
    {"cpus",    shell_cmd_cpus,    "Show per-CPU state (jiffies, IPIs, work done)"},
    {"smptest", shell_cmd_smptest, "Queue work items and report which CPU ran them" },
    {"ping",    shell_cmd_ping,    "Send ICMP echo requests: ping <ipv4>"},
    {"netstat", shell_cmd_netstat, "Show network statistics"},
    {"tcpecho", shell_cmd_tcpecho, "TCP round trip: tcpecho <ipv4> <port> <text>"},
    {"tcpsend", shell_cmd_tcpsend, "TCP one-way send: tcpsend <ipv4> <port> <text>"},
    {"tcpstat", shell_cmd_tcpstat, "Show TCP connection/segment counters"},
    {"tcpserve",shell_cmd_tcpserve,"TCP echo server: tcpserve <port>"},
    {"tcpooo",  shell_cmd_tcpooo,  "Self-test: TCP passive open + out-of-order reassembly"},
    {"tcpapi",  shell_cmd_tcpapi,  "TCP echo server over the socket ABI: tcpapi <port>"},
    {"kbdstat", shell_cmd_kbdstat, "Show keyboard IRQ1 ring counters"},
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

        /* One prompt per command line. */
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

int shell_cmd_reboot(int argc, char **argv) {
    (void)argc; (void)argv;
    shell_puts("Rebooting...\n");
    shell_scroll_message("Rebooting system...");
    sys_reboot();
    return 0;
}

int shell_cmd_shutdown(int argc, char **argv) {
    (void)argc; (void)argv;
    shell_puts("Shutting down...\n");
    shell_scroll_message("Shutting down system...");
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

int shell_cmd_sudo(int argc, char **argv) {
    if (argc < 2) {
        shell_puts("Usage: sudo <command> [args...]\n");
        return -1;
    }
    
    /* In a real implementation, this would check permissions and escalate privileges */
    shell_puts("[sudo] Password: ");
    shell_puts("********\n");  /* Simulated password input */
    
    /* Execute command with elevated privileges */
    /* For now, just run the command normally */
    char cmdline[SHELL_MAX_CMD_LEN] = {0};
    for (int i = 1; i < argc; i++) {
        strcat(cmdline, argv[i]);
        if (i < argc - 1) strcat(cmdline, " ");
    }
    
    shell_printf("[sudo] Executing: %s\n", cmdline);
    return shell_process_line(cmdline);
}

int shell_cmd_mount(int argc, char **argv) {
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

/* ==================== Console input helper ==================== *//* Uses PS/2 keyboard driver for input */
int console_getc(void) {
    /* Poll keyboard for scancode */
    int scancode = mbos_kbd_poll();
    if (scancode < 0) return -1;
    
    /* Decode scancode to ASCII */
    int ch = mbos_kbd_decode(scancode);
    if (ch <= 0) return -1;
    
    return ch;
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


