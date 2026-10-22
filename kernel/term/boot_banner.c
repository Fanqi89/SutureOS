/* SutureOS: post-boot system banner.
 *
 * Boot presentation (as specified): the screen must show, in this order,
 *
 *   1. the scrolling English kernel log (produced by console_printf during
 *      kmain/smp/driver init),
 *   2. the system name,
 *   3. a clean terminal where commands can be typed,
 *   4. the GUI after the `startgui` command.
 *
 * This module owns step 2-3: it wipes the log off the screen, draws a centred
 * SutureOS banner (name, version, a one-line status) and leaves the cursor on a
 * fresh line so the shell prompt follows immediately.
 *
 * Everything is written through the normal console path, so the banner also
 * shows up in the serial log (useful for headless acceptance runs).
 */
#include <stitch/types.h>
#include <stitch/string.h>
#include <stitch/console.h>

#include "sutureos/version.h"
#include "term.h"
#include "boot_banner.h"

/* The console draws the banner itself (so serial and screen stay in sync);
 * these are the pieces it needs from the rest of the kernel. */
extern void console_puts(const char *s);
extern void console_printf(const char *fmt, ...);

/* fbcon fallback when the flanterm terminal is not up (no framebuffer). */
extern void fbcon_clear(void);

/* How many lines of status detail the banner prints. */
#define BANNER_INFO_LINES 4

static void banner_rule(void)
{
    /* 64 columns wide; the terminal is usually 80+ columns, so keep it well
     * inside the smallest supported width (80x24 -> 80 columns). */
    console_puts("================================================"
                 "================\n");
}

void boot_banner_draw(void)
{
    /* Wipe the scrolling log so the terminal starts clean. flanterm owns the
     * screen when it is available; fbcon_clear() would desynchronise flanterm's
     * grid (the very reason shell_cmd_clear() prefers term_clear()). The
     * _full variant repaints every cell - a plain term_clear() only flushes the
     * cells flanterm considers dirty, and the tail of the boot log stayed
     * visible on the top row. */
    if (term_available())
        term_clear_full();
    else
        fbcon_clear();

    console_puts("\n");
    banner_rule();
    console_puts("        SutureOS  -  a stitched-together hobby OS kernel\n");
    console_printf("                    version %s  (GPL-3.0)\n",
                   SUTUREOS_VERSION);
    banner_rule();
    console_puts("\n");
    console_puts("  All kernel subsystems reported [  OK  ] above.\n");
    console_puts("  Type 'help' for the command list, 'startgui' for the desktop.\n");
    console_puts("\n");
}
