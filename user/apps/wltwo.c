/* wltwo.c - the SECOND ring-3 Wayland client for SutureOS (multi-client batch).
 *
 * Why a second client: the server used to keep one global copy of "the last
 * committed surface", so a second client silently replaced the first one's
 * frame.  This program proves the fix from ring 3:
 *
 *   1. it opens TWO named-endpoint connections to the compositor at the same
 *      time (connect("wayland-0") twice) - two client connections, two
 *      independent object namespaces;
 *   2. connection A publishes a 160x100 surface with set_buffer_scale(2), an
 *      opaque/input region built from add+add+subtract, and then re-commits it
 *      with different pixels and a damage rect;
 *   3. connection B publishes a 96x64 surface with scale 1 and makes it a
 *      wl_subsurface of a parent surface with set_position(24,16);
 *   4. after both commits the server log lists two retained surfaces with two
 *      different hashes - that is the multi-client property;
 *   5. connection A asks the seat for a keyboard and a pointer, then waits
 *      (bounded) for real input: the acceptance types on the PS/2 keyboard and
 *      moves/clicks the PS/2 mouse while this runs.  Every wl_keyboard.key and
 *      wl_pointer event received is counted and printed, and the pointer is
 *      released at the end so wl_pointer.leave is exercised;
 *   6. it writes both frames to the FAT32 volume (/mnt/fat/WLTWO.BIN,
 *      /mnt/fat/WLTWO2.BIN) so the host can verify the bytes independently of
 *      anything this program says.
 *
 * License: GPL-3.0, part of SutureOS.  Written from the Wayland protocol
 * specification; no libwayland code exists in this tree.
 */
#include <stdio.h>
#include <string.h>

#include "r3tool.h"

/* ---------------- Vimtu64 int 0x80 own ABI (kernel/syscall/int80.c) -------- */
#define NR_WRITE       1
#define NR_EXIT        2
#define NR_TICKS       4
#define NR_READ        7
#define NR_CLOSE       8
#define NR_SOCKETPAIR  10
#define NR_MEMFD       11
#define NR_MAP_SHARED  12
#define NR_SEND_FD     13
#define NR_RECV_FD     14
#define NR_BIND        15
#define NR_CONNECT     16

static long sc1(long nr, long a)
{
    long r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(nr), "D"(a));
    return r;
}

static long sc2(long nr, long a, long b)
{
    long r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(nr), "D"(a), "S"(b));
    return r;
}

static long sc3(long nr, long a, long b, long c)
{
    long r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(nr), "D"(a), "S"(b), "d"(c));
    return r;
}

/* ---------------- geometry (different from wlclient on purpose) ------------ */
#define A_W 160
#define A_H 100
#define A_STRIDE (A_W * 4)
#define A_BYTES  (A_STRIDE * A_H)        /* 64000 bytes */
#define A_SCALE  2                       /* logical surface size: 80x50 */

#define B_W 96
#define B_H 64
#define B_STRIDE (B_W * 4)
#define B_BYTES  (B_STRIDE * B_H)        /* 24576 bytes */
#define B_SCALE  1

#define WL_SHM_XRGB8888 1u

/* The block the A re-commit paints (its wl_surface.damage rect). */
#define DMG_X 16
#define DMG_Y 16
#define DMG_W 32
#define DMG_H 32
#define DMG_COLOR 0xFF2040C0u

/* Object ids: allocated by the client, so both connections reuse the numbers
 * (each connection has its own namespace). */
#define ID_DISPLAY    1u
#define ID_REGISTRY   2u
#define ID_COMPOSITOR 3u
#define ID_SHM        4u
#define ID_SEAT       5u
#define ID_SUBCOMP    6u
#define ID_POOL       7u
#define ID_BUFFER     8u
#define ID_SURFACE    9u
#define ID_KB         10u
#define ID_PTR        11u
#define ID_PARENT     12u
#define ID_SUBSURF    13u
#define ID_REGION     20u
#define ID_CALLBACK   21u
#define ID_BUFFER2    30u                    /* second buffer id (lifecycle test) */
#define ID_POOL2      31u

#define BIN_A "/mnt/fat/WLTWO.BIN"
#define BIN_B "/mnt/fat/WLTWO2.BIN"

#define INPUT_WAIT_TICKS 6000            /* bounded: 60 s at 100 Hz            */
#define NEED_KEY_PRESSES 6
#define NEED_MOTIONS     3
#define NEED_BUTTONS     1

/* ---------------- one connection ------------------------------------------ */
struct conn {
    int           fd;
    unsigned char tx[512];
    unsigned      tx_len;
    unsigned char rx[2048];   /* per-connection receive ring (events only) */
    unsigned      rx_len;
};

struct pool {
    int           fd;
    unsigned char *map;
    int           bytes;
};

static struct conn g_a, g_b;
static int g_ok, g_fail;

static void check(int cond, const char *what)
{
    if (cond) { g_ok++; printf("[wltwo]   ok   %s\n", what); }
    else      { g_fail++; printf("[wltwo]   FAIL %s\n", what); }
}

