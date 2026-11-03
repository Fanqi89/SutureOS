/* SutureOS: terminal system installer (kernel/install/installer.c)
 *
 * What it does (real writes through blk_write()/VFS, nothing simulated):
 *   1. lists the writable block devices and lets the user pick the target disk
 *      (CD-ROMs / the ramdisk are never offered; the choice needs a second
 *      confirmation typed as "yes" before anything is touched);
 *   2. asks for the root password (empty = account locked, /etc/shadow gets `!`)
 *      and optionally for a normal user (name + password);
 *   3. writes an MBR with one active FAT32 (0x0C) partition starting at LBA 2048
 *      - the 1 MiB gap is where Limine's BIOS stage 2 lives;
 *   4. installs the Limine BIOS boot code exactly the way `limine bios-install`
 *      does (see tools/limine/limine-binary/limine.c bios_install():
 *      sector 0 = limine-bios-hdd.bin[0..511], stage 2 = the rest of that blob
 *      at byte offset 4096, and the stage-2 offset patched into the boot sector
 *      at 0x1A4). The blob is compiled in from limine-bios-hdd.h (BSD-2-Clause,
 *      same file the ISO's Limine was built from);
 *   5. formats the partition as FAT32 (kernel/fs/fatfs_format.c);
 *   6. copies the system files from the ISO: /kernel.elf, /limine-bios.sys,
 *      writes /limine.conf, /SUTUREOS/INSTALLED and /etc/{passwd,shadow,group,
 *      sudoers,motd};
 *   7. syncs and prints `[install] done`.
 *
 * After the first reboot the kernel finds the FAT32 volume carrying
 * /SUTUREOS/INSTALLED (kernel/fs/fatfs.c fatfs_mount_auto + kmain) and seeds
 * /etc from it, so the accounts survive.
 *
 * 来源 / Source: 自己的实现；Limine 引导区安装步骤依据 Limine 5 的安装器源码
 *   tools/limine/limine-binary/limine.c (BSD-2-Clause, Mintsuki 等) 的
 *   bios_install() 流程；FAT32 几何与 scripts/mkfat32_testimg.py 一致。
 * License: GPL-3.0（SutureOS 本体）+ BSD-2-Clause（Limine 引导镜像数据）。
 */
#include "installer.h"
#include <sutureos/version.h>
#include <stitch/types.h>
#include <stitch/string.h>
#include <stitch/console.h>
#include "block.h"
#include "vfs.h"
#include "fatfs.h"
#include "userdb.h"
#include "heap.h"

/* Console keyboard input: the shell owns the PS/2 ring buffer and exports this
 * one entry point (returns the next decoded character, or -1 when idle). */
extern int console_getc(void);

/* Limine's BIOS MBR + stage 2 image, exactly as the official installer embeds it.
 * The header is 151 KB of text but the array itself is ~24 KB of .rodata. */
#include "../../tools/limine/limine-binary/limine-bios-hdd.h"

#define INST_PART_START     2048u            /* 1 MiB: the gap Limine stage 2 lives in */
#define INST_STAGE2_OFFSET  4096u            /* byte offset limine.c uses for stage 2 */
#define INST_STAGE2_PATCH   0x1A4u           /* where the boot sector keeps that offset */
#define INST_MIN_SECTORS    71680u           /* 35 MiB: smallest sensible FAT32 volume */
#define INST_LINE_MAX       96
#define INST_MAX_DISKS      8
#define INST_COPY_CHUNK     32768u

struct inst_disk {
    int      drive;
    char     name[16];
    uint64_t sectors;
    uint32_t sector_size;
};

static struct inst_disk g_disks[INST_MAX_DISKS];
static int g_disk_count;

/* ---------------- tiny UI helpers ---------------- */

static void inst_puts(const char *s) { console_puts(s); }

static int inst_readline(const char *prompt, char *out, uint32_t cap, int echo) {
    uint32_t len = 0;
    int c;

    if (!out || cap == 0) return -1;
    out[0] = '\0';
    inst_puts(prompt);
    for (;;) {
        c = console_getc();
        if (c < 0) continue;
        if (c == '\r' || c == '\n') { inst_puts("\n"); break; }
        if (c == 0x7F || c == '\b') {
            if (len > 0) {
                len--;
                if (echo) inst_puts("\b \b");
            }
            continue;
        }
        if (c == 0x1B || c == 0x03) {          /* ESC / Ctrl-C aborts the line */
            inst_puts("^C\n");
            return -1;
        }
        if (c < 0x20 || c > 0x7E) continue;
        if (len + 1 >= cap) continue;
        out[len++] = (char)c;
        if (echo) console_putc((char)c); else console_putc('*');
    }
    out[len] = '\0';
    return (int)len;
}

/* Read one password (no echo, '*' instead). Returns 0 when the user just pressed
 * Enter, which the caller interprets as "no password". */
static int inst_read_password(const char *prompt, char *out, uint32_t cap) {
    int n = inst_readline(prompt, out, cap, 0);
    if (n < 0) return -1;
    return n;
}

static void inst_hex(const uint8_t *in, uint32_t n, char *out) {
    static const char hexd[] = "0123456789abcdef";
    for (uint32_t i = 0; i < n; i++) {
        out[i * 2]     = hexd[(in[i] >> 4) & 0x0F];
        out[i * 2 + 1] = hexd[in[i] & 0x0F];
    }
    out[n * 2] = '\0';
}

