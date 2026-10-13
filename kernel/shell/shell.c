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
    if (g_shell.fg_color != 0xFFFFFF || g_shell.bg_color != 0x000000) {
        fbcon_putc(c);  /* Use framebuffer console with colors */
    } else {
        console_putc(c);  /* Fallback to serial console */
    }
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
    
    /* Initialize framebuffer console with black/white colors */
    fbcon_init(0, 0, 0, 0, 0);  /* Will be properly initialized by kmain */
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
    fbcon_clear();
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
    shell_puts("Starting GUI interface...\n");
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
    shell_puts("GUI exited, returning to terminal\n");
    fbcon_clear();
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

/* ==================== Console input helper ==================== */
/* Uses PS/2 keyboard driver for input */
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