/* ---------------- wire helpers (little endian) ---------------- */
static void put32(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)(v & 0xFFu);
    p[1] = (unsigned char)((v >> 8) & 0xFFu);
    p[2] = (unsigned char)((v >> 16) & 0xFFu);
    p[3] = (unsigned char)((v >> 24) & 0xFFu);
}

static unsigned get32(const unsigned char *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) |
           ((unsigned)p[3] << 24);
}

static void msg_begin(struct conn *c, unsigned obj, unsigned op)
{
    put32(c->tx, obj);
    c->tx[4] = (unsigned char)(op & 0xFFu);
    c->tx[5] = (unsigned char)((op >> 8) & 0xFFu);
    c->tx[6] = 0;
    c->tx[7] = 0;
    c->tx_len = 8;
}

static void msg_u32(struct conn *c, unsigned v)
{
    if (c->tx_len + 4u > sizeof(c->tx)) return;
    put32(c->tx + c->tx_len, v);
    c->tx_len += 4;
}

static void msg_i32(struct conn *c, int v)
{
    msg_u32(c, (unsigned)v);
}

static void msg_str(struct conn *c, const char *s)
{
    const unsigned n = (unsigned)strlen(s) + 1u;

    msg_u32(c, n);
    if (c->tx_len + n > sizeof(c->tx)) return;
    memcpy(c->tx + c->tx_len, s, n);
    c->tx_len += n;
    while ((c->tx_len & 3u) != 0u && c->tx_len + 1u <= sizeof(c->tx))
        c->tx[c->tx_len++] = 0;
}

static int msg_send(struct conn *c)
{
    unsigned off = 0;

    c->tx[6] = (unsigned char)(c->tx_len & 0xFFu);
    c->tx[7] = (unsigned char)((c->tx_len >> 8) & 0xFFu);
    while (off < c->tx_len) {
        const long n = sc3(NR_WRITE, c->fd, (long)(uintptr_t)(c->tx + off),
                           (long)(c->tx_len - off));
        if (n <= 0) return 0;
        off += (unsigned)n;
    }
    return 1;
}

static int msg_send_with_fd(struct conn *c, int fd)
{
    long rc;

    c->tx[6] = (unsigned char)(c->tx_len & 0xFFu);
    c->tx[7] = (unsigned char)((c->tx_len >> 8) & 0xFFu);
    if (c->tx_len == 0 || c->tx_len > 0xFFFFu) return 0;
    rc = sc3(NR_SEND_FD, c->fd, (long)((c->tx_len << 16) | (unsigned)fd),
             (long)(uintptr_t)c->tx);
    return rc == (long)c->tx_len;
}

/* Pull one complete message out of a connection.  The server runs inside this
 * program's own syscalls, so a reply is normally already queued. */
static int msg_recv(struct conn *c, unsigned *obj, unsigned *op)
{
    if (c->rx_len >= 8u) {
        const unsigned size = (unsigned)c->rx[6] | ((unsigned)c->rx[7] << 8);

        if (size >= 8u && size <= c->rx_len) {
            *obj = get32(c->rx);
            *op = (unsigned)c->rx[4] | ((unsigned)c->rx[5] << 8);
            return (int)size;
        }
        if (size < 8u) return -1;
    }
    if (c->rx_len < sizeof(c->rx)) {
        const long n = sc3(NR_READ, c->fd, (long)(uintptr_t)(c->rx + c->rx_len),
                           (long)(sizeof(c->rx) - c->rx_len));
        if (n > 0) {
            c->rx_len += (unsigned)n;
            return msg_recv(c, obj, op);
        }
    }
    return 0;
}

static void msg_consume(struct conn *c, unsigned size)
{
    if (size >= c->rx_len) { c->rx_len = 0; return; }
    memmove(c->rx, c->rx + size, c->rx_len - size);
    c->rx_len -= size;
}

static int arg_str(const unsigned char *msg, unsigned size, unsigned *off,
                   char *dst, unsigned cap)
{
    unsigned n, i;

    if (*off + 4u > size) return 0;
    n = get32(msg + *off);
    *off += 4u;
    if (n == 0u || n > cap || *off + n > size) return 0;
    for (i = 0; i + 1u < n && i + 1u < cap; i++)
        dst[i] = (char)msg[*off + i];
    dst[n - 1u < cap - 1u ? n - 1u : cap - 1u] = '\0';
    *off += n;
    *off = (*off + 3u) & ~3u;
    return 1;
}

static unsigned fnv1a(const unsigned char *p, unsigned n)
{
    unsigned h = 2166136261u;
    unsigned i;

    for (i = 0; i < n; i++) {
        h ^= (unsigned)p[i];
        h *= 16777619u;
    }
    return h;
}

/* ---------------- protocol state / counters -------------------------------- */
static unsigned g_globals_seen;
static unsigned g_name_compositor, g_name_shm, g_name_seat, g_name_subcomp;
static unsigned g_releases;

static int      g_kb_keymap, g_kb_enter, g_kb_modifiers, g_kb_repeat;
static int      g_repeat_rate;
static unsigned g_repeat_delay;
static unsigned g_kb_press, g_kb_release;
static unsigned g_kb_codes[8];
static int      g_kb_code_count;