static void inst_put32(uint8_t *p, uint32_t off, uint32_t v) {
    p[off]     = (uint8_t)(v & 0xFF);
    p[off + 1] = (uint8_t)((v >> 8) & 0xFF);
    p[off + 2] = (uint8_t)((v >> 16) & 0xFF);
    p[off + 3] = (uint8_t)((v >> 24) & 0xFF);
}

static void inst_chs(uint8_t *out, uint32_t lba) {
    uint32_t cyl = lba / (63u * 255u);
    uint32_t rem = lba % (63u * 255u);
    uint32_t head = rem / 63u;
    uint32_t sec = rem % 63u + 1u;
    if (cyl > 1023u) { cyl = 1023u; head = 255u; sec = 63u; }
    out[0] = (uint8_t)head;
    out[1] = (uint8_t)((sec & 0x3Fu) | ((cyl >> 2) & 0xC0u));
    out[2] = (uint8_t)(cyl & 0xFFu);
}

/* ---------------- disk enumeration ---------------- */

static int inst_collect_disks(void) {
    g_disk_count = 0;
    inst_puts("\nDisks in this machine:\n");
    for (int d = 0; d < 20; d++) {
        struct blk_device *dev = blk_find_by_drive(d);
        const char *why = NULL;
        if (!dev) continue;
        if (dev->type == BLK_TYPE_RAM) why = "ramdisk, not a real disk";
        else if (dev->type == BLK_TYPE_ATAPI) why = "CD-ROM, read-only";
        else if (dev->flags & BLK_FLAG_READONLY) why = "read-only";
        else if (dev->sector_count < INST_MIN_SECTORS) why = "too small (< 35 MiB)";
        else if (g_disk_count >= INST_MAX_DISKS) why = "too many disks, ignored";

        if (why) {
            console_printf("  [ ] drive %d  %s  %llu MiB  (%s)\n",
                           (int64_t)d, dev->name ? dev->name : "?",
                           (dev->sector_count * 512ull) / (1024ull * 1024ull),
                           why);
            continue;
        }
        struct inst_disk *s = &g_disks[g_disk_count];
        s->drive = d;
        s->sectors = dev->sector_count;
        s->sector_size = dev->sector_size;
        {
            uint32_t n = 0;
            if (dev->name) {
                while (dev->name[n] && n + 1 < sizeof(s->name)) { s->name[n] = dev->name[n]; n++; }
            }
            s->name[n] = '\0';
        }
        console_printf("  [%d] drive %d  %s  %llu MiB  (%llu sectors)\n",
                       (int64_t)g_disk_count, (int64_t)d, s->name,
                       (s->sectors * 512ull) / (1024ull * 1024ull), s->sectors);
        g_disk_count++;
    }
    return g_disk_count;
}

/* ---------------- ISO source (the install medium) ---------------- */

/* The installer runs from the live ISO, so kernel.elf / limine-bios.sys are read
 * back from the mounted ISO9660 volume rather than embedded in the kernel. */
static int inst_find_iso(char *mount_out, uint32_t mount_cap) {
    int atapi = blk_atapi_device_count();
    char src[8];
    for (int i = 0; i < atapi; i++) {
        int drive = blk_atapi_drive_at(i);
        vfs_stat_t st;
        if (drive < 0) continue;
        snprintf(src, sizeof(src), "%d", drive);
        if (vfs_mount(src, "/mnt/cdsrc", "iso9660") != VFS_OK) continue;
        if (vfs_stat("/mnt/cdsrc/boot/kernel.elf", &st) == VFS_OK ||
            vfs_stat("/mnt/cdsrc/kernel.elf", &st) == VFS_OK) {
            if (mount_out && mount_cap > 1) {
                strncpy(mount_out, "/mnt/cdsrc", mount_cap - 1);
                mount_out[mount_cap - 1] = '\0';
            }
            return drive;                    /* found the live medium */
        }
        vfs_unmount("/mnt/cdsrc");
    }
    return -1;
}

/* Resolve a file on the ISO. The ISO is built with Rock Ridge + Joliet, but this
 * kernel's ISO9660 driver reads the primary volume descriptor only, so long
 * names appear in their 8.3 mangled form ("limine-bios.sys" -> "LIMINE_B.SYS").
 * Try the pretty name first (correct on a future Joliet-capable driver), then
 * the mangled one. Returns 0 and fills `out` when found. */
static int inst_find_source(const char *iso, const char *const *names,
                            uint32_t count, char *out, uint32_t cap) {
    vfs_stat_t st;
    for (uint32_t i = 0; i < count; i++) {
        snprintf(out, cap, "%s/%s", iso, names[i]);
        if (vfs_stat(out, &st) == VFS_OK) return 0;
    }
    return -1;
}

/* ---------------- file copying / text writing ---------------- */

