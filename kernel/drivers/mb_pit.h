/* StitchOS module: PIT 8253/8254 interval timer (mbos_ prefix)
 * Ported from: MandelbrotOS (C:\Users\fanqi\Desktop\自研操作系统\MandelbrotOS\src\kernel\pit.c)
 *              counter readback idea borrowed from
 *              Phobos (C:\Users\fanqi\Desktop\自研操作系统\Phobos\mainline\kernel\dev\sys\pit.cpp,
 *              GetCounter latch command); this module stays under MandelbrotOS
 *              terms as required by the port brief.
 * Original license: MPL-2.0 (file-level copyleft; this file remains under MPL-2.0 terms)
 * Changes: timer_phase()/init_timer() split into mbos_pit_init(hz)/mbos_pit_hz();
 *          divisor base corrected from the source's 1193180 to the AT's actual
 *          1193182 Hz; irq_install_handler(0, ...) and the sleep()/timer_ticks
 *          globals dropped (no interrupts in this phase - the tick counter
 *          belongs to the phase-2 PIT IRQ handler); latch readback added from
 *          the Phobos readback approach for the self test.
 */
#ifndef STITCH_MB_PIT_H
#define STITCH_MB_PIT_H

#include <stitch/types.h>

#define MBOS_PIT_BASE_HZ 1193182u    /* input clock of channel 0 (PC/AT) */

/* Program channel 0, lo/hi access, mode 3 (square wave) at `hz`.
 * hz == 0 falls back to 100 Hz; the divisor is clamped to 1..65535. */
void     mbos_pit_init(uint32_t hz);
uint32_t mbos_pit_hz(void);          /* last requested frequency          */
uint32_t mbos_pit_divisor(void);     /* last programmed divisor           */

/* Latch channel 0 (command 0x00 to 0x43) and read its current 16-bit count
 * back from port 0x40 (Phobos PIT::GetCounter style). */
uint16_t mbos_pit_counter(void);

/* boot-time self test (st_run("mandelbrot_pit", ...)) */
void st_mbos_pit_test(void);

#endif /* STITCH_MB_PIT_H */