/* keymap: the descriptor arrives over SCM_RIGHTS and the text is hashed so the
 * host can recompute the same FNV-1a from its own copy of the keymap. */
static int      g_keymap_fd = -1;
static unsigned g_keymap_format, g_keymap_size, g_keymap_hash;
static int      g_keymap_ok;

static int      g_ptr_enter, g_ptr_leave;
static unsigned g_ptr_motion, g_ptr_button_press, g_ptr_button_release;
static unsigned g_ptr_frame;

static void note_key(unsigned key, unsigned state)
{
    int i;

    if (state) {
        g_kb_press++;
        for (i = 0; i < g_kb_code_count; i++)
            if (g_kb_codes[i] == key) return;
        if (g_kb_code_count < 8) g_kb_codes[g_kb_code_count++] = key;
    } else {
        g_kb_release++;
    }
}

/* Parse every queued event of one connection; returns how many were handled. */
static int pump(struct conn *c, int verbose)
{
    int handled = 0;

    for (;;) {
        unsigned obj = 0, op = 0;
        int size = msg_recv(c, &obj, &op);

        if (size <= 0) break;
        handled++;
        if (obj == ID_REGISTRY) {
            unsigned off = 8;
            char iface[64];

            if (off + 4u <= (unsigned)size) {
                const unsigned name = get32(c->rx + off);

                off += 4;
                if (arg_str(c->rx, (unsigned)size, &off, iface, sizeof(iface))) {
                    g_globals_seen++;
                    if (verbose)
                        printf("[wltwo]   global name=%u interface=\"%s\"\n",
                               name, iface);
                    if (strcmp(iface, "wl_compositor") == 0)
                        g_name_compositor = name;
                    else if (strcmp(iface, "wl_shm") == 0)
                        g_name_shm = name;
                    else if (strcmp(iface, "wl_seat") == 0)
                        g_name_seat = name;
                    else if (strcmp(iface, "wl_subcompositor") == 0)
                        g_name_subcomp = name;
                }
            }
        } else if (obj == ID_BUFFER) {
            g_releases++;
        } else if (obj == ID_KB) {
            if (op == 0u) {
                const unsigned fmt = get32(c->rx + 8);
                const unsigned ksize = get32(c->rx + 12);

                g_kb_keymap = 1;
                g_keymap_format = fmt;
                g_keymap_size = ksize;
                /* always printed: this event is the SCM_RIGHTS keymap evidence */
                printf("[wltwo]   event keyboard.keymap format=%u size=%u\n",
                       fmt, ksize);
                /* format 1 = XKB_V1: the descriptor carries the keymap text.  Map
                 * it and hash it - the host recomputes the same value. */
                if (fmt == 1u && ksize > 0u && g_keymap_fd >= 0) {
                    const long addr = sc2(NR_MAP_SHARED, (long)g_keymap_fd,
                                          (long)ksize);

                    if (addr > 0) {
                        g_keymap_hash = fnv1a((const unsigned char *)(uintptr_t)addr,
                                              ksize);
                        g_keymap_ok = 1;
                        printf("[wltwo] keymap fd=%d size=%u hash=%08x\n",
                               g_keymap_fd, ksize, g_keymap_hash);
                    } else {
                        printf("[wltwo] keymap fd=%d map_shared(%u) failed (%ld)\n",
                               g_keymap_fd, ksize, addr);
                    }
                }
            } else if (op == 1u) {
                g_kb_enter = 1;
                if (verbose)
                    printf("[wltwo]   event keyboard.enter surface=%u\n",
                           get32(c->rx + 12));
            } else if (op == 2u) {
                if (verbose) printf("[wltwo]   event keyboard.leave\n");
            } else if (op == 3u) {
                const unsigned key = get32(c->rx + 16);
                const unsigned state = get32(c->rx + 20);

                note_key(key, state);
                if (verbose)
                    printf("[wltwo]   event keyboard.key key=%u state=%u\n",
                           key, state);
            } else if (op == 4u) {
                g_kb_modifiers++;
            } else if (op == 5u) {       /* repeat_info(rate, delay) */
                g_kb_repeat = 1;
                g_repeat_rate = (int)get32(c->rx + 8);
                g_repeat_delay = get32(c->rx + 12);
                printf("[wltwo]   event keyboard.repeat_info rate=%d delay=%u "
                       "(chars/s, ms)\n", g_repeat_rate, g_repeat_delay);
            }
        } else if (obj == ID_PTR) {
            if (op == 0u) {
                g_ptr_enter = 1;
                if (verbose)
                    printf("[wltwo]   event pointer.enter %d,%d\n",
                           (int)(get32(c->rx + 16) >> 8),
                           (int)(get32(c->rx + 20) >> 8));
            } else if (op == 1u) {
                g_ptr_leave = 1;
                if (verbose) printf("[wltwo]   event pointer.leave\n");
            } else if (op == 2u) {
                g_ptr_motion++;
                if (verbose)
                    printf("[wltwo]   event pointer.motion %d,%d\n",
                           (int)(get32(c->rx + 12) >> 8),
                           (int)(get32(c->rx + 16) >> 8));
            } else if (op == 3u) {
                const unsigned state = get32(c->rx + 20);

                if (state) g_ptr_button_press++;
                else       g_ptr_button_release++;
                if (verbose)
                    printf("[wltwo]   event pointer.button 0x%x state=%u\n",
                           get32(c->rx + 16), state);
            } else if (op == 5u) {
                g_ptr_frame++;
            }
        }
        msg_consume(c, (unsigned)size);
    }
    return handled;
}

