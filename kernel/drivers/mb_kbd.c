/* StitchOS module: PS/2 keyboard scancode decoder (mbos_ prefix)
 * Ported from: MandelbrotOS (C:\Users\fanqi\Desktop\自研操作系统\MandelbrotOS\src\kernel\kbd.c,
 *              C:\Users\fanqi\Desktop\自研操作系统\MandelbrotOS\src\include\kernel\kbd.h)
 * Original license: MPL-2.0 (file-level copyleft; this file remains under MPL-2.0 terms)
 * Changes:
 *   - mapndebounce()/handelescape() merged into one pure decoder
 *     mbos_kbd_decode(); the source's undefined fall-off-end returns (make
 *     codes with an empty map slot, e.g. caps) now return 0;
 *   - caps-lock case folding ADDED (source tracked no caps state): for
 *     letters caps XOR shift decides case, caps never affects symbols;
 *   - the E0 prefix now actually persists until the following byte (the
 *     source set E0ESC and cleared it again on the same 0xE0 byte because
 *     0xE0 has bit7 set, so the flag never survived);
 *   - kbdhandler()/irq_install_handler(1, ...) removed: this phase has no
 *     interrupts (kernel cli). Phase 2 wires it with
 *     moos_irq_register(33, ...) -> mbos_kbd_poll() (IRQ1 = vector 33);
 *   - gets()/malloc/printf echo and Ctrl-L cls() dropped (shell owns input);
 *   - keymaps copied verbatim from the source, only made const.
 */
#include "mb_kbd.h"
#include "mb_mouse.h"   /* mbos_mouse_feed_byte() */

#include <stitch/io.h>
#include <stitch/selftest.h>
#include <stitch/console.h>   /* console_printf (key diagnostics) */

/* ---- decoder state ---- */
static uint32_t kb_mode;      /* SHIFT | CTRL | ALT | E0ESC (MandelbrotOS) */
static bool     kb_caps;      /* StitchOS addition */

/* ---- scancode set 1 keymaps (verbatim from MandelbrotOS kbd.c) ---- */
static const char kb_map[128] = {
    0,
    0x1b, /* esc */
    '1', '2', '3', '4', '5', '6', '7', '8', '9', '0',
    '-', '=', '\b', '\t', 'q', 'w', 'e', 'r',
    't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0, /* left ctrl */
    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0, /* left shift */
    '\\', 'z', 'x', 'c', 'v', 'b', 'n',
    'm', ',', '.', '/',
    0, /* right shift */
    '*',
    0,                            /* alt */
    ' ',                          /* space*/
    0,                            /* capslock */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* f1 ... f10 */
    0,                            /* num lock*/
    0,                            /* scroll Lock */
    0,                            /* home key */
    0,                            /* up arrow */
    0,                            /* page up */
    '-',
    0, /* left arrow */
    0,
    0, /* right arrow */
    '+',
    0, /* end key*/
    0, /* down arrow */
    0, /* page down */
    0, /* insert key */
    0, /* delete key */
    0, 0, 0,
    0, /* f11 key */
    0, /* f12 key */
    0, /* all other keys are undefined */
};

static const char kb_shift_map[128] = {
    0,
    0x1b, /* esc */
    '!', '@', '#', '$', '%', '^', '&', '*', '(', ')',
    '_', '+', '\b', '\t', 'Q', 'W', 'E', 'R',
    'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0, /* left control */
    'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '\"', '~',
    0, /* left shift */
    '|', 'Z', 'X', 'C', 'V', 'B', 'N',
    'M', '<', '>', '?',
    0, /* right shift */
    '*',
    0,                            /* alt */
    ' ',                          /* space bar */
    0,                            /* caps lock */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* f1 ... f10 */
    0,                            /* num lock*/
    0,                            /* scroll lock */
    0,                            /* home key */
    0,                            /* up arrow */
    0,                            /* page up */
    '-',
    0, /* left arrow */
    0,
    0, /* right arrow */
    '+',
    0, /* end key*/
    0, /* down arrow */
    0, /* page down */
    0, /* insert key */
    0, /* delete key */
    0, 0, 0,
    0, /* f11 key */
    0, /* f12 key */
    0, /* all other keys are undefined */
};

/* Source shift() helper: modifier mask carried by this scancode, 0 = none.
 * An E0-prefixed byte only re-maps ctrl/alt (extended 0x1D/0x38), exactly
 * like the source - extended shift bytes are ignored there too. */
static uint8_t mod_of(uint8_t sc)
{
    const uint8_t ch = (uint8_t)(sc & 0x7Fu);

    if (kb_mode & MBOS_KBD_E0ESC) {
        switch (ch) {
        case 0x1D: return MBOS_KBD_CTRL;
        case 0x38: return MBOS_KBD_ALT;
        default:   return 0;
        }
    }
    switch (ch) {
    case 0x2A:                     /* left shift  */
    case 0x36: return MBOS_KBD_SHIFT; /* right shift */
    case 0x1D: return MBOS_KBD_CTRL;
    case 0x38: return MBOS_KBD_ALT;
    default:   return 0;
    }
}

