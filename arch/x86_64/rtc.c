/* SutureOS: CMOS RTC driver. See rtc.h for what this anchors and what it is not. */
#include "rtc.h"

#include <stitch/console.h>
#include <stitch/io.h>
#include <stitch/string.h>

#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

#define CMOS_REG_SECONDS 0x00
#define CMOS_REG_MINUTES 0x02
#define CMOS_REG_HOURS   0x04
#define CMOS_REG_DAY     0x07
#define CMOS_REG_MONTH   0x08
#define CMOS_REG_YEAR    0x09
#define CMOS_REG_STATUS_A 0x0A
#define CMOS_REG_STATUS_B 0x0B
#define CMOS_REG_CENTURY 0x32   /* not standardised; QEMU/VMware/SeaBIOS expose it */

/* Nominal tick rate used for the uptime -> seconds conversion (same constant the
 * rest of the kernel uses for uptime, see kernel/gui/apps.c APP_PIT_HZ). */
#define RTC_TICK_HZ 100u

static uint64_t g_boot_epoch;
static int      g_have_rtc;
static uint8_t  g_last_raw[8];      /* last CMOS snapshot, for diagnostics */
static uint32_t g_selftest_checks;
static uint32_t g_selftest_failed;

static uint8_t cmos_read(uint8_t reg)
{
    outb(CMOS_ADDR, (uint8_t)(0x80u | reg));   /* 0x80: keep NMI disabled */
    return inb(CMOS_DATA);
}

static int cmos_update_in_progress(void)
{
    return (cmos_read(CMOS_REG_STATUS_A) & 0x80u) != 0;
}

static uint8_t bcd_to_bin(uint8_t v)
{
    return (uint8_t)(((v >> 4) & 0x0Fu) * 10u + (v & 0x0Fu));
}

/* days from 1970-01-01 for a proleptic Gregorian date (Howard Hinnant's
 * days_from_civil, integer only). */
static int64_t days_from_civil(int64_t y, unsigned m, unsigned d)
{
    y -= (m <= 2) ? 1 : 0;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);                    /* [0,399] */
    const unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;        /* [0,146096] */
    return era * 146097 + (int64_t)doe - 719468;
}

static void civil_from_days(int64_t z, int *year, unsigned *month, unsigned *day)
{
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t y = (int64_t)yoe + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned m = mp + (mp < 10 ? 3 : -9);
    *year = (int)(y + (m <= 2 ? 1 : 0));
    *month = m;
    *day = d;
}

uint64_t rtc_cmos_to_epoch(uint8_t sec, uint8_t min, uint8_t hour, uint8_t day,
                           uint8_t month, uint16_t year, int is_bcd, int is_pm24)
{
    uint32_t s = sec, mi = min, h = hour, d = day, mo = month, y = year;

    const int pm = (!is_pm24) && ((hour & 0x80u) != 0);

    if (is_bcd) {
        s = bcd_to_bin(sec);
        mi = bcd_to_bin(min);
        h = bcd_to_bin((uint8_t)(hour & 0x7Fu));
        d = bcd_to_bin(day);
        mo = bcd_to_bin(month);
        y = (uint32_t)bcd_to_bin((uint8_t)(year & 0xFFu));
        if (y < 100u) y += 2000u;      /* two-digit year from the CMOS */
    } else {
        h = (uint32_t)(hour & 0x7Fu);
    }
    if (!is_pm24) {
        /* 12-hour mode: bit7 is the PM flag, stripped above */
        if (h == 12u) h = pm ? 12u : 0u;
        else if (pm) h += 12u;
    }
    if (mo < 1u) mo = 1u;
    if (mo > 12u) mo = 12u;
    if (d < 1u) d = 1u;
    if (d > 31u) d = 31u;
    if (y < 1970u) y = 1970u;

    {
        const int64_t days = days_from_civil((int64_t)y, mo, d);
        const int64_t secs = days * 86400 + (int64_t)h * 3600 + (int64_t)mi * 60 + (int64_t)s;
        return (secs > 0) ? (uint64_t)secs : 0u;
    }
}

uint64_t rtc_uptime_seconds(void)
{
    extern volatile uint64_t g_tick_count;
    return g_tick_count / RTC_TICK_HZ;
}

uint64_t rtc_now_epoch(void)
{
    return g_boot_epoch + rtc_uptime_seconds();
}

int rtc_have_hardware_clock(void)
{
    return g_have_rtc;
}