static int inst_copy_file(const char *src, const char *dst, uint64_t *out_bytes) {
    static uint8_t chunk[INST_COPY_CHUNK];
    uint64_t total = 0;
    uint64_t next_report = 256ull * 1024ull;
    int in, out, n;

    in = vfs_open(src, VFS_O_RDONLY);
    if (in < 0) {
        console_printf("[install] cannot open source %s (err=%d)\n", src, (int64_t)in);
        return -1;
    }
    out = vfs_open(dst, VFS_O_CREAT | VFS_O_WRONLY | VFS_O_TRUNC);
    if (out < 0) {
        console_printf("[install] cannot create %s (err=%d)\n", dst, (int64_t)out);
        vfs_close(in);
        return -1;
    }
    while ((n = vfs_read(in, chunk, (uint32_t)sizeof(chunk))) > 0) {
        int off = 0;
        while (off < n) {
            int w = vfs_write(out, chunk + off, (uint32_t)(n - off));
            if (w <= 0) {
                console_printf("[install] write %s failed at %llu (err=%d)\n",
                               dst, total, (int64_t)w);
                vfs_close(in);
                vfs_close(out);
                return -1;
            }
            off += w;
        }
        total += (uint64_t)n;
        if (total >= next_report) {
            console_printf("  ... %llu KiB\n", total / 1024ull);
            next_report += 256ull * 1024ull;
        }
    }
    vfs_close(in);
    vfs_close(out);
    if (n < 0) {
        console_printf("[install] read %s failed (err=%d)\n", src, (int64_t)n);
        return -1;
    }
    if (out_bytes) *out_bytes = total;
    return 0;
}

static int inst_write_text(const char *path, const char *text) {
    uint32_t len = (uint32_t)strlen(text);
    int r = vfs_write_file(path, text, len);
    if (r != VFS_OK) {
        console_printf("[install] write %s failed (err=%d)\n", path, (int64_t)r);
        return -1;
    }
    return 0;
}

/* ---------------- accounts ---------------- */

/* /etc/shadow hash spec (same format userdb.c writes):
 *   $sutureos$pbkdf2-sha256$<iterations>$<salt-hex>$<hash-hex>
 * An empty password is stored as `!` (Linux' locked-account marker), which
 * userdb.c parses as "account has no usable hash". Documented in
 * docs/INSTALLER.md. */
static void inst_shadow_line(const char *name, const char *password,
                            char *out, uint32_t cap) {
    uint8_t salt[USERDB_SALT_BYTES];
    uint8_t hash[32];
    char salt_hex[USERDB_SALT_BYTES * 2 + 1];
    char hash_hex[65];

    if (!password || password[0] == '\0') {
        snprintf(out, cap, "%s:!:0", name);
        return;
    }
    userdb_new_salt(name, salt);
    userdb_hash_password(password, salt, USERDB_ITERATIONS, hash);
    inst_hex(salt, USERDB_SALT_BYTES, salt_hex);
    inst_hex(hash, 32, hash_hex);
    snprintf(out, cap, "%s:$sutureos$pbkdf2-sha256$%u$%s$%s:0",
             name, (unsigned)USERDB_ITERATIONS, salt_hex, hash_hex);
}

static int inst_write_accounts(const char *root_pw, const char *user_name,
                               const char *user_pw) {
    char passwd[640];
    char shadow[768];
    char group[256];
    char sudoers[640];
    char spec[192];
    int n;

    /* ---- /etc/passwd ---- */
    n = snprintf(passwd, sizeof(passwd),
                 "# /etc/passwd - SutureOS user database (written by the installer)\n"
                 "# name:passwd:uid:gid:gecos:home:shell\n"
                 "root:x:0:0:root:/root:/bin/sh\n");
    if (user_name && user_name[0]) {
        n += snprintf(passwd + n, sizeof(passwd) - (uint32_t)n,
                      "%s:x:1000:1000:SutureOS User:/home/%s:/bin/sh\n",
                      user_name, user_name);
    }

    /* ---- /etc/shadow ---- */
    n = snprintf(shadow, sizeof(shadow),
                 "# /etc/shadow - SutureOS password hashes (written by the installer)\n"
                 "# name:$sutureos$pbkdf2-sha256$<iters>$<salt>$<hash>:<flags>\n"
                 "# '!' = no password set (account locked)\n");
    inst_shadow_line("root", root_pw, spec, sizeof(spec));
    n += snprintf(shadow + n, sizeof(shadow) - (uint32_t)n, "%s\n", spec);
    if (user_name && user_name[0]) {
        inst_shadow_line(user_name, user_pw, spec, sizeof(spec));
        n += snprintf(shadow + n, sizeof(shadow) - (uint32_t)n, "%s\n", spec);
    }

    /* ---- /etc/group (the new user goes into wheel so sudo works) ---- */
    if (user_name && user_name[0]) {
        snprintf(group, sizeof(group),
                 "# /etc/group - SutureOS groups\n"
                 "root:x:0:\n"
                 "wheel:x:10:%s\n"
                 "%s:x:1000:\n", user_name, user_name);
    } else {
        snprintf(group, sizeof(group),
                 "# /etc/group - SutureOS groups\n"
                 "root:x:0:\n"
                 "wheel:x:10:\n");
    }

    /* ---- /etc/sudoers: same policy shape as the built-in seed ---- */
    n = snprintf(sudoers, sizeof(sudoers),
                 "# /etc/sudoers - SutureOS sudo policy (written by the installer)\n"
                 "Defaults timestamp_timeout=15\n"
                 "Defaults passwd_tries=3\n"
                 "root ALL=(ALL:ALL) ALL\n"
                 "%wheel ALL=(ALL:ALL) ALL\n");
    if (user_name && user_name[0]) {
        snprintf(sudoers + n, sizeof(sudoers) - (uint32_t)n,
                 "%s ALL=(ALL:ALL) ALL\n", user_name);
    }

    if (inst_write_text("/mnt/target/etc/passwd", passwd) != 0) return -1;
    if (inst_write_text("/mnt/target/etc/shadow", shadow) != 0) return -1;
    if (inst_write_text("/mnt/target/etc/group", group) != 0) return -1;
    if (inst_write_text("/mnt/target/etc/sudoers", sudoers) != 0) return -1;

    /* /etc/motd: keep the live system's text so the welcome message is identical */
    {
        size_t len = 0;
        void *motd = vfs_read_file("/etc/motd", &len);
        if (motd && len > 0) {
            int fd = vfs_open("/mnt/target/etc/motd", VFS_O_CREAT | VFS_O_WRONLY | VFS_O_TRUNC);
            if (fd >= 0) {
                (void)vfs_write(fd, motd, (uint32_t)len);
                vfs_close(fd);
            }
        }
        if (motd) lemis_heap_free(motd);
    }
    return 0;
}

