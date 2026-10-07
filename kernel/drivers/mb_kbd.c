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

#include <stitch/io.h>
#include <stitch/selftest.h>

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

int mbos_kbd_poll(void)
{
    if ((inb(MBOS_KBD_STAT) & MBOS_KBD_STAT_OBF) == 0)
        return -1;                      /* nothing to read */
    return (int)inb(MBOS_KBD_DATA);
}

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