int rtc_init(void)
{
    uint8_t sec = 0, min = 0, hour = 0, day = 0, month = 0, year = 0;
    uint8_t century = 0;
    uint8_t status_b;
    int guard;

    g_boot_epoch = 0;
    g_have_rtc = 0;

    /* Wait for the update cycle to finish (UIP clear). */
    for (guard = 0; guard < 100000 && cmos_update_in_progress(); guard++) {
        __asm__ volatile("pause");
    }
    if (guard >= 100000) {
        console_puts("[rtc] CMOS update never finished (keeping uptime clock)\n");
        return 0;
    }

    sec     = cmos_read(CMOS_REG_SECONDS);
    min     = cmos_read(CMOS_REG_MINUTES);
    hour    = cmos_read(CMOS_REG_HOURS);
    day     = cmos_read(CMOS_REG_DAY);
    month   = cmos_read(CMOS_REG_MONTH);
    year    = cmos_read(CMOS_REG_YEAR);
    century = cmos_read(CMOS_REG_CENTURY);
    status_b = cmos_read(CMOS_REG_STATUS_B);
    g_last_raw[0] = sec; g_last_raw[1] = min; g_last_raw[2] = hour;
    g_last_raw[3] = day; g_last_raw[4] = month; g_last_raw[5] = year;
    g_last_raw[6] = century; g_last_raw[7] = status_b;

    {
        const int is_bcd = (status_b & 0x04u) == 0;        /* bit2 clear => BCD */
        const int is_pm24 = (status_b & 0x02u) != 0;       /* bit1 set  => 24h */
        /* Decode ONCE here into binary values; rtc_cmos_to_epoch() is then
         * called in binary/24h mode (it used to be handed an already-binary
         * year while still decoding it as BCD, which made every read fail). */
        uint32_t bsec = is_bcd ? bcd_to_bin(sec) : sec;
        uint32_t bmin = is_bcd ? bcd_to_bin(min) : min;
        uint32_t bday = is_bcd ? bcd_to_bin(day) : day;
        uint32_t bmon = is_bcd ? bcd_to_bin(month) : month;
        uint32_t byear = is_bcd ? bcd_to_bin(year) : year;
        uint32_t bhour = is_bcd ? (uint32_t)(bcd_to_bin((uint8_t)(hour & 0x7Fu)) |
                                            (hour & 0x80u))
                                : hour;
        uint32_t full_year = byear;

        if (is_bcd && century >= 19u && century <= 40u) {
            full_year = (uint32_t)bcd_to_bin(century) * 100u + byear;
        } else if (byear < 100u) {
            full_year = 2000u + byear;
        }

        if (!is_pm24) {
            const int pm = (bhour & 0x80u) != 0;
            uint32_t h12 = bhour & 0x7Fu;
            if (h12 == 12u) h12 = pm ? 12u : 0u;
            else if (pm) h12 += 12u;
            bhour = h12;
        }

        g_boot_epoch = rtc_cmos_to_epoch((uint8_t)bsec, (uint8_t)bmin,
                                         (uint8_t)bhour, (uint8_t)bday,
                                         (uint8_t)bmon, (uint16_t)full_year,
                                         0, 1);        /* already binary, 24h */
        g_have_rtc = (g_boot_epoch > 946684800u);          /* later than 2000-01-01 */
    }

    if (g_have_rtc) {
        char stamp[32];
        rtc_format_epoch(g_boot_epoch, stamp, sizeof(stamp));
        console_printf("[  OK  ] rtc: CMOS clock read, boot time %s\n", stamp);
        return 1;
    }

    g_boot_epoch = 0;
    console_printf("[rtc] implausible CMOS time (raw sec=%x min=%x hour=%x "
                   "day=%x mon=%x year=%x cent=%x stb=%x)\n",
                   (uint64_t)sec, (uint64_t)min, (uint64_t)hour, (uint64_t)day,
                   (uint64_t)month, (uint64_t)year, (uint64_t)century,
                   (uint64_t)status_b);
    return 0;
}

/* Zero-padded decimal helper: kprintf/snprintf in this kernel do not implement
 * the '0' flag consistently, and a wall clock that prints " 1: 2: 3" looks
 * broken. */
static void fmt_pad2(char *out, unsigned v)
{
    out[0] = (char)('0' + (v / 10u) % 10u);
    out[1] = (char)('0' + v % 10u);
    out[2] = '\0';
}