/* ---------------- persistent user-data skeleton ---------------- */

/* Format version of /SUTUREOS/PERSIST (kernel/fs/persist.c reads the digits). */
#define INST_PERSIST_ROOT "/mnt/target/SUTUREOS/PERSIST"
#define INST_PERSIST_VERSION_TEXT "sutureos-persist 1\n"

/* Copy one file inside the freshly formatted volume (source and destination are
 * both VFS paths under /mnt/target). Small /etc files only. */
static int inst_copy_in_volume(const char *src, const char *dst) {
    static uint8_t buf[2048];
    int in = vfs_open(src, VFS_O_RDONLY);
    int out, n;

    if (in < 0) return -1;
    out = vfs_open(dst, VFS_O_CREAT | VFS_O_WRONLY | VFS_O_TRUNC);
    if (out < 0) { vfs_close(in); return -1; }
    while ((n = vfs_read(in, buf, (uint32_t)sizeof(buf))) > 0) {
        if (vfs_write(out, buf, (uint32_t)n) != n) { vfs_close(in); vfs_close(out); return -1; }
    }
    vfs_close(in);
    vfs_close(out);
    return (n < 0) ? -1 : 0;
}

/* The persistent user-data tree the kernel uses to survive reboots:
 *   /SUTUREOS/PERSIST/VERSION
 *   /SUTUREOS/PERSIST/etc/{passwd,shadow,group,sudoers,motd}
 *   /SUTUREOS/PERSIST/home/   (per-user homes are mirrored here at runtime)
 *   /SUTUREOS/PERSIST/root/
 * The installer seeds etc from the /etc it just wrote, so the first boot already
 * finds accounts in PERSIST and later changes are flushed back into it. */
static int inst_write_persist(const char *root_pw, const char *user_name) {
    static const char *const etc_files[] = {
        "passwd", "shadow", "group", "sudoers", "motd"
    };
    char src[128];
    char dst[128];
    int copied = 0;

    (void)root_pw; (void)user_name;
    if (vfs_mkdir_p(INST_PERSIST_ROOT) != VFS_OK) return -1;
    if (vfs_mkdir_p(INST_PERSIST_ROOT "/etc") != VFS_OK) return -1;
    if (vfs_mkdir_p(INST_PERSIST_ROOT "/home") != VFS_OK) return -1;
    if (vfs_mkdir_p(INST_PERSIST_ROOT "/root") != VFS_OK) return -1;
    if (inst_write_text(INST_PERSIST_ROOT "/VERSION",
                        INST_PERSIST_VERSION_TEXT) != 0) return -1;
    for (unsigned i = 0; i < sizeof(etc_files) / sizeof(etc_files[0]); i++) {
        snprintf(src, sizeof(src), "/mnt/target/etc/%s", etc_files[i]);
        snprintf(dst, sizeof(dst), INST_PERSIST_ROOT "/etc/%s", etc_files[i]);
        if (inst_copy_in_volume(src, dst) != 0) {
            console_printf("[install] could not seed PERSIST/etc/%s\n", etc_files[i]);
            return -1;
        }
        copied++;
    }
    console_printf("[install]   " INST_PERSIST_ROOT
                   " = VERSION + etc/(%d files) + home/ + root/\n", (int64_t)copied);
    return 0;
}

/* ---------------- the destructive part ---------------- */

