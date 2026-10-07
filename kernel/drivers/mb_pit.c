/* StitchOS module: PIT 8253/8254 interval timer (mbos_ prefix)
 * Ported from: MandelbrotOS (C:\Users\fanqi\Desktop\自研操作系统\MandelbrotOS\src\kernel\pit.c)
 *              counter readback idea borrowed from
 *              Phobos (C:\Users\fanqi\Desktop\自研操作系统\Phobos\mainline\kernel\dev\sys\pit.cpp
 *              - GetCounter() latches before reading); per the port brief only
 *              the idea is taken, this file stays MandelbrotOS/MPL-2.0.
 * Original license: MPL-2.0 (file-level copyleft; this file remains under MPL-2.0 terms)
 * Changes: timer_phase()/init_timer() split into mbos_pit_init/mbos_pit_hz;
 *          divisor base 1193180 (source) corrected to the AT's 1193182;
 *          irq_install_handler(0, timer_handler) and the timer_ticks/sleep
 *          globals dropped - interrupts stay masked this phase, the tick
 *          counter hooks into moos_irq_register(32, ...) in phase 2;
 *          mbos_pit_counter() latch readback + self test added.
 */
#include "mb_pit.h"

#include <stitch/io.h>
#include <stitch/selftest.h>

/* "Handles the timer. In this case, it's very simple: We increment the
 *  'timer_ticks' variable every time the timer fires. By default, the timer
 *  fires 18.222 times per second. Why 18.222Hz? Some engineer at IBM must've
 *  been smoking something funky" - kept from MandelbrotOS pit.c (the source
 *  file insists this comment survives). */

#define PIT_CTRL 0x43u
#define PIT_CH0  0x40u
#define PIT_CMD_LATCH_CH0 0x00u     /* latch channel 0 counter (Phobos style) */
#define PIT_CMD_CH0_MODE3 0x36u     /* ch0, lo/hi access, mode 3 square wave  */

static uint32_t g_hz;
static uint32_t g_divisor;

void mbos_pit_init(uint32_t hz)
{
    uint32_t div;

    if (hz == 0)
        hz = 100;                       /* sane default (source used 1000) */
    div = MBOS_PIT_BASE_HZ / hz;        /* source: 1193180 / hz (see header) */
    if (div == 0)
        div = 1;
    if (div > 65535u)
        div = 65535u;                   /* 16-bit counter limit */

    outb(PIT_CTRL, PIT_CMD_CH0_MODE3);
    outb(PIT_CH0, (uint8_t)(div & 0xFFu));
    outb(PIT_CH0, (uint8_t)((div >> 8) & 0xFFu));

    g_hz      = hz;
    g_divisor = div;
}

uint32_t mbos_pit_hz(void)
{
    return g_hz;
}

uint32_t mbos_pit_divisor(void)
{
    return g_divisor;
}

uint16_t mbos_pit_counter(void)
{
    uint8_t lo, hi;

    outb(PIT_CTRL, PIT_CMD_LATCH_CH0);  /* freeze channel 0's count */
    lo = inb(PIT_CH0);
    hi = inb(PIT_CH0);
    return (uint16_t)((uint16_t)lo | (uint16_t)(hi << 8));
}

/* ==================== self test ==================== */
/* Runs against real hardware ports (QEMU 8254) but never needs an interrupt:
 * mode 3 keeps counting with interrupts masked. */
void st_mbos_pit_test(void)
{
    bool ok = true;
    uint16_t count;

    /* 1) program 100 Hz: divisor math + counter readback inside 0..divisor */
    mbos_pit_init(100);
    ok = ok && (mbos_pit_hz() == 100u);
    ok = ok && (mbos_pit_divisor() == (MBOS_PIT_BASE_HZ / 100u));   /* = 11931 */
    count = mbos_pit_counter();
    ok = ok && ((uint32_t)count <= mbos_pit_divisor());

    /* 2) divisor = 1193182 / hz at several frequencies (independent literals) */
    mbos_pit_init(1000);
    ok = ok && (mbos_pit_divisor() == 1193u);
    ok = ok && (mbos_pit_hz() == 1000u);
    mbos_pit_init(18);
    /* 1193182/18 = 66287 > 65535, so divisor clamps to 65535 */
    ok = ok && (mbos_pit_divisor() == 65535u);
    mbos_pit_init(100);
    ok = ok && (mbos_pit_divisor() == 11931u);

    /* 3) outb sequence is accepted and the latched 16-bit value stays sane:
     *    low/high bytes recombine to a count no larger than the divisor
     *    (reading 0x43 back is meaningless on the 8254, so it is skipped). */
    count = mbos_pit_counter();
    ok = ok && ((uint32_t)count <= 11931u);

    /* clamping paths: below 1 Hz overflows 16 bits -> 65535; 0 -> default 100 */
    mbos_pit_init(1);
    ok = ok && (mbos_pit_divisor() == 65535u);
    mbos_pit_init(0);
    ok = ok && (mbos_pit_hz() == 100u);
    ok = ok && (mbos_pit_divisor() == 11931u);

    st_run("mandelbrot_pit", ok);
}