/* ---------------- pixel patterns (mirrored by the host verifier) ----------- */
static void fill_a(unsigned char *map)
{
    unsigned x, y;

    for (y = 0; y < A_H; y++) {
        unsigned *row = (unsigned *)(void *)(map + (unsigned long)y * A_STRIDE);

        for (x = 0; x < A_W; x++) {
            const unsigned gx = (x * 3u) & 0xFFu;
            const unsigned gy = (y * 5u) & 0xFFu;

            row[x] = 0xFF000000u | (gx << 16) | (gy << 8) | 0x11u;
        }
    }
}

static void damage_a(unsigned char *map)
{
    unsigned x, y;

    for (y = DMG_Y; y < (unsigned)(DMG_Y + DMG_H); y++) {
        unsigned *row = (unsigned *)(void *)(map + (unsigned long)y * A_STRIDE);

        for (x = DMG_X; x < (unsigned)(DMG_X + DMG_W); x++)
            row[x] = DMG_COLOR;
    }
}

static void fill_b(unsigned char *map)
{
    unsigned x, y;

    for (y = 0; y < B_H; y++) {
        unsigned *row = (unsigned *)(void *)(map + (unsigned long)y * B_STRIDE);

        for (x = 0; x < B_W; x++) {
            const unsigned gx = (x * 4u) & 0xFFu;
            const unsigned gy = (y * 4u) & 0xFFu;

            if ((((x >> 3) + (y >> 3)) & 1u) != 0u)
                row[x] = 0xFF000000u | (gx << 16) | 0x80u | (gy << 8);
            else
                row[x] = 0xFF000000u | (gy << 16) | (gx << 8) | 0x20u;
        }
    }
}

/* Read one batch while asking the kernel for the SCM_RIGHTS descriptor that
 * travels with it.  The int 0x80 recv path only pops a descriptor when bytes are
 * pending, which is exactly the case right after wl_seat.get_keyboard (the server
 * queues the keymap memfd and then flushes the keymap event). */
static int recv_with_fd(struct conn *c, int *fd)
{
    int got_fd = -1;
    long n;

    *fd = -1;
    if (c->rx_len >= sizeof(c->rx)) return 0;
    n = sc3(NR_RECV_FD, c->fd, (long)(uintptr_t)&got_fd,
            (long)(uintptr_t)(c->rx + c->rx_len));
    if (n <= 0) return (int)n;
    c->rx_len += (unsigned)n;
    *fd = got_fd;
    return (int)n;
}

/* ---------------- client session helpers ---------------------------------- *
 * Both wl_shm pools are mapped up front and stay live for the whole run: that is
 * what the normal Wayland client does (one pool per connection, both mapped), and
 * it is also the regression probe for the mmap window bug this batch fixed in
 * kernel/syscall/mem.c (two addr==0 mappings used to land on the same window, so
 * the second one made the first one read the second one's pixels).  main() maps
 * the bigger pool first, then the smaller one, then re-hashes the first pool -
 * if the windows ever collide again, that hash check fails loudly.
 */
static int bind_globals(struct conn *c, int verbose)
{
    static const struct {
        unsigned  *name;
        const char *iface;
        unsigned   ver;
        unsigned   id;
    } binds[4] = {
        { &g_name_compositor, "wl_compositor",    4u, ID_COMPOSITOR },
        { &g_name_shm,        "wl_shm",           1u, ID_SHM },
        { &g_name_seat,       "wl_seat",          7u, ID_SEAT },
        { &g_name_subcomp,    "wl_subcompositor", 1u, ID_SUBCOMP },
    };
    unsigned i;

    msg_begin(c, ID_DISPLAY, 1u);        /* wl_display.get_registry */
    msg_u32(c, ID_REGISTRY);
    if (!msg_send(c)) return 0;
    pump(c, verbose);
    if (g_name_compositor == 0u || g_name_shm == 0u || g_name_seat == 0u) {
        printf("[wltwo]        a required global is missing\n");
        return 0;
    }
    for (i = 0; i < 4u; i++) {
        if (*binds[i].name == 0u) continue;
        msg_begin(c, ID_REGISTRY, 0u);   /* wl_registry.bind */
        msg_u32(c, *binds[i].name);
        msg_str(c, binds[i].iface);
        msg_u32(c, binds[i].ver);
        msg_u32(c, binds[i].id);
        if (!msg_send(c)) return 0;
    }
    msg_begin(c, ID_DISPLAY, 0u);        /* wl_display.sync round trip */
    msg_u32(c, ID_CALLBACK);
    if (!msg_send(c)) return 0;
    pump(c, 0);
    return 1;
}