static int inst_write_boot_area(int drive, uint32_t part_start, uint32_t part_sectors) {
    uint8_t *sector0;
    uint8_t *stage2;
    uint32_t stage2_len = (uint32_t)sizeof(binary_limine_hdd_bin_data) - 512u;
    uint32_t stage2_sectors = (stage2_len + 511u) / 512u;
    uint32_t stage2_lba = INST_STAGE2_OFFSET / 512u;
    uint8_t entry[16];

    sector0 = lemis_heap_alloc(512);
    stage2 = lemis_heap_alloc(stage2_sectors * 512u);
    if (!sector0 || !stage2) {
        if (sector0) lemis_heap_free(sector0);
        if (stage2) lemis_heap_free(stage2);
        inst_puts("[install] out of memory for the boot area\n");
        return -1;
    }

    /* Sector 0 = Limine's MBR image, with our partition table on top and the
     * stage-2 offset patched in exactly like limine bios-install does. */
    memcpy(sector0, binary_limine_hdd_bin_data, 512);
    inst_put32(sector0, INST_STAGE2_PATCH, INST_STAGE2_OFFSET);
    inst_put32(sector0, INST_STAGE2_PATCH + 4, 0);          /* u64 high half */

    memset(entry, 0, sizeof(entry));
    entry[0] = 0x80;                                        /* active / bootable */
    inst_chs(entry + 1, part_start);
    entry[4] = 0x0C;                                        /* FAT32 LBA */
    inst_chs(entry + 5, part_start + part_sectors - 1u);
    inst_put32(entry, 8, part_start);
    inst_put32(entry, 12, part_sectors);
    memcpy(sector0 + 446, entry, 16);                       /* partition 1 */
    memset(sector0 + 462, 0, 16 * 3);                       /* partitions 2..4 */
    sector0[510] = 0x55;
    sector0[511] = 0xAA;

    if (blk_write(drive, 0, 1, sector0) != 0) {
        inst_puts("[install] MBR write failed\n");
        lemis_heap_free(sector0);
        lemis_heap_free(stage2);
        return -1;
    }
    memset(stage2, 0, stage2_sectors * 512u);
    memcpy(stage2, binary_limine_hdd_bin_data + 512, stage2_len);
    if (blk_write(drive, stage2_lba, stage2_sectors, stage2) != 0) {
        inst_puts("[install] Limine stage 2 write failed\n");
        lemis_heap_free(sector0);
        lemis_heap_free(stage2);
        return -1;
    }
    lemis_heap_free(sector0);
    lemis_heap_free(stage2);
    console_printf("[install] MBR + Limine BIOS stage 2 written (partition %u..%u,"
                   " stage2 at LBA %u, %u sectors)\n",
                   (uint64_t)part_start, (uint64_t)(part_start + part_sectors - 1u),
                   (uint64_t)stage2_lba, (uint64_t)stage2_sectors);
    return 0;
}

static const char k_limine_conf[] =
    "# SutureOS installed system (written by the installer)\n"
    "# Loaded by Limine (BSD-2-Clause) from the FAT32 system volume.\n"
    "timeout: 0\n"
    "default_entry: 1\n"
    "serial: yes\n"
    "\n"
    "/SutureOS\n"
    "    comment: SutureOS - a stitched-together hobby OS kernel (GPL-3.0)\n"
    "    protocol: multiboot\n"
    "    path: boot():/kernel.elf\n"
    "\n"
    "/SutureOS (verbose)\n"
    "    comment: SutureOS - verbose boot with full self-test output\n"
    "    protocol: multiboot\n"
    "    path: boot():/kernel.elf\n"
    "    cmdline: verbose\n";

