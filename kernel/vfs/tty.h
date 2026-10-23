/* SutureOS: the smallest tty layer the console needs to answer ioctl().
 *
 * File descriptors 0/1/2 are the console (kernel/term + fbcon/serial). There is
 * no line discipline here: input echo and canonical mode live in the shell and
 * in user_console_read(), and this kernel deliberately does not change the way
 * it draws or reads the console. What this module adds is the *ioctl surface*
 * a terminal-aware program (an editor, `ls` asking for the window size, `stty`)
 * expects, so it can round-trip its settings instead of getting -ENOTTY.
 *
 * Layouts are the Linux x86-64 ones:
 *   struct termios  = 60 bytes (4 x tcflag_t, c_line, c_cc[32], 2 x speed_t)
 *   struct winsize  = 8 bytes  (row, col, xpixel, ypixel)
 */
#ifndef SUTUREOS_TTY_H
#define SUTUREOS_TTY_H

#include <stitch/types.h>

typedef unsigned char  tty_cc_t;
typedef unsigned int   tty_speed_t;
typedef unsigned int   tty_tcflag_t;

struct termios {
    tty_tcflag_t c_iflag;
    tty_tcflag_t c_oflag;
    tty_tcflag_t c_cflag;
    tty_tcflag_t c_lflag;
    tty_cc_t     c_line;
    tty_cc_t     c_cc[32];
    tty_speed_t  c_ispeed;
    tty_speed_t  c_ospeed;
};

struct winsize {
    uint16_t ws_row;
    uint16_t ws_col;
    uint16_t ws_xpixel;
    uint16_t ws_ypixel;
};

/* ioctl request numbers (Linux, x86-64) */
#define TCGETS      0x5401u
#define TCSETS      0x5402u
#define TCSETSW     0x5403u
#define TCSETSF     0x5404u
#define TCFLSH      0x540Bu
#define TIOCGWINSZ  0x5413u

/* termios flag bits we actually describe (the rest stay zero, which is also what
 * a caller that only round-trips the struct will see). */
#define TTY_IFLAG_IGNBRK   0x00000001u
#define TTY_IFLAG_BRKINT   0x00000002u
#define TTY_IFLAG_ICRNL    0x00000100u
#define TTY_IFLAG_IXON     0x00000400u
#define TTY_OFLAG_OPOST    0x00000001u
#define TTY_OFLAG_ONLCR    0x00000004u
#define TTY_CFLAG_CS8      0x00000030u
#define TTY_CFLAG_CREAD    0x00000080u
#define TTY_LFLAG_ISIG     0x00000001u
#define TTY_LFLAG_ICANON   0x00000002u
#define TTY_LFLAG_ECHO     0x00000008u
#define TTY_LFLAG_ECHOE    0x00000010u
#define TTY_LFLAG_ECHOK    0x00000020u
#define TTY_LFLAG_IEXTEN   0x00008000u
#define TTY_B38400         0x0000000Fu

/* c_cc[] indices */
#define TTY_VINTR   0
#define TTY_VQUIT   1
#define TTY_VERASE  2
#define TTY_VKILL   3
#define TTY_VEOF    4
#define TTY_VTIME   5
#define TTY_VMIN    6
#define TTY_VSTART  8
#define TTY_VSTOP   9
#define TTY_VSUSP   10

/* 1 when this fd is one of the console descriptors (0, 1, 2). */
int tty_is_console(int fd);

/* Answer one ioctl on the console. Returns 0 on success, a negative errno on
 * failure (-ENOTTY for a request this tty does not implement). `arg` points at
 * the caller's struct for the requests that take one; the caller is responsible
 * for validating the pointer (syscalls2.c does it with the ring-3 rule). */
int tty_ioctl(int fd, uint64_t request, void *arg);

/* The persisted console termios (read-only view, for diagnostics). */
const struct termios *tty_current(void);

#endif /* SUTUREOS_TTY_H */