void mbos_kbd_init(void)
{
    kb_mode = 0;
    kb_caps = false;
}

int mbos_kbd_decode(uint8_t scancode)
{
    const uint8_t sc = scancode;
    uint8_t mod;
    char base, shifted;

    /* E0 prefix: remember it for the next byte, produce nothing */
    if (MBOS_KBD_IS_ESCAPE(sc)) {
        kb_mode |= MBOS_KBD_E0ESC;
        return 0;
    }

    /* modifier make/break updates state only (source shift() path) */
    mod = mod_of(sc);
    if (mod != 0) {
        if (MBOS_KBD_IS_RELEASE(sc))
            kb_mode &= ~mod;
        else
            kb_mode |= mod;
        kb_mode &= ~MBOS_KBD_E0ESC;      /* one byte consumed after E0 */
        return 0;
    }

    /* caps lock: toggle on make, ignore the break (StitchOS addition) */
    if ((sc & 0x7Fu) == MBOS_KBD_CAPS) {
        if (!MBOS_KBD_IS_RELEASE(sc))
            kb_caps = !kb_caps;
        kb_mode &= ~MBOS_KBD_E0ESC;
        return 0;
    }

    /* break codes never produce characters */
    if (MBOS_KBD_IS_RELEASE(sc)) {
        kb_mode &= ~MBOS_KBD_E0ESC;
        return 0;
    }
    kb_mode &= ~MBOS_KBD_E0ESC;

    base   = kb_map[sc & 0x7Fu];
    shifted = kb_shift_map[sc & 0x7Fu];
    if (base == 0)
        return 0;                       /* unmapped make code */

    /* case folding: letters use caps XOR shift, everything else shift only */
    {
        bool shift_on = (kb_mode & MBOS_KBD_SHIFT) != 0;
        if (base >= 'a' && base <= 'z')
            shift_on = shift_on != kb_caps;
        return (int)(unsigned char)(shift_on ? shifted : base);
    }
}

/* ==================== IRQ1 bookkeeping ====================
 * IRQ1 is now actually delivered (the IOAPIC routes the 8259 cascade as ExtINT),
 * but measurements on this machine show it stops after ~2 interrupts, and - more
 * importantly - having the handler read the 8042 races the polled reader: the
 * shell can pass the output-buffer-full test and then lose the byte to the
 * interrupt (measured: a 27-character shell command arrived as 18 characters,
 * i.e. WORSE than pure polling). So the handler only *counts* the interrupt and
 * the single consumer of 0x60 stays the polling path. The counters keep the
 * delivery state visible through `kbdstat`. */
static volatile uint32_t g_kbd_irqs, g_kbd_drops;
static volatile int      g_kbd_irq_on;      /* 1 = the 8042 asserts IRQ1 */

void mbos_kbd_irq_handler(void)
{
    g_kbd_irqs++;
    /* Intentionally does NOT read 0x60: see the comment above. */
}

uint32_t mbos_kbd_irq_count(void)  { return g_kbd_irqs; }
uint32_t mbos_kbd_dropped_count(void) { return g_kbd_drops; }

/* Ask the 8042 to assert IRQ1 (configuration byte bit 0). Every wait is
 * bounded: the controller may be absent on an exotic machine. */
void mbos_kbd_enable_irq(void)
{
    uint32_t spins;
    uint8_t  cfg;

    /* Flush anything already sitting in the output buffer first, so the first
     * interrupt does not carry a stale byte. */
    for (spins = 0; spins < 100000u; spins++) {
        if ((inb(MBOS_KBD_STAT) & MBOS_KBD_STAT_OBF) == 0)
            break;
        (void)inb(MBOS_KBD_DATA);
    }

    /* Command 0x20: read the configuration byte. */
    for (spins = 0; spins < 100000u; spins++)
        if ((inb(MBOS_KBD_STAT) & 0x02u) == 0) break;
    outb(MBOS_KBD_CMD, 0x20);
    for (spins = 0; spins < 100000u; spins++)
        if ((inb(MBOS_KBD_STAT) & MBOS_KBD_STAT_OBF) != 0) break;
    if ((inb(MBOS_KBD_STAT) & MBOS_KBD_STAT_OBF) == 0)
        return;                                  /* no 8042: stay polled */
    cfg = inb(MBOS_KBD_DATA);

    cfg |= 0x01u;                                /* enable IRQ1            */
    cfg &= (uint8_t)~0x10u;                      /* enable the keyboard clk */

    /* Command 0x60: write the configuration byte back. */
    for (spins = 0; spins < 100000u; spins++)
        if ((inb(MBOS_KBD_STAT) & 0x02u) == 0) break;
    outb(MBOS_KBD_CMD, 0x60);
    for (spins = 0; spins < 100000u; spins++)
        if ((inb(MBOS_KBD_STAT) & 0x02u) == 0) break;
    outb(MBOS_KBD_DATA, cfg);

    /* Record that IRQ1 is enabled (diagnostics only: the polling path remains
     * the single consumer of 0x60 - see the handler comment). */
    g_kbd_irq_on = 1;
}