static int inst_install(int drive, const char *root_pw,
                        const char *user_name, const char *user_pw) {
    struct inst_disk *d = NULL;
    struct fat32_geometry geo;
    uint32_t part_start = INST_PART_START;
    uint32_t part_sectors;
    char src[VFS_MAX_PATH];
    const char *iso = "/mnt/cdsrc";
    uint64_t bytes = 0;
    int r;
    char target[16];

    for (int i = 0; i < g_disk_count; i++)
        if (g_disks[i].drive == drive) d = &g_disks[i];
    if (!d) return -1;
    if (d->sectors <= part_start + INST_MIN_SECTORS) {
        inst_puts("[install] disk too small for a FAT32 system partition\n");
        return -1;
    }
    part_sectors = (uint32_t)(d->sectors - part_start);

    inst_puts("\n");
    console_printf("[install] target: drive %d (%s), %llu MiB, %llu sectors\n",
                   (int64_t)d->drive, d->name,
                   (d->sectors * 512ull) / (1024ull * 1024ull), d->sectors);
    console_printf("[install] partition 1: type 0x0C active, LBA %u..%u (%llu MiB)\n",
                   (uint64_t)part_start, (uint64_t)(part_start + part_sectors - 1u),
                   ((uint64_t)part_sectors * 512ull) / (1024ull * 1024ull));

    /* 1. boot area (MBR + Limine stage 2) */
    if (inst_write_boot_area(d->drive, part_start, part_sectors) != 0) return -1;

    /* 2. format the system partition */
    inst_puts("[install] formatting FAT32 system partition ...\n");
    if (fat32_format_partition(d->drive, part_start, part_sectors, &geo) != FATFS_OK) {
        inst_puts("[install] FAT32 format failed\n");
        return -1;
    }

    /* 3. mount it and lay out the files */
    snprintf(target, sizeof(target), "%d:%u", d->drive, part_start);
    r = vfs_mount(target, "/mnt/target", "fat32");
    if (r != VFS_OK) {
        console_printf("[install] mount of the new volume failed (err=%d)\n", (int64_t)r);
        return -1;
    }
    if (vfs_mkdir_p("/mnt/target/SUTUREOS") != VFS_OK ||
        vfs_mkdir_p("/mnt/target/etc") != VFS_OK) {
        inst_puts("[install] mkdir on the new volume failed\n");
        return -1;
    }

    /* 4. kernel + Limine payload, read back from the live ISO */
    {
        static const char *const kernel_names[] = {
            "boot/kernel.elf", "kernel.elf", "BOOT/KERNEL.ELF", "KERNEL.ELF"
        };
        if (inst_find_source(iso, kernel_names,
                             (uint32_t)(sizeof(kernel_names) / sizeof(kernel_names[0])),
                             src, sizeof(src)) != 0) {
            inst_puts("[install] no kernel.elf on the install medium\n");
            return -1;
        }
        console_printf("[install] kernel source: %s\n", src);
    }
    /* Multi-MiB copies: skip the per-sector read-back verify (still a real
     * write, still FAT-verified and synced) so installation takes a minute. */
    fatfs_set_bulk_mode(1);
    inst_puts("[install] copying kernel.elf ...\n");
    if (inst_copy_file(src, "/mnt/target/kernel.elf", &bytes) != 0) return -1;
    console_printf("[install]   /kernel.elf = %llu bytes\n", bytes);

    {
        static const char *const limine_names[] = {
            "boot/limine/limine-bios.sys", "boot/limine/LIMINE_B.SYS",
            "limine-bios.sys", "LIMINE_B.SYS"
        };
        if (inst_find_source(iso, limine_names,
                             (uint32_t)(sizeof(limine_names) / sizeof(limine_names[0])),
                             src, sizeof(src)) != 0) {
            inst_puts("[install] no limine-bios.sys on the install medium\n");
            return -1;
        }
        console_printf("[install] limine source: %s\n", src);
    }
    inst_puts("[install] copying limine-bios.sys ...\n");
    if (inst_copy_file(src, "/mnt/target/limine-bios.sys", &bytes) != 0) return -1;
    console_printf("[install]   /limine-bios.sys = %llu bytes\n", bytes);

    if (inst_write_text("/mnt/target/limine.conf", k_limine_conf) != 0) return -1;

    /* 5. marker + accounts */
    {
        char marker[256];
        snprintf(marker, sizeof(marker),
                 "SutureOS installed system\n"
                 "volume=1 partition_start=%u\n"
                 "kernel=/kernel.elf\n"
                 "bootloader=limine-bios\n"
                 "root_password=%s\n"
                 "user=%s\n",
                 (unsigned)part_start,
                 (root_pw && root_pw[0]) ? "set" : "none",
                 (user_name && user_name[0]) ? user_name : "none");
        if (inst_write_text("/mnt/target/SUTUREOS/INSTALLED", marker) != 0) return -1;
    }
    inst_puts("[install] writing /etc (passwd, shadow, group, sudoers, motd) ...\n");
    if (inst_write_accounts(root_pw, user_name, user_pw) != 0) return -1;

    /* 5b. persistent user-data skeleton (kernel/fs/persist.c, format v1):
     *     /SUTUREOS/PERSIST/{VERSION,etc/{passwd,shadow,group,sudoers,motd},
     *     home/,root/}. The installer seeds it with the same /etc files it just
     *     wrote, so the very first boot restores them from PERSIST and later
     *     runtime changes (useradd, /etc/motd, /home files) are flushed back
     *     into the same tree on `sync` / `shutdown`. */
    inst_puts("[install] writing /SUTUREOS/PERSIST skeleton ...\n");
    if (inst_write_persist(root_pw, user_name) != 0) return -1;

    /* 6. flush and unmount: the volume must be consistent on disk */
    fatfs_set_bulk_mode(0);
    (void)vfs_sync("/mnt/target");
    if (vfs_unmount("/mnt/target") != VFS_OK) {
        inst_puts("[install] warning: unmount of the new volume failed\n");
    }
    (void)blk_flush(d->drive);

    /* 7. hand the new system volume to this live session's persistence layer:
     *    mount it at /mnt/sys (the mountpoint kernel/fs/persist.c expects) and
     *    arm the mirror, so `sync` / `shutdown` in this same session already
     *    write /etc, /home and /root onto the fresh disk. Without this the very
     *    first session after installation would still be volatile. */
    {
        char src[8];
        snprintf(src, sizeof(src), "%d", d->drive);
        if (vfs_mount(src, "/mnt/sys", "fat32") == VFS_OK) {
            extern int persist_detect(void);
            (void)persist_detect();
            console_printf("[install] system volume mounted at /mnt/sys, "
                           "persistence active for this session\n");
        } else {
            inst_puts("[install] warning: could not mount the new system volume "
                      "at /mnt/sys (persistence in this session is off)\n");
        }
    }

    console_printf("[install] done (drive %d, partition LBA %u, root_password=%s"
                   ", user=%s)\n",
                   (int64_t)d->drive, (uint64_t)part_start,
                   (root_pw && root_pw[0]) ? "set" : "none",
                   (user_name && user_name[0]) ? user_name : "none");
    inst_puts("[install] Reboot, remove the ISO and boot from the hard disk.\n");
    return 0;
}

/* ---------------- entry point (shell `install`) ---------------- */

/* ==================== system upgrade (overwrite only, v0.36.0) ============
 * "Upgrade" is deliberately the simple, honest operation the user asked for:
 * overwrite the SYSTEM files on the installed volume with the ones from the
 * current install medium (kernel.elf, limine-bios.sys, limine.conf), refresh
 * the MBR + Limine stage 2, and bump the recorded version. User data -
 * /etc, /home, /root and the whole /SUTUREOS/PERSIST tree - is never touched.
 *
 * The installed volume is found by content (a FAT32 volume carrying
 * /SUTUREOS/INSTALLED), so this works from a live ISO session where the
 * installed disk is just another data disk.
 */
