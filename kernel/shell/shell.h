/* SutureOS: Simple Terminal Shell
 * Minimal terminal emulator with command line editing, history, and builtins
 * Compatible with Linux-like terminal interface (black background, white text)
 */
#ifndef STITCH_SHELL_H
#define STITCH_SHELL_H

#include <stitch/types.h>

/* Shell configuration */
#define SHELL_MAX_CMD_LEN     256
#define SHELL_MAX_ARGS        16
#define SHELL_HISTORY_SIZE    16
#define SHELL_PROMPT_MAX      64

/* Terminal colors (VGA text mode compatible) */
#define TERM_COLOR_BLACK      0x000000
#define TERM_COLOR_WHITE      0xFFFFFF
#define TERM_COLOR_RED        0xFF0000
#define TERM_COLOR_GREEN      0x00FF00
#define TERM_COLOR_YELLOW     0xFFFF00
#define TERM_COLOR_BLUE       0x0000FF
#define TERM_COLOR_MAGENTA    0xFF00FF
#define TERM_COLOR_CYAN       0x00FFFF

/* Shell state */
struct shell_state {
    char cmdline[SHELL_MAX_CMD_LEN];
    int cmdline_len;
    int cursor_pos;
    
    /* History */
    char history[SHELL_HISTORY_SIZE][SHELL_MAX_CMD_LEN];
    int history_count;
    int history_index;
    
    /* Prompt */
    char prompt[SHELL_PROMPT_MAX];
    
    /* Terminal state */
    bool running;
    int exit_code;
    
    /* Colors */
    uint32_t fg_color;
    uint32_t bg_color;
};

/* Initialize shell */
void shell_init(void);

/* Main shell loop - blocks until exit */
int shell_run(void);

/* Process a single line of input */
int shell_process_line(const char *line);

/* Built-in commands */
int shell_cmd_help(int argc, char **argv);
int shell_cmd_clear(int argc, char **argv);
int shell_cmd_echo(int argc, char **argv);
int shell_cmd_exit(int argc, char **argv);
int shell_cmd_reboot(int argc, char **argv);
int shell_cmd_shutdown(int argc, char **argv);
int shell_cmd_startgui(int argc, char **argv);
int shell_cmd_ls(int argc, char **argv);
int shell_cmd_cd(int argc, char **argv);
int shell_cmd_pwd(int argc, char **argv);
int shell_cmd_cat(int argc, char **argv);
int shell_cmd_ps(int argc, char **argv);
int shell_cmd_kill(int argc, char **argv);
int shell_cmd_sudo(int argc, char **argv);
int shell_cmd_mount(int argc, char **argv);
int shell_cmd_umount(int argc, char **argv);
int shell_cmd_df(int argc, char **argv);
int shell_cmd_cpus(int argc, char **argv);
int shell_cmd_smptest(int argc, char **argv);
int shell_cmd_ping(int argc, char **argv);
int shell_cmd_netstat(int argc, char **argv);

/* Terminal output helpers */
void shell_putc(char c);
void shell_puts(const char *s);
void shell_printf(const char *fmt, ...);
void shell_set_colors(uint32_t fg, uint32_t bg);
void shell_scroll_message(const char *msg);  /* For boot/shutdown messages */

#endif /* STITCH_SHELL_H */