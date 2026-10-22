/* SutureOS: CMOS RTC driver - the missing wall clock.
 *
 * The kernel only had a monotonic tick counter (g_tick_count) and a fake
 * clock_gettime() that returned a hardcoded 1234567890, so `date` was impossible
 * and file timestamps meant nothing. This reads the CMOS real-time clock once at
 * boot (BCD or binary, 12h or 24h, century register when the firmware provides
 * one) and anchors the monotonic tick counter on it:
 *
 *     epoch = boot_epoch + g_tick_count / SUTUREOS_TICK_HZ
 *
 * That is what gettimeofday()/time()/clock_gettime() and the shell's `date` now
 * report. There is no timezone database and no NTP: the RTC is interpreted as UTC
 * and printed as such, like a server with TZ=UTC.
 */
#ifndef SUTUREOS_RTC_H
#define SUTUREOS_RTC_H

#include <stitch/types.h>

/* Anchors the wall clock from the CMOS RTC. Returns 1 when a plausible time was
 * read (year 1970..2150), 0 otherwise (the clock then falls back to 1970-01-01
 * plus uptime, and says so in the boot log). */
int rtc_init(void);

/* Seconds since the Unix epoch, computed from the boot anchor + uptime. */
uint64_t rtc_now_epoch(void);

/* Seconds since boot (monotonic, tick based). */
uint64_t rtc_uptime_seconds(void);

/* Does the wall clock come from a real RTC read? */
int rtc_have_hardware_clock(void);

/* Format "YYYY-MM-DD HH:MM:SS UTC" into out. */
void rtc_format_epoch(uint64_t epoch, char *out, size_t outlen);

/* Exposed for the self test: decode a raw CMOS snapshot without touching the
 * hardware (so the BCD/12h logic can be tested with known inputs). */
uint64_t rtc_cmos_to_epoch(uint8_t sec, uint8_t min, uint8_t hour, uint8_t day,
                           uint8_t month, uint16_t year, int is_bcd, int is_pm24);

/* Self test: BCD/binary decoding, 12h conversion, leap years, epoch formatting.
 * Prints "rtc selftest: ... PASS/FAIL" and a RESULT line; 0 when all passed. */
int rtc_selftest(void);

#endif /* SUTUREOS_RTC_H */