int mbos_kbd_poll(void)
{
    /* Single consumer of 0x60 for the KEYBOARD: the polling path (see the IRQ1
     * comment above - the handler must not read the port, it only counts).
     *
     * The 8042 output buffer is SHARED with the PS/2 mouse: status bit 5 says
     * the byte came from the aux port. Such a byte must still be READ here and
     * handed to the mouse driver - if it is left in the buffer the buffer stays
     * full forever, the keyboard byte behind it can never be read and typing
     * dies (observed: the first character after boot was swallowed, and typing
     * only recovered when an IRQ1 handler happened to read the aux byte as if it
     * were a scancode). So: drain aux bytes into the mouse, return only
     * scancodes. */
    for (int guard = 0; guard < 16; guard++) {
        const uint8_t st = inb(MBOS_KBD_STAT);
        if ((st & MBOS_KBD_STAT_OBF) == 0)
            return -1;                  /* nothing to read */
        if ((st & 0x20u) != 0) {
            /* aux (mouse) byte: consume it so the buffer drains, and let the
             * mouse driver decode it out of band */
            mbos_mouse_feed_byte(inb(MBOS_KBD_DATA));
            continue;
        }
        return (int)inb(MBOS_KBD_DATA);
    }
    return -1;                          /* pathological flood: nothing for us */
}

/* ---- key-delivery diagnostics ----
 * "the GUI ignores my keys" has two very different causes: the byte never
 * reaches the poller, or it is decoded to 0 and dropped. Logging the first few
 * (raw scancode, decoded char) pairs tells them apart from the serial log. */
static uint32_t g_key_log_count;

void mbos_kbd_key_log_note(int sc, int ch)
{
    if (g_key_log_count >= 12) {
        return;
    }
    g_key_log_count++;
    console_printf("[kbd] key #%u sc=0x%x ch=0x%x\n",
                   (uint64_t)g_key_log_count, (uint64_t)(uint32_t)sc,
                   (uint64_t)(uint32_t)ch);
}

uint32_t mbos_kbd_key_log_count(void) { return g_key_log_count; }

uint32_t mbos_kbd_fallback_count(void) { return g_kbd_drops; }
int      mbos_kbd_irq_active(void)     { return g_kbd_irq_on; }

/* ==================== self test ==================== */
/* Synthetic scancode sequences only - no hardware, no IRQ (kernel is cli).
 * Source irq handler idea kept for phase 2: irq_install_handler(1, kbdhandler)
 * becomes moos_irq_register(33, handler) with the handler calling
 * mbos_kbd_poll() + mbos_kbd_decode(). */
void st_mbos_kbd_test(void)
{
    bool ok = true;

    mbos_kbd_init();

    /* 1) 'A' key (set1 0x1E make, 0x9E break) decodes to 'a'; break is silent */
    ok = ok && (mbos_kbd_decode(0x1E) == 'a');
    ok = ok && (mbos_kbd_decode(0x9E) == 0);

    /* 2) hold left shift + 'A' -> 'A'; releasing shift restores 'a' */
    ok = ok && (mbos_kbd_decode(0x2A) == 0);     /* LShift make  */
    ok = ok && (mbos_kbd_decode(0x1E) == 'A');
    ok = ok && (mbos_kbd_decode(0x9E) == 0);     /* 'A' break silent */
    ok = ok && (mbos_kbd_decode(0xAA) == 0);     /* LShift break */
    ok = ok && (mbos_kbd_decode(0x1E) == 'a');

    /* 3) caps on: letter keys upper-case even without shift ... */
    ok = ok && (mbos_kbd_decode(0x3A) == 0);     /* caps make toggles on */
    ok = ok && (mbos_kbd_decode(0xBA) == 0);     /* caps break: no toggle, no char */
    ok = ok && (mbos_kbd_decode(0x1E) == 'A');
    ok = ok && (mbos_kbd_decode(0x1E) == 'A');   /* still on (no autorepeat toggle) */
    /*    ... shift + caps flips letters back, symbols stay shifted */
    ok = ok && (mbos_kbd_decode(0x2A) == 0);
    ok = ok && (mbos_kbd_decode(0x1E) == 'a');
    ok = ok && (mbos_kbd_decode(0x02) == '!');   /* symbol keys ignore caps (still shifted) */
    ok = ok && (mbos_kbd_decode(0xAA) == 0);
    ok = ok && (mbos_kbd_decode(0x3A) == 0);     /* caps make toggles off */
    ok = ok && (mbos_kbd_decode(0x1E) == 'a');

    /* 4) break codes never produce characters (fresh state) */
    mbos_kbd_init();
    ok = ok && (mbos_kbd_decode(0x9E) == 0);
    ok = ok && (mbos_kbd_decode(0xAA) == 0);
    ok = ok && (mbos_kbd_decode(0xBA) == 0);
    ok = ok && (mbos_kbd_decode(0x1E) == 'a');   /* still decodes after breaks */

    st_run("mandelbrot_kbd", ok);
}
