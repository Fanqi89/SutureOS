/* SutureOS: console tty ioctl surface. See tty.h for the scope and for why the
 * line discipline is not implemented here. */
#include "tty.h"

#include <stitch/string.h>
#include "term.h"          /* term_available()/term_cols()/term_rows() */

#define TTY_EFAULT  14
#define TTY_EINVAL  22
#define TTY_ENOTTY  25

/* One console tty serves fds 0/1/2, so one state is enough. It is initialised
 * lazily (static storage starts zeroed, and a zeroed termios is not "sane"). */
static struct termios g_tty;
static int g_tty_ready;

static void tty_defaults(void)
{
    memset(&g_tty, 0, sizeof(g_tty));
    g_tty.c_iflag = TTY_IFLAG_ICRNL | TTY_IFLAG_IXON;
    g_tty.c_oflag = TTY_OFLAG_OPOST | TTY_OFLAG_ONLCR;
    g_tty.c_cflag = TTY_CFLAG_CS8 | TTY_CFLAG_CREAD | TTY_B38400;
    g_tty.c_lflag = TTY_LFLAG_ISIG | TTY_LFLAG_ICANON | TTY_LFLAG_ECHO |
                    TTY_LFLAG_ECHOE | TTY_LFLAG_ECHOK | TTY_LFLAG_IEXTEN;
    g_tty.c_line = 0;
    g_tty.c_cc[TTY_VINTR]  = 3;    /* ^C */
    g_tty.c_cc[TTY_VQUIT]  = 28;   /* ^\ */
    g_tty.c_cc[TTY_VERASE] = 127;  /* DEL */
    g_tty.c_cc[TTY_VKILL]  = 21;   /* ^U */
    g_tty.c_cc[TTY_VEOF]   = 4;    /* ^D */
    g_tty.c_cc[TTY_VTIME]  = 0;
    g_tty.c_cc[TTY_VMIN]   = 1;
    g_tty.c_cc[TTY_VSTART] = 17;   /* ^Q */
    g_tty.c_cc[TTY_VSTOP]  = 19;   /* ^S */
    g_tty.c_cc[TTY_VSUSP]  = 26;   /* ^Z */
    g_tty.c_ispeed = TTY_B38400;
    g_tty.c_ospeed = TTY_B38400;
}

static void tty_ensure(void)
{
    if (!g_tty_ready) {
        tty_defaults();
        g_tty_ready = 1;
    }
}

int tty_is_console(int fd)
{
    return fd >= 0 && fd < 3;
}

const struct termios *tty_current(void)
{
    tty_ensure();
    return &g_tty;
}

int tty_ioctl(int fd, uint64_t request, void *arg)
{
    if (!tty_is_console(fd)) return -TTY_ENOTTY;
    tty_ensure();

    switch (request) {
    case TCGETS:
        if (arg == NULL) return -TTY_EFAULT;
        memcpy(arg, &g_tty, sizeof(g_tty));
        return 0;
    case TCSETS:
    case TCSETSW:
    case TCSETSF:
        if (arg == NULL) return -TTY_EFAULT;
        /* TCSETSW/TCSETSF would wait for the output queue to drain, but the
         * console writes synchronously (framebuffer/serial, no queue), so all
         * three store the settings immediately. */
        memcpy(&g_tty, arg, sizeof(g_tty));
        return 0;
    case TIOCGWINSZ: {
        struct winsize *ws = (struct winsize *)arg;
        const int have = term_available();
        if (ws == NULL) return -TTY_EFAULT;
        ws->ws_row = (uint16_t)(have ? term_rows() : 24);
        ws->ws_col = (uint16_t)(have ? term_cols() : 80);
        /* A serial-only boot has no terminal geometry to report: use the
         * historical 80x24 default rather than lying with 0x0. */
        if (ws->ws_row == 0) ws->ws_row = 24;
        if (ws->ws_col == 0) ws->ws_col = 80;
        ws->ws_xpixel = 0;
        ws->ws_ypixel = 0;
        return 0;
    }
    case TCFLSH:
        /* arg is an int queue selector (TCIFLUSH/TCOFLUSH/TCIOFLUSH), not a
         * pointer. Nothing is buffered in this tty, so flushing is a no-op that
         * succeeds for the three valid selectors and is EINVAL for anything
         * else, the way the tty layer answers an out-of-range queue. */
        {
            const int queue = (int)(uintptr_t)arg;
            return (queue >= 0 && queue <= 2) ? 0 : -TTY_EINVAL;
        }
    default:
        return -TTY_ENOTTY;
    }
}
