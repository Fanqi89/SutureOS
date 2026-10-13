/* SutureOS: ACPI power management (power off / reset)
 *
 * Why this file exists: sys_reboot()/sys_shutdown() used to be stubs that only
 * printed a line, so `reboot` and `shutdown` did nothing. Here the real ACPI
 * path is implemented:
 *   - power off : FADT PM1a_CNT_BLK (+ PM1b if present) with SLP_TYP=5 (S5)
 *                 and SLP_EN, falling back to the QEMU/Bochs/VirtualBox
 *                 power-off ports when no FADT is available.
 *   - reset     : FADT RESET_REG (when the FADT advertises it), then the
 *                 chipset reset port 0xCF9, then the 8042 keyboard controller
 *                 pulse, then a triple fault as the last resort.
 * Both return only if every method failed.
 */
#include <stitch/types.h>
#include <stitch/io.h>
#include <stitch/console.h>
#include "acpi.h"
#include "power.h"

/* FADT (FACP) field offsets (ACPI 2.0+ layout) */
#define FADT_PM1A_CNT_BLK   64
#define FADT_PM1B_CNT_BLK   68
#define FADT_PM1_CNT_LEN    89
#define FADT_FLAGS          112
#define FADT_RESET_REG      116
#define FADT_RESET_VALUE    128

#define FADT_FLAG_RESET_REG_SUP (1u << 10)

#define SLP_TYP_S5          5        /* \_S5 SLP_TYP used by QEMU/Bochs/VMware */
#define SLP_EN              (1u << 13)

static uint32_t g_pm1a_cnt;
static uint32_t g_pm1b_cnt;
static uint8_t  g_pm1_cnt_len;
static uint8_t  g_reset_space;      /* 0 = memory, 1 = I/O */
static uint8_t  g_reset_size;
static uint64_t g_reset_addr;
static uint8_t  g_reset_value;
static int      g_have_reset_reg;
static int      g_ready;

static void rd(void *dst, const uint8_t *src, uint32_t n)
{
    uint8_t *d = (uint8_t *)dst;
    for (uint32_t i = 0; i < n; i++) d[i] = src[i];
}

void power_init(void)
{
    if (g_ready) return;
    g_ready = 1;

    acpi_rsdp_t *rsdp = hanos_acpi_find_rsdp();
    if (!rsdp) {
        console_puts("[power] no RSDP: ACPI power off/reset unavailable (port fallbacks only)\n");
        return;
    }
    acpi_sdt_header_t *fadt = hanos_acpi_find_table(rsdp, "FACP");
    if (!fadt) {
        console_puts("[power] no FADT: ACPI power off/reset unavailable (port fallbacks only)\n");
        return;
    }
    if (!hanos_acpi_checksum_valid(fadt, fadt->length)) {
        console_puts("[power] WARNING: FADT checksum invalid, ignoring ACPI tables\n");
        return;
    }

    const uint8_t *t = (const uint8_t *)fadt;
    rd(&g_pm1a_cnt, t + FADT_PM1A_CNT_BLK, 4);
    rd(&g_pm1b_cnt, t + FADT_PM1B_CNT_BLK, 4);
    g_pm1_cnt_len = t[FADT_PM1_CNT_LEN];

    uint32_t flags = 0;
    rd(&flags, t + FADT_FLAGS, 4);
    if (fadt->length > FADT_RESET_VALUE && (flags & FADT_FLAG_RESET_REG_SUP)) {
        const uint8_t *gas = t + FADT_RESET_REG;
        g_reset_space = gas[0];           /* 0 = system memory, 1 = system I/O */
        g_reset_size  = gas[3];           /* access size in bytes */
        rd(&g_reset_addr, gas + 4, 8);
        g_reset_value = t[FADT_RESET_VALUE];
        g_have_reset_reg = (g_reset_addr != 0) ? 1 : 0;
    }

    console_printf("[power] ACPI FADT: PM1a_CNT=0x%x PM1b_CNT=0x%x len=%u reset_reg=%s\n",
                   (uint64_t)g_pm1a_cnt, (uint64_t)g_pm1b_cnt,
                   (uint64_t)g_pm1_cnt_len,
                   g_have_reset_reg ? "yes" : "no");
}

uint32_t power_pm1a_cnt(void) { return g_pm1a_cnt; }

/* --------- power off --------- */