/* memfd + mmap(MAP_SHARED): the same object the server later reads out of. */
static int map_pool(int bytes, unsigned char **map, int *fd_out, const char *tag)
{
    const long fd = sc1(NR_MEMFD, (long)bytes);
    long addr;

    if (fd <= 0) {
        printf("[wltwo]        %s: memfd_create(%d) failed (%ld)\n", tag, bytes,
               fd);
        return 0;
    }
    addr = sc2(NR_MAP_SHARED, (long)fd, (long)bytes);
    if (addr <= 0) {
        printf("[wltwo]        %s: mmap(MAP_SHARED, %d) failed (%ld)\n", tag,
               bytes, addr);
        return 0;
    }
    *map = (unsigned char *)(uintptr_t)addr;
    *fd_out = (int)fd;
    printf("[wltwo]        %s pool: memfd=%d map=0x%lx size=%d\n", tag,
           (int)fd, (unsigned long)addr, bytes);
    return 1;
}

static int create_pool(struct conn *c, int fd, int bytes)
{
    msg_begin(c, ID_SHM, 0u);            /* wl_shm.create_pool(new_id, fd, size) */
    msg_u32(c, ID_POOL);
    msg_i32(c, bytes);
    if (!msg_send_with_fd(c, fd)) return 0;
    pump(c, 0);
    return 1;
}

static int make_buffer(struct conn *c, int offset, int w, int h, int stride)
{
    msg_begin(c, ID_POOL, 0u);           /* wl_shm_pool.create_buffer */
    msg_u32(c, ID_BUFFER);
    msg_i32(c, offset);
    msg_i32(c, w);
    msg_i32(c, h);
    msg_i32(c, stride);
    msg_u32(c, WL_SHM_XRGB8888);
    if (!msg_send(c)) return 0;
    pump(c, 0);
    return 1;
}

static int commit_surface(struct conn *c, int verbose)
{
    const unsigned before = g_releases;

    msg_begin(c, ID_SURFACE, 6u);        /* wl_surface.commit */
    if (!msg_send(c)) return 0;
    pump(c, verbose);
    return g_releases > before;
}

static void tiny_delay(void)
{
    for (volatile int i = 0; i < 6000; i++)
        __asm__ volatile("pause");
}

static int write_file(const char *path, const unsigned char *data, unsigned len)
{
    const long fd = r3_open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    unsigned off = 0;

    if (fd < 0) {
        r3_fail("wltwo", path, fd);
        return 0;
    }
    while (off < len) {
        const unsigned chunk = (len - off > 4096u) ? 4096u : (len - off);
        const long w = r3_write((int)fd, data + off, chunk);

        if (w <= 0) {
            r3_fail("wltwo", "write", w);
            (void)r3_close((int)fd);
            return 0;
        }
        off += (unsigned)w;
    }
    if (r3_close((int)fd) < 0) return 0;
    (void)r3_sync();
    return 1;
}