void rtc_format_epoch(uint64_t epoch, char *out, size_t outlen)
{
    const int64_t secs = (int64_t)epoch;
    int64_t days = secs / 86400;
    int64_t rem = secs % 86400;
    int year;
    unsigned month, day;

    if (rem < 0) { rem += 86400; days--; }
    civil_from_days(days, &year, &month, &day);
    {
        char ybuf[5];
        char mbuf[3], dbuf[3], hbuf[3], nibuf[3], sbuf[3];
        unsigned y = (year < 0) ? 0u : (unsigned)year;
        ybuf[0] = (char)('0' + (y / 1000u) % 10u);
        ybuf[1] = (char)('0' + (y / 100u) % 10u);
        ybuf[2] = (char)('0' + (y / 10u) % 10u);
        ybuf[3] = (char)('0' + y % 10u);
        ybuf[4] = '\0';
        fmt_pad2(mbuf, month);
        fmt_pad2(dbuf, day);
        fmt_pad2(hbuf, (unsigned)(rem / 3600));
        fmt_pad2(nibuf, (unsigned)((rem % 3600) / 60));
        fmt_pad2(sbuf, (unsigned)(rem % 60));
        snprintf(out, outlen, "%s-%s-%s %s:%s:%s UTC",
                 ybuf, mbuf, dbuf, hbuf, nibuf, sbuf);
    }
}

/* ---------------- self test ---------------- */

static void st_report(const char *what, int ok)
{
    g_selftest_checks++;
    if (!ok) g_selftest_failed++;
    console_printf("rtc selftest: %s ... %s\n", what, ok ? "PASS" : "FAIL");
}

int rtc_selftest(void)
{
    g_selftest_checks = 0;
    g_selftest_failed = 0;

    /* 2026-10-22 12:34:56 UTC = 1792672496 */
    {
        const uint64_t e = rtc_cmos_to_epoch(0x56, 0x34, 0x12, 0x22, 0x10, 0x26, 1, 1);
        st_report("BCD decode 2026-10-22 12:34:56",
                  e == 1792672496u);
    }
    /* binary (non-BCD) registers must decode identically */
    {
        const uint64_t e = rtc_cmos_to_epoch(56, 34, 12, 22, 10, 2026, 0, 1);
        st_report("binary decode matches BCD", e == 1792672496u);
    }
    /* 12h mode: 01:00 PM -> 13:00 */
    {
        const uint64_t pm = rtc_cmos_to_epoch(0, 0, (uint8_t)(0x80u | 0x01u), 0x01, 0x01, 0x26, 1, 0);
        const uint64_t expect = rtc_cmos_to_epoch(0, 0, 13, 0x01, 0x01, 2026, 0, 1);
        st_report("12h PM flag -> +12 hours", pm == expect);
    }
    /* midnight in 12h mode: 12 AM == 00:00 */
    {
        const uint64_t am = rtc_cmos_to_epoch(0, 0, 0x12, 0x01, 0x01, 0x26, 1, 0);  /* 12 AM */
        const uint64_t expect = rtc_cmos_to_epoch(0, 0, 0, 0x01, 0x01, 2026, 0, 1);
        st_report("12h 12AM -> 00:00", am == expect);
    }
    /* leap year: 2024-02-29 exists, 2026-02-29 clamps to 29 (no crash) */
    {
        const uint64_t e = rtc_cmos_to_epoch(0, 0, 0, 29, 2, 2024, 0, 1);
        const int64_t days = (int64_t)(e / 86400);
        st_report("leap day 2024-02-29 accepted", days == 19782);
    }
    /* formatting round trip */
    {
        char buf[40];
        rtc_format_epoch(1792672496u, buf, sizeof(buf));
        st_report("format 1792672496 -> 2026-10-22 12:34:56",
                  strcmp(buf, "2026-10-22 12:34:56 UTC") == 0);
    }
    /* monotonic: uptime clock never goes backwards */
    {
        const uint64_t a = rtc_now_epoch();
        const uint64_t b = rtc_now_epoch();
        st_report("wall clock monotonic", b >= a);
    }
    st_report("hardware clock detected", g_have_rtc == 1);

    console_printf("rtc selftest: RESULT %s (%u/%u)\n",
                   (g_selftest_failed == 0) ? "PASS" : "FAIL",
                   g_selftest_checks - g_selftest_failed, g_selftest_checks);
    return (g_selftest_failed == 0) ? 0 : 1;
}