static int inst_find_installed(char *mount_out, uint32_t mount_cap,
                               int *drive_out, uint32_t *part_start,
                               uint32_t *part_sectors) {
    uint8_t *mbr = lemis_heap_alloc(512);
    if (!mbr) return -1;

    for (int i = 0; i < g_disk_count; i++) {
        const int drive = g_disks[i].drive;
        char dev[8];
        vfs_stat_t st;
        snprintf(dev, sizeof(dev), "%d", drive);
        if (vfs_mount(dev, "/mnt/upg", "fat32") != VFS_OK) continue;
        if (vfs_stat("/mnt/upg/SUTUREOS/INSTALLED", &st) != VFS_OK) {
            vfs_unmount("/mnt/upg");
            continue;
        }
        /* read the MBR to recover the partition geometry for the boot area */
        if (blk_read(drive, 0, 1, mbr) != 0) {
            vfs_unmount("/mnt/upg");
            continue;
        }
        int found = 0;
        for (int part = 0; part < 4; part++) {
            const uint8_t *e = mbr + 446 + part * 16;
            const uint8_t type = e[4];
            if (type != 0x0B && type != 0x0C && type != 0x0E) continue;
            const uint32_t lba = (uint32_t)e[8] | ((uint32_t)e[9] << 8) |
                                 ((uint32_t)e[10] << 16) | ((uint32_t)e[11] << 24);
            const uint32_t secs = (uint32_t)e[12] | ((uint32_t)e[13] << 8) |
                                  ((uint32_t)e[14] << 16) | ((uint32_t)e[15] << 24);
            if (secs < INST_MIN_SECTORS) continue;
            *drive_out = drive;
            *part_start = lba;
            *part_sectors = secs;
            found = 1;
            break;
        }
        if (!found) {
            vfs_unmount("/mnt/upg");
            continue;
        }
        if (mount_out && mount_cap > 1) {
            strncpy(mount_out, "/mnt/upg", mount_cap - 1);
            mount_out[mount_cap - 1] = '\0';
        }
        lemis_heap_free(mbr);
        return 0;
    }
    lemis_heap_free(mbr);
    return -1;
}

static int inst_write_version(const char *mount, const char *extra) {
    char buf[256];
    int n = snprintf(buf, sizeof(buf),
                     "sutureos-installed 1\nversion=%s\nupgraded=1\n%s",
                     SUTUREOS_VERSION, extra ? extra : "");
    if (n < 0 || n >= (int)sizeof(buf)) return -1;
    return inst_write_text("/mnt/upg/SUTUREOS/VERSION", buf);
}

int installer_upgrade(void) {
    char iso_mount[16];
    int iso_drive;
    char mount[16];
    int drive = -1;
    uint32_t part_start = 0, part_sectors = 0;
    const char *const kernel_names[] = {"boot/kernel.elf", "kernel.elf",
                                        "KERNEL.ELF"};
    const char *const limine_names[] = {"boot/limine/limine-bios.sys",
                                        "limine-bios.sys", "LIMINE_B.SYS",
                                        "LIMINE_BIOS.SYS"};
    const char *const conf_names[] = {"boot/limine.conf", "limine.conf",
                                      "LIMINE.CONF"};
    char src[VFS_MAX_PATH];

    if (g_disk_count == 0) inst_collect_disks();

    iso_drive = inst_find_iso(iso_mount, sizeof(iso_mount));
    if (iso_drive < 0) {
        inst_puts("[upgrade] no install medium found (boot the ISO to upgrade)\n");
        return -1;
    }

    if (inst_find_installed(mount, sizeof(mount), &drive, &part_start,
                            &part_sectors) != 0) {
        inst_puts("[upgrade] no installed SutureOS volume found on any disk\n");
        vfs_unmount(iso_mount);
        return -1;
    }
    console_printf("[upgrade] installed system on drive %d (partition LBA %u..%u)\n",
                   (int64_t)drive, (uint64_t)part_start,
                   (uint64_t)(part_start + part_sectors - 1u));
    console_printf("[upgrade] upgrade medium: drive %d, target version %s\n",
                   (int64_t)iso_drive, SUTUREOS_VERSION);
    fatfs_set_bulk_mode(1);   /* same acceleration the installer uses */

    /* 1) kernel.elf - the actual new system */
    if (inst_find_source(iso_mount, kernel_names,
                         (uint32_t)(sizeof(kernel_names) / sizeof(kernel_names[0])),
                         src, sizeof(src)) != 0) {
        inst_puts("[upgrade] kernel.elf not found on the medium\n");
        goto fail;
    }
    {
        uint64_t bytes = 0;
        if (inst_copy_file(src, "/mnt/upg/kernel.elf", &bytes) != 0) goto fail;
        console_printf("[upgrade]   /kernel.elf <- %s (%llu bytes)\n",
                       src, bytes);
    }

    /* 2) limine-bios.sys (stage 2 payload) */
    if (inst_find_source(iso_mount, limine_names,
                         (uint32_t)(sizeof(limine_names) / sizeof(limine_names[0])),
                         src, sizeof(src)) == 0) {
        uint64_t bytes = 0;
        if (inst_copy_file(src, "/mnt/upg/limine-bios.sys", &bytes) != 0) goto fail;
        console_printf("[upgrade]   /limine-bios.sys <- %s (%llu bytes)\n",
                       src, bytes);
    } else {
        inst_puts("[upgrade]   limine-bios.sys not on the medium (kept the old one)\n");
    }

    /* 3) limine.conf - regenerate so the boot entry is always the current one */
    {
        int out = vfs_open("/mnt/upg/limine.conf",
                           VFS_O_CREAT | VFS_O_WRONLY | VFS_O_TRUNC);
        if (out < 0) {
            inst_puts("[upgrade] cannot write limine.conf\n");
            goto fail;
        }
        vfs_write(out, (const void *)k_limine_conf,
                  (uint32_t)strlen(k_limine_conf));
        vfs_close(out);
    }

    /* 4) refresh MBR + Limine stage 2 on the installed disk */
    if (inst_write_boot_area(drive, part_start, part_sectors) != 0) goto fail;

    /* 5) record the new version (PERSIST and /etc are left untouched) */
    (void)inst_write_version(mount, "");

    vfs_sync(mount);
    fatfs_set_bulk_mode(0);
    vfs_unmount(mount);
    vfs_unmount(iso_mount);
    console_printf("[upgrade] done (drive %d, system files overwritten,"
                   " user data kept)\n", (int64_t)drive);
    return 0;

fail:
    fatfs_set_bulk_mode(0);
    vfs_unmount(mount);
    vfs_unmount(iso_mount);
    inst_puts("[upgrade] FAILED - the installed system was not upgraded\n");
    return -1;
}