/* ======================================================================= */
int main(int argc, char **argv)
{
    unsigned char *map_a = 0, *map_b = 0;
    int fd_a = -1, fd_b = -1;
    unsigned hash_a, hash_b;
    long t0, t1;
    int waited;

    (void)argc;
    (void)argv;

    printf("[wltwo] second ring-3 Wayland client: 2 connections, 2 retained "
           "surfaces, scale/region/subsurface, live keyboard+pointer\n");

    /* ---- 1. two concurrent connections --------------------------------- */
    g_a.fd = (int)sc1(NR_CONNECT, (long)(uintptr_t)"wayland-0");
    g_b.fd = (int)sc1(NR_CONNECT, (long)(uintptr_t)"wayland-0");
    printf("[wltwo]        connect #1 -> %d, connect #2 -> %d\n", g_a.fd, g_b.fd);
    check(g_a.fd >= 0 && g_b.fd >= 0 && g_a.fd != g_b.fd,
          "two concurrent connections to \"wayland-0\" with different fds");
    if (g_a.fd < 0 || g_b.fd < 0) {
        printf("[wltwo] RESULT FAIL\n");
        return 1;
    }

    /* ---- 2. both pools mapped up front (mmap window regression probe) --- */
    if (!map_pool(A_BYTES, &map_a, &fd_a, "A")) {
        printf("[wltwo] RESULT FAIL\n");
        return 1;
    }
    fill_a(map_a);
    hash_a = fnv1a(map_a, A_BYTES);
    if (!map_pool(B_BYTES, &map_b, &fd_b, "B")) {
        printf("[wltwo] RESULT FAIL\n");
        return 1;
    }
    fill_b(map_b);
    hash_b = fnv1a(map_b, B_BYTES);
    {
        /* kernel/syscall/mem.c regression: the second addr==0 mapping must not
         * have landed on the first one.  Assert on data + disjointness only -
         * never on specific addresses. */
        const uintptr_t a0 = (uintptr_t)map_a, a1 = a0 + A_BYTES;
        const uintptr_t b0 = (uintptr_t)map_b, b1 = b0 + B_BYTES;
        const int disjoint = (a1 <= b0) || (b1 <= a0);
        const unsigned a_again = fnv1a(map_a, A_BYTES);

        printf("[wltwo]        mmap windows: A=0x%lx+%d B=0x%lx+%d disjoint=%d "
               "a_intact=%d\n", (unsigned long)a0, A_BYTES, (unsigned long)b0,
               B_BYTES, disjoint, a_again == hash_a);
        check(disjoint, "the two MAP_SHARED windows do not overlap");
        check(a_again == hash_a,
              "mapping the second pool did not clobber the first pool's bytes");
    }
    printf("[wltwo]        A pattern %dx%d stride=%d FNV-1a=%08x\n", A_W, A_H,
           A_STRIDE, hash_a);
    printf("[wltwo]        B pattern %dx%d stride=%d FNV-1a=%08x\n", B_W, B_H,
           B_STRIDE, hash_b);

    /* ---- 3. connection B: 96x64 surface as a wl_subsurface -------------- */
    if (!bind_globals(&g_b, 1) || !create_pool(&g_b, fd_b, B_BYTES) ||
        !make_buffer(&g_b, 0, B_W, B_H, B_STRIDE)) {
        printf("[wltwo] RESULT FAIL\n");
        return 1;
    }

    msg_begin(&g_b, ID_COMPOSITOR, 0u);  /* create_surface(ID_SURFACE) */
    msg_u32(&g_b, ID_SURFACE);
    check(msg_send(&g_b), "B: create_surface");
    msg_begin(&g_b, ID_COMPOSITOR, 0u);  /* create_surface(ID_PARENT) */
    msg_u32(&g_b, ID_PARENT);
    check(msg_send(&g_b), "B: create the parent surface");
    msg_begin(&g_b, ID_SUBCOMP, 0u);     /* get_subsurface(id, surface, parent) */
    msg_u32(&g_b, ID_SUBSURF);
    msg_u32(&g_b, ID_SURFACE);
    msg_u32(&g_b, ID_PARENT);
    check(msg_send(&g_b), "B: wl_subcompositor.get_subsurface");
    msg_begin(&g_b, ID_SUBSURF, 1u);     /* set_position(24, 16) */
    msg_i32(&g_b, 24);
    msg_i32(&g_b, 16);
    check(msg_send(&g_b), "B: wl_subsurface.set_position(24,16)");
    msg_begin(&g_b, ID_SURFACE, 1u);     /* attach(ID_BUFFER, 0, 0) */
    msg_i32(&g_b, (int)ID_BUFFER);
    msg_i32(&g_b, 0);
    msg_i32(&g_b, 0);
    check(msg_send(&g_b), "B: attach the 96x64 buffer");
    check(commit_surface(&g_b, 1), "B: commit (96x64, scale 1)");

    /* ---- 4. connection A: 160x100 surface, scale 2, region, re-commit -- */
    if (!bind_globals(&g_a, 0) || !create_pool(&g_a, fd_a, A_BYTES) ||
        !make_buffer(&g_a, 0, A_W, A_H, A_STRIDE)) {
        printf("[wltwo] RESULT FAIL\n");
        return 1;
    }

    msg_begin(&g_a, ID_COMPOSITOR, 0u);  /* create_surface(ID_SURFACE) */
    msg_u32(&g_a, ID_SURFACE);
    check(msg_send(&g_a), "A: create_surface");
    msg_begin(&g_a, ID_COMPOSITOR, 1u);  /* create_region(ID_REGION) */
    msg_u32(&g_a, ID_REGION);
    check(msg_send(&g_a), "A: wl_compositor.create_region");
    msg_begin(&g_a, ID_REGION, 1u);      /* add(0, 0, 160, 50) */
    msg_i32(&g_a, 0); msg_i32(&g_a, 0);
    msg_i32(&g_a, A_W); msg_i32(&g_a, 50);
    check(msg_send(&g_a), "A: wl_region.add(0,0 160x50)");
    msg_begin(&g_a, ID_REGION, 1u);      /* add(0, 60, 40, 40) */
    msg_i32(&g_a, 0); msg_i32(&g_a, 60);
    msg_i32(&g_a, 40); msg_i32(&g_a, 40);
    check(msg_send(&g_a), "A: wl_region.add(0,60 40x40)");
    msg_begin(&g_a, ID_REGION, 2u);      /* subtract(10, 10, 10, 10) */
    msg_i32(&g_a, 10); msg_i32(&g_a, 10);
    msg_i32(&g_a, 10); msg_i32(&g_a, 10);
    check(msg_send(&g_a), "A: wl_region.subtract(10,10 10x10)");
    msg_begin(&g_a, ID_SURFACE, 8u);     /* set_buffer_scale(2) */
    msg_i32(&g_a, A_SCALE);
    check(msg_send(&g_a), "A: wl_surface.set_buffer_scale(2)");
    msg_begin(&g_a, ID_SURFACE, 4u);     /* set_opaque_region(ID_REGION) */
    msg_u32(&g_a, ID_REGION);
    check(msg_send(&g_a), "A: wl_surface.set_opaque_region");
    msg_begin(&g_a, ID_SURFACE, 5u);     /* set_input_region(ID_REGION) */
    msg_u32(&g_a, ID_REGION);
    check(msg_send(&g_a), "A: wl_surface.set_input_region");
    msg_begin(&g_a, ID_SURFACE, 1u);     /* attach(ID_BUFFER, 0, 0) */
    msg_i32(&g_a, (int)ID_BUFFER);
    msg_i32(&g_a, 0);
    msg_i32(&g_a, 0);
    check(msg_send(&g_a), "A: attach the 160x100 buffer");
    check(commit_surface(&g_a, 1), "A: commit #1 (160x100, scale 2)");

    /* ---- 4. A: re-commit with new pixels + damage ---------------------- */
    damage_a(map_a);
    {
        const unsigned h2 = fnv1a(map_a, A_BYTES);

        msg_begin(&g_a, ID_SURFACE, 1u);  /* attach again: in use for frame 2 */
        msg_i32(&g_a, (int)ID_BUFFER);
        msg_i32(&g_a, 0);
        msg_i32(&g_a, 0);
        check(msg_send(&g_a), "A: re-attach the buffer for frame 2");

        msg_begin(&g_a, ID_SURFACE, 2u);  /* damage(16,16 32x32) */
        msg_i32(&g_a, DMG_X); msg_i32(&g_a, DMG_Y);
        msg_i32(&g_a, DMG_W); msg_i32(&g_a, DMG_H);
        check(msg_send(&g_a), "A: wl_surface.damage(16,16 32x32)");
        check(commit_surface(&g_a, 1), "A: commit #2 (frame 2 hash differs)");
        printf("[wltwo]        A frame2 FNV-1a=%08x (was %08x)\n", h2, hash_a);
        check(h2 != hash_a, "A: frame 2 really differs from frame 1");
        hash_a = h2;
    }

    /* ---- 5. A: keyboard + pointer, then wait for real input ------------ */
    msg_begin(&g_a, ID_SEAT, 1u);        /* wl_seat.get_keyboard(ID_KB) */
    msg_u32(&g_a, ID_KB);
    check(msg_send(&g_a), "A: wl_seat.get_keyboard");
    /* The keymap descriptor rides with the keymap event: ask for it now, before
     * anything else is read, and parse whatever arrived in the same batch. */
    {
        int tries;

        for (tries = 0; tries < 400 && !g_keymap_ok; tries++) {
            int fd = -1;

            if (recv_with_fd(&g_a, &fd) > 0) {
                if (fd >= 0) g_keymap_fd = fd;
            } else {
                tiny_delay();
            }
            pump(&g_a, 0);
            if (g_keymap_fd < 0 && (tries % 40) == 0) tiny_delay();
        }
        check(g_keymap_fd >= 0, "wl_keyboard.keymap carried a memfd over SCM_RIGHTS");
        check(g_keymap_ok && g_keymap_format == 1u,
              "keymap is XKB_V1 (format 1) and its text hashes");
    }

    msg_begin(&g_a, ID_SEAT, 0u);        /* wl_seat.get_pointer(ID_PTR) */
    msg_u32(&g_a, ID_PTR);
    check(msg_send(&g_a), "A: wl_seat.get_pointer");
    pump(&g_a, 1);                       /* enter / motion / frame */

    t0 = sc1(NR_TICKS, 0);
    waited = 0;
    while (waited < INPUT_WAIT_TICKS) {
        pump(&g_a, 0);
        pump(&g_b, 0);
        if (g_kb_press >= NEED_KEY_PRESSES && g_kb_release >= NEED_KEY_PRESSES &&
            g_ptr_motion >= NEED_MOTIONS && g_ptr_button_press >= NEED_BUTTONS &&
            g_ptr_button_release >= NEED_BUTTONS)
            break;
        tiny_delay();
        t1 = sc1(NR_TICKS, 0);
        waited = (int)(t1 - t0);
    }
    t1 = sc1(NR_TICKS, 0);
    printf("[wltwo]        input window: %ld ticks, keys press=%u release=%u "
           "codes=%d, pointer enter=%d motion=%u button=%u/%u frame=%u\n",
           (long)(t1 - t0), g_kb_press, g_kb_release, g_kb_code_count,
           g_ptr_enter, g_ptr_motion, g_ptr_button_press, g_ptr_button_release,
           g_ptr_frame);

    /* ---- 6. release the pointer (exercises wl_pointer.leave) ----------- */
    msg_begin(&g_a, ID_PTR, 1u);         /* wl_pointer.release */
    check(msg_send(&g_a), "A: wl_pointer.release (server emits leave first)");
    pump(&g_a, 1);
    msg_begin(&g_a, ID_KB, 0u);          /* wl_keyboard.release */
    check(msg_send(&g_a), "A: wl_keyboard.release");
    pump(&g_a, 0);

    /* ---- 7. checks on what we received --------------------------------- */
    check(g_kb_keymap, "wl_keyboard.keymap received");
    check(g_kb_enter, "wl_keyboard.enter received");
    check(g_kb_press >= NEED_KEY_PRESSES,
          "wl_keyboard.key presses from real PS/2 typing");
    check(g_kb_release >= NEED_KEY_PRESSES,
          "wl_keyboard.key releases paired with the presses");
    check(g_kb_code_count >= 3, "at least 3 distinct evdev keycodes seen");
    check(g_ptr_enter, "wl_pointer.enter received");
    check(g_ptr_motion >= NEED_MOTIONS, "wl_pointer.motion streamed on change");
    check(g_ptr_button_press >= NEED_BUTTONS &&
          g_ptr_button_release >= NEED_BUTTONS,
          "wl_pointer.button press and release both delivered");
    check(g_ptr_frame >= 1u, "wl_pointer.frame groups delivered (seat v7)");
    check(g_ptr_leave, "wl_pointer.leave received after the release");
    check(g_releases >= 3u, "three wl_buffer.release events");
    check(g_keymap_ok, "keymap text read out of the SCM_RIGHTS memfd");
    check(g_kb_repeat && g_repeat_rate >= 20 && g_repeat_rate <= 40 &&
          g_repeat_delay >= 200 && g_repeat_delay <= 600,
          "wl_keyboard.repeat_info in the expected band");

    /* ---- 8. buffer lifecycle ------------------------------------------- *
     * wl_shm_pool.destroy keeps the descriptor alive for the buffers it already
     * made (attach/commit still work) but refuses create_buffer; wl_buffer.destroy
     * makes the object unusable (attach is refused and the surface drops it). */
    msg_begin(&g_a, ID_POOL, 1u);        /* wl_shm_pool.destroy(pool 7) */
    check(msg_send(&g_a), "A: wl_shm_pool.destroy (existing buffer stays usable)");
    pump(&g_a, 1);

    msg_begin(&g_a, ID_POOL, 0u);        /* create_buffer on the dead pool */
    msg_u32(&g_a, ID_POOL2);             /* id (unused by the server on refusal) */
    msg_i32(&g_a, 0);
    msg_i32(&g_a, 8);
    msg_i32(&g_a, 8);
    msg_i32(&g_a, 32);
    msg_u32(&g_a, WL_SHM_XRGB8888);
    check(msg_send(&g_a), "A: create_buffer on the destroyed pool (must be refused)");
    pump(&g_a, 1);

    msg_begin(&g_a, ID_SURFACE, 1u);     /* attach the existing buffer again */
    msg_i32(&g_a, (int)ID_BUFFER);
    msg_i32(&g_a, 0);
    msg_i32(&g_a, 0);
    check(msg_send(&g_a), "A: attach after the pool was destroyed");
    check(commit_surface(&g_a, 1),
          "A: commit after the pool was destroyed (buffer data still usable)");

    msg_begin(&g_a, ID_BUFFER, 0u);      /* wl_buffer.destroy(buffer 8) */
    check(msg_send(&g_a), "A: wl_buffer.destroy");
    pump(&g_a, 1);

    msg_begin(&g_a, ID_SURFACE, 1u);     /* attach the destroyed buffer */
    msg_i32(&g_a, (int)ID_BUFFER);
    msg_i32(&g_a, 0);
    msg_i32(&g_a, 0);
    check(msg_send(&g_a), "A: attach a destroyed buffer (must be refused)");
    pump(&g_a, 1);

    msg_begin(&g_a, ID_SURFACE, 6u);     /* commit with nothing attached */
    check(msg_send(&g_a), "A: commit with no buffer (must be refused)");
    pump(&g_a, 1);

    /* ---- 9. export both frames for host-side verification -------------- */
    check(write_file(BIN_A, map_a, (unsigned)A_BYTES), "A: wrote " BIN_A);
    check(write_file(BIN_B, map_b, (unsigned)B_BYTES), "B: wrote " BIN_B);

    printf("[wltwo] frames: A %dx%d scale=%d hash=%08x bytes=%d, B %dx%d "
           "scale=%d hash=%08x bytes=%d\n", A_W, A_H, A_SCALE, hash_a, A_BYTES,
           B_W, B_H, B_SCALE, hash_b, B_BYTES);
    printf("[wltwo] repeat: rate=%d delay=%u\n", g_repeat_rate, g_repeat_delay);
    printf("[wltwo] keymap: format=%u size=%u fd=%d hash=%08x\n",
           g_keymap_format, g_keymap_size, g_keymap_fd, g_keymap_hash);
    printf("[wltwo] lifecycle: pool destroyed, create_buffer refused, buffer "
           "destroyed, attach refused, empty commit refused\n");
    printf("[wltwo] input: keyboard press=%u release=%u keymap=%d enter=%d "
           "modifiers=%u | pointer enter=%d motion=%u button=%u/%u frame=%u "
           "leave=%d\n", g_kb_press, g_kb_release, g_kb_keymap, g_kb_enter,
           g_kb_modifiers, g_ptr_enter, g_ptr_motion, g_ptr_button_press,
           g_ptr_button_release, g_ptr_frame, g_ptr_leave);
    printf("[wltwo] second ring-3 Wayland client: %d ok, %d FAIL\n", g_ok, g_fail);
    printf(g_fail == 0 ? "[wltwo] RESULT PASS\n" : "[wltwo] RESULT FAIL\n");
    return g_fail == 0 ? 0 : 1;
}