static void power_off_acpi(void)
{
    if (!g_pm1a_cnt) return;
    /* Writing SLP_EN with SLP_TYP=S5 asks the platform to enter soft-off.
     * The value is platform specific (normally read from the DSDT's \_S5
     * object); 5 is what QEMU, Bochs, VirtualBox and VMware use. */
    uint16_t v = (uint16_t)((SLP_TYP_S5 << 10) | SLP_EN);
    console_printf("[power] ACPI S5: outw(0x%x, 0x%x)%s\n",
                   (uint64_t)g_pm1a_cnt, (uint64_t)v,
                   g_pm1b_cnt ? " + PM1b" : "");
    if (g_pm1_cnt_len == 0 || g_pm1_cnt_len == 2) {
        outw((uint16_t)g_pm1a_cnt, v);
        if (g_pm1b_cnt) outw((uint16_t)g_pm1b_cnt, v);
    } else {
        outb((uint16_t)g_pm1a_cnt, (uint8_t)(v & 0xFF));
        if (g_pm1b_cnt) outb((uint16_t)g_pm1b_cnt, (uint8_t)(v & 0xFF));
    }
}

static void power_off_ports(void)
{
    console_puts("[power] trying emulator power-off ports (QEMU 0x604 / Bochs 0xB004 / VBox 0x4004)\n");
    outw(0x604, 0x2000);    /* QEMU: ACPI PM1a_CNT (isa-debug-exit style) */
    outw(0xB004, 0x2000);   /* Bochs / older QEMU */
    outw(0x4004, 0x3400);   /* VirtualBox */
}

void power_off(void)
{
    console_puts("[     ] Syncing filesystems...\n");
    extern int vfs_sync(const char *mountpoint);
    vfs_sync(0);
    console_puts("[  OK  ] Filesystems synced\n");

    power_init();
    console_puts("[     ] Powering off (ACPI S5)...\n");
    power_off_acpi();
    power_off_ports();

    /* Nothing worked: do not pretend, park the CPU. */
    console_puts("[FAILED] power off not supported by this machine, system halted\n");
    for (;;) __asm__ volatile("cli; hlt");
}

/* --------- reset --------- */

static int reset_via_acpi(void)
{
    if (!g_have_reset_reg) return -1;
    console_printf("[power] ACPI RESET_REG: space=%u size=%u addr=0x%x value=0x%x\n",
                   (uint64_t)g_reset_space, (uint64_t)g_reset_size,
                   (uint64_t)g_reset_addr, (uint64_t)g_reset_value);
    if (g_reset_space == 1) {                     /* system I/O */
        uint16_t port = (uint16_t)g_reset_addr;
        if (g_reset_size == 4) outw(port, (uint16_t)g_reset_value);
        else outb(port, g_reset_value);
        return 0;
    }
    if (g_reset_space == 0) {                     /* memory mapped */
        volatile uint8_t *p = (volatile uint8_t *)(uintptr_t)g_reset_addr;
        p[0] = g_reset_value;
        return 0;
    }
    return -1;
}

void power_reset(void)
{
    console_puts("[     ] Syncing filesystems...\n");
    extern int vfs_sync(const char *mountpoint);
    vfs_sync(0);
    console_puts("[  OK  ] Filesystems synced\n");

    power_init();

    if (reset_via_acpi() == 0) {
        console_puts("[     ] Waiting for ACPI reset...\n");
        for (volatile int i = 0; i < 50000000; i++) __asm__ volatile("pause");
    }

    console_puts("[     ] Trying chipset reset port 0xCF9\n");
    outb(0xCF9, 0x06);
    for (volatile int i = 0; i < 100000; i++) __asm__ volatile("pause");
    outb(0xCF9, 0x0E);
    for (volatile int i = 0; i < 50000000; i++) __asm__ volatile("pause");

    console_puts("[     ] Trying 8042 keyboard-controller reset\n");
    while (inb(0x64) & 0x02) __asm__ volatile("pause");
    outb(0x64, 0xFE);
    for (volatile int i = 0; i < 50000000; i++) __asm__ volatile("pause");

    console_puts("[     ] Last resort: triple fault\n");
    {
        struct __attribute__((packed)) { uint16_t limit; uint64_t base; } null_idt = {0, 0};
        __asm__ volatile("lidt %0; int3" : : "m"(null_idt));
    }

    console_puts("[FAILED] reset not supported by this machine, system halted\n");
    for (;;) __asm__ volatile("cli; hlt");
}