int installer_run(void) {
    char root_pw[64], root_pw2[64];
    char user_name[USERDB_NAME_LEN];
    char user_pw[64], user_pw2[64];
    char answer[16];
    char name_ok[USERDB_NAME_LEN];
    int sel, drive, iso;

    inst_puts("\n=== SutureOS installer (terminal) ===\n");
    inst_puts("This installs SutureOS onto a real hard disk. Nothing is written\n"
              "until the final confirmation.\n");

    iso = inst_find_iso(NULL, 0);
    if (iso < 0) {
        inst_puts("[install] ERROR: the install medium (ISO9660 with /boot/kernel.elf)"
                  " was not found.\n");
        inst_puts("[install] Boot the installation ISO and try again.\n");
        return -1;
    }
    console_printf("[install] install medium: ISO9660 on drive %d (/mnt/cdsrc)\n",
                   (int64_t)iso);

    if (inst_collect_disks() <= 0) {
        inst_puts("[install] no writable hard disk found - nothing to do.\n");
        return -1;
    }

    inst_puts("\nTarget disk index [q = quit]: ");
    if (inst_readline("", answer, sizeof(answer), 1) < 0) return -1;
    if (answer[0] == 'q' || answer[0] == 'Q') { inst_puts("[install] cancelled.\n"); return -1; }
    if (answer[0] < '0' || answer[0] > '9') {
        inst_puts("[install] that is not a disk index.\n");
        return -1;
    }
    sel = 0;
    for (const char *p = answer; *p >= '0' && *p <= '9'; p++) sel = sel * 10 + (*p - '0');
    if (sel < 0 || sel >= g_disk_count) {
        inst_puts("[install] no such disk index.\n");
        return -1;
    }
    drive = g_disks[sel].drive;

    console_printf("\nSelected disk %d (%s, %llu MiB).\n", (int64_t)drive,
                   g_disks[sel].name,
                   (g_disks[sel].sectors * 512ull) / (1024ull * 1024ull));
    inst_puts("  !! EVERYTHING on this disk will be erased.\n");
    inst_puts("Type 'yes' to continue: ");
    if (inst_readline("", answer, sizeof(answer), 1) < 0) return -1;
    if (strcmp(answer, "yes") != 0) {
        inst_puts("[install] cancelled - nothing was written.\n");
        return -1;
    }

    /* root password: empty = no password (shadow field '!') */
    if (inst_read_password("root password (empty = no password): ",
                           root_pw, sizeof(root_pw)) < 0) return -1;
    if (root_pw[0]) {
        if (inst_read_password("repeat root password: ", root_pw2, sizeof(root_pw2)) < 0)
            return -1;
        if (strcmp(root_pw, root_pw2) != 0) {
            inst_puts("[install] passwords do not match - cancelled.\n");
            return -1;
        }
    }

    /* optional normal user */
    user_name[0] = '\0';
    user_pw[0] = '\0';
    inst_puts("Create a normal user? [y/N]: ");
    if (inst_readline("", answer, sizeof(answer), 1) < 0) return -1;
    if (answer[0] == 'y' || answer[0] == 'Y') {
        int n;
        if (inst_readline("user name: ", name_ok, sizeof(name_ok), 1) < 0) return -1;
        n = 0;
        while (name_ok[n]) {
            char c = name_ok[n];
            int ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                     c == '_' || c == '-';
            if (!ok || n >= (int)sizeof(user_name) - 1) {
                inst_puts("[install] invalid user name (lowercase letters, digits,"
                          " '_' and '-' only) - cancelled.\n");
                return -1;
            }
            n++;
        }
        if (n == 0) { inst_puts("[install] empty user name - cancelled.\n"); return -1; }
        strncpy(user_name, name_ok, sizeof(user_name) - 1);
        user_name[sizeof(user_name) - 1] = '\0';
        if (inst_read_password("user password: ", user_pw, sizeof(user_pw)) < 0) return -1;
        if (user_pw[0] == '\0') {
            inst_puts("[install] the normal user needs a password - cancelled.\n");
            return -1;
        }
        if (inst_read_password("repeat user password: ", user_pw2, sizeof(user_pw2)) < 0)
            return -1;
        if (strcmp(user_pw, user_pw2) != 0) {
            inst_puts("[install] passwords do not match - cancelled.\n");
            return -1;
        }
    }

    if (inst_install(drive, root_pw, user_name, user_pw) != 0) {
        inst_puts("[install] FAILED - see the messages above.\n");
        return -1;
    }
    return 0;
}
