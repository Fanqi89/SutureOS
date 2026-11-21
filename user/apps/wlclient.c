/* wlclient.c - the first ring-3 Wayland client for SutureOS (v0.58.0).
 *
 * It talks to the in-kernel minimal Wayland server (kernel/ipc/wl_server.c) over
 * a *named* AF_UNIX endpoint ("wayland-0", kernel/ipc/wl_sock.c), which the
 * client reaches through two numbers this batch added to the Vimtu64 int 0x80
 * own ABI (kernel/syscall/int80.c): 15 bind(sockfd, name) and 16 connect(name).
 *
 * What it does, in Wayland's own order:
 *   1. a fresh socketpair + bind()/duplicate-bind() check (proves the name table
 *      is real: the second bind must be refused with -EADDRINUSE);
 *   2. connect("wayland-0") -> the client end of a connected pair;
 *   3. wl_display.get_registry + a wl_display.sync round trip;
 *   4. wl_registry.bind for wl_compositor / wl_shm / wl_seat;
 *   5. memfd_create(320*200*4) + mmap(MAP_SHARED) + a deterministic checkerboard
 *      pattern, so the compositor side can prove the bytes it reads are ours;
 *   6. wl_shm.create_pool(fd over SCM_RIGHTS) + wl_shm_pool.create_buffer +
 *      wl_compositor.create_surface + wl_surface.attach + wl_surface.commit
 *      (frame #1);
 *   7. wait for wl_buffer.release;
 *   8. write the same pixels to /mnt/fat/WL.BIN through the VFS so the host can
 *      recompute the FNV-1a hash independently and compare it with the hash the
 *      server logged at the first commit;
 *   9. wl_surface.frame() (v0.60): request a wl_callback, which the server
 *      completes on the next commit (wl_callback.done(serial));
 *  10. commit #2: change one 32x32 block of the shared buffer, pass a
 *      wl_surface.damage() rect and commit again; the server logs the damage and
 *      a second commit hash, then releases the buffer and fires the callback;
 *  11. wl_callback.destroy -> wl_display.delete_id, proving id recycling;
 *  12. wl_seat.get_pointer -> a real wl_pointer that receives enter/motion (and
 *      button when held) from the kernel's PS/2 mouse snapshot;
 *  13. a request for an object id that was never created -> wl_display.error,
 *      so the error path is proven instead of assumed.
 *
 * License: GPL-3.0, part of SutureOS. Written from the Wayland protocol
 * specification; no libwayland code is used (there is no libwayland here).
 */
#include <stdio.h>
#include <string.h>

#include "r3tool.h"

/* ---------------- Vimtu64 int 0x80 own ABI (kernel/syscall/int80.c) -------- */
#define NR_WRITE       1
#define NR_EXIT        2
#define NR_CLOSE       8
#define NR_READ        7
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

/* ---------------- surface geometry --------------------------------------- */
#define SURF_W      320
#define SURF_H      200
#define SURF_STRIDE (SURF_W * 4)
#define SURF_BYTES  (SURF_STRIDE * SURF_H)      /* 256000 bytes = 63 pages  */
#define SURF_WL_SHM_FORMAT_XRGB8888 1u

/* Object ids this client picks (Wayland lets the client allocate them). */
#define ID_DISPLAY    1u
#define ID_REGISTRY   2u
#define ID_COMPOSITOR 3u
#define ID_SHM        4u
#define ID_SEAT       5u
#define ID_CALLBACK   6u
#define ID_POOL       7u
#define ID_BUFFER     8u
#define ID_SURFACE    9u
#define ID_FRAME_CB   10u    /* wl_callback from wl_surface.frame()          */
#define ID_POINTER    11u    /* wl_pointer from wl_seat.get_pointer()        */
#define ID_BOGUS      99u    /* never created: drives the wl_display.error test */

/* Frame 2 changes this block so the damage path has real new pixels. */
#define DAMAGE_X      32
#define DAMAGE_Y      32
#define DAMAGE_W      32
#define DAMAGE_H      32
#define DAMAGE_COLOR  0xFF112233u

#define TEXT_BIN "/mnt/fat/WL.BIN"

static int g_ok, g_fail;
static int g_sock = -1;

static void check(int cond, const char *what)
{
    if (cond) { g_ok++; printf("[wlclient]   ok   %s\n", what); }
    else      { g_fail++; printf("[wlclient]   FAIL %s\n", what); }
}

/* ---------------- wire helpers (little endian) ---------------- */
static unsigned char g_tx[512];
static unsigned g_tx_len;
static unsigned char g_rx[8192];
static unsigned g_rx_len;

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

static void msg_begin(unsigned obj, unsigned op)
{
    put32(g_tx, obj);
    g_tx[4] = (unsigned char)(op & 0xFFu);
    g_tx[5] = 0;
    g_tx[6] = 0;                     /* size patched by msg_send() */
    g_tx[7] = 0;
    g_tx_len = 8;
}

static void msg_u32(unsigned v)
{
    if (g_tx_len + 4u > sizeof(g_tx)) return;
    put32(g_tx + g_tx_len, v);
    g_tx_len += 4;
}

static void msg_i32(int v)
{
    msg_u32((unsigned)v);
}

/* Wayland string: u32 length including the NUL, then the bytes, pad to 4. */
static void msg_str(const char *s)
{
    const unsigned n = (unsigned)strlen(s) + 1u;
    unsigned i;

    msg_u32(n);
    if (g_tx_len + n > sizeof(g_tx)) return;
    memcpy(g_tx + g_tx_len, s, n);
    g_tx_len += n;
    for (i = 0; (g_tx_len & 3u) != 0u && g_tx_len + 1u <= sizeof(g_tx); i++) {
        g_tx[g_tx_len++] = 0;
    }
}

/* Write the accumulated request; returns 1 when every byte was written. */
static int msg_send(void)
{
    unsigned off = 0;

    g_tx[6] = (unsigned char)(g_tx_len & 0xFFu);
    g_tx[7] = (unsigned char)((g_tx_len >> 8) & 0xFFu);
    while (off < g_tx_len) {
        const long n = sc3(NR_WRITE, g_sock, (long)(uintptr_t)(g_tx + off),
                           (long)(g_tx_len - off));
        if (n <= 0) return 0;
        off += (unsigned)n;
    }
    return 1;
}

/* Same, but the pool descriptor rides along with SCM_RIGHTS (the int 0x80 send
 * number packs the payload length into the upper half of its second argument,
 * because the interrupt frame does not carry a fourth argument). */
static int msg_send_with_fd(int fd)
{
    long rc;

    g_tx[6] = (unsigned char)(g_tx_len & 0xFFu);
    g_tx[7] = (unsigned char)((g_tx_len >> 8) & 0xFFu);
    if (g_tx_len == 0 || g_tx_len > 0xFFFFu) return 0;
    rc = sc3(NR_SEND_FD, g_sock, (long)((g_tx_len << 16) | (unsigned)fd),
             (long)(uintptr_t)g_tx);
    return rc == (long)g_tx_len;
}

/* Pull one complete message out of the socket. Returns its size, 0 when no
 * message is pending.
 *
 * The server is driven by this program's own syscalls (write() dispatches the
 * request and queues the reply), so a reply is normally already sitting in the
 * socket ring when we get here. A bounded number of retries covers the cases
 * where it is not - a fixed spin budget, never an unbounded wait: a client that
 * hangs looking for an event would look exactly like a dead compositor. */
static int msg_recv(unsigned *obj, unsigned *op)
{
    int tries;

    for (tries = 0; tries < 8; tries++) {
        if (g_rx_len >= 8u) {
            const unsigned size = (unsigned)g_rx[6] |
                                  ((unsigned)g_rx[7] << 8);
            if (size >= 8u && size <= g_rx_len) {
                *obj = get32(g_rx);
                *op = (unsigned)g_rx[4] | ((unsigned)g_rx[5] << 8);
                return (int)size;
            }
            if (size < 8u) return -1;              /* protocol desync */
        }
        if (g_rx_len < sizeof(g_rx)) {
            const long n = sc3(NR_READ, g_sock,
                               (long)(uintptr_t)(g_rx + g_rx_len),
                               (long)(sizeof(g_rx) - g_rx_len));
            if (n > 0) {
                g_rx_len += (unsigned)n;
                continue;
            }
        }
        {
            for (volatile int pad = 0; pad < 2000; pad++) {
                __asm__ volatile("pause");
            }
        }
    }
    return 0;
}

static void msg_consume(unsigned size)
{
    if (size >= g_rx_len) {
        g_rx_len = 0;
        return;
    }
    memmove(g_rx, g_rx + size, g_rx_len - size);
    g_rx_len -= size;
}

/* Read a string argument out of a message body; advances *off. Returns 1 on
 * success and NUL-terminates `dst`. */
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

/* ---------------- FNV-1a over the pixels ---------------- */
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

/* ---------------- protocol helpers ---------------- */
static unsigned g_globals_seen;
static unsigned g_global_name_compositor, g_global_name_shm, g_global_name_seat;
static int g_callback_done, g_buffer_released;
static unsigned g_seat_caps;
/* v0.60: frame callbacks / damage / display error / pointer */
static int g_frame_done, g_pointer_enter, g_pointer_motion;
static unsigned g_frame_serial;
static unsigned g_releases;
static int g_pointer_enter_x, g_pointer_enter_y;
static int g_pointer_motion_x, g_pointer_motion_y;
static int g_display_error_seen;
static unsigned g_display_error_object, g_display_error_code;
static unsigned g_delete_ids;

/* Handle every pending event; returns when `until_callback_done` is satisfied or
 * nothing is pending any more (bounded). */
static void pump_events(int until_callback_done)
{
    int guard;

    for (guard = 0; guard < 64; guard++) {
        unsigned obj = 0, op = 0;
        int size = msg_recv(&obj, &op);

        if (size <= 0) break;
        if (op == 0u && obj == ID_REGISTRY) {
            /* wl_registry.global(name u32, interface string, version u32) */
            unsigned off = 8;
            char iface[64];

            if (off + 4u <= (unsigned)size) {
                const unsigned name = get32(g_rx + off);
                off += 4;
                if (arg_str(g_rx, (unsigned)size, &off, iface, sizeof(iface))) {
                    g_globals_seen++;
                    printf("[wlclient]   global name=%u interface=\"%s\"\n",
                           name, iface);
                    if (strcmp(iface, "wl_compositor") == 0) {
                        g_global_name_compositor = name;
                    } else if (strcmp(iface, "wl_shm") == 0) {
                        g_global_name_shm = name;
                    } else if (strcmp(iface, "wl_seat") == 0) {
                        g_global_name_seat = name;
                    }
                }
            }
        } else if (op == 0u && obj == ID_CALLBACK) {
            g_callback_done = 1;
            printf("[wlclient]   event wl_callback(%u).done serial=%u\n", obj,
                   (unsigned)get32(g_rx + 8));
        } else if (op == 0u && obj == ID_FRAME_CB) {
            /* wl_callback.done(serial) for the wl_surface.frame() request */
            g_frame_done = 1;
            g_frame_serial = get32(g_rx + 8);
            printf("[wlclient] frame done serial=%u\n", g_frame_serial);
        } else if (op == 0u && obj == ID_BUFFER) {
            g_buffer_released = 1;
            g_releases++;
            printf("[wlclient]   event wl_buffer(%u).release (n=%u)\n", obj,
                   g_releases);
        } else if (op == 0u && obj == ID_SEAT) {
            g_seat_caps = get32(g_rx + 8);
            printf("[wlclient]   event wl_seat(%u).capabilities=0x%x\n", obj,
                   g_seat_caps);
        } else if (op == 0u && obj == ID_POINTER) {
            /* wl_pointer.enter(serial u32, surface u32, x fixed, y fixed) */
            if ((unsigned)size >= 24u) {
                g_pointer_enter = 1;
                g_pointer_enter_x = (int)(get32(g_rx + 16) >> 8);
                g_pointer_enter_y = (int)(get32(g_rx + 20) >> 8);
                printf("[wlclient] pointer enter %d,%d\n", g_pointer_enter_x,
                       g_pointer_enter_y);
            }
        } else if (op == 2u && obj == ID_POINTER) {
            if ((unsigned)size >= 20u) {
                g_pointer_motion = 1;
                g_pointer_motion_x = (int)(get32(g_rx + 12) >> 8);
                g_pointer_motion_y = (int)(get32(g_rx + 16) >> 8);
                printf("[wlclient] pointer motion %d,%d\n", g_pointer_motion_x,
                       g_pointer_motion_y);
            }
        } else if (op == 3u && obj == ID_POINTER) {
            printf("[wlclient]   event wl_pointer.button=%u state=%u\n",
                   (unsigned)get32(g_rx + 16), (unsigned)get32(g_rx + 20));
        } else if (op == 0u && obj == ID_DISPLAY) {
            /* wl_display.error(object_id, code, message) */
            char msg[96];
            unsigned off = 16;

            g_display_error_seen = 1;
            g_display_error_object = get32(g_rx + 8);
            g_display_error_code = get32(g_rx + 12);
            msg[0] = '\0';
            (void)arg_str(g_rx, (unsigned)size, &off, msg, sizeof(msg));
            printf("[wlclient] display error object=%u code=%u message=\"%s\"\n",
                   g_display_error_object, g_display_error_code, msg);
        } else if (op == 1u && obj == ID_DISPLAY) {
            /* wl_display.delete_id(id) */
            g_delete_ids++;
            printf("[wlclient]   event wl_display.delete_id id=%u\n",
                   (unsigned)get32(g_rx + 8));
        } else {
            printf("[wlclient]   event object=%u opcode=%u size=%d (ignored)\n",
                   obj, op, size);
        }
        msg_consume((unsigned)size);
        if (until_callback_done && g_callback_done) return;
    }
}

/* ======================================================================= */
int main(int argc, char **argv)
{
    int pair[2];
    long rc;
    int pool_fd;
    unsigned char *map;
    unsigned x, y;
    unsigned hash;
    long out;
    unsigned written = 0;

    (void)argc;
    (void)argv;

    printf("[wlclient] ring-3 Wayland client (named AF_UNIX + wl_shm shared "
           "buffer)\n");

    /* ---- 1. the named-endpoint ABI itself ------------------------------- */
    pair[0] = pair[1] = -1;
    rc = sc1(NR_SOCKETPAIR, (long)(uintptr_t)&pair[0]);
    check(rc == 0, "socketpair() for the bind() check");
    if (rc == 0) {
        const long b1 = sc2(NR_BIND, (long)pair[0], (long)(uintptr_t)"wlclient-test");
        const long b2 = sc2(NR_BIND, (long)pair[0], (long)(uintptr_t)"wlclient-test");

        printf("[wlclient]        bind #1 -> %ld, duplicate bind -> %ld "
               "(expect 0 then -98 EADDRINUSE)\n", b1, b2);
        check(b1 == 0, "bind(fd, \"wlclient-test\") succeeds");
        check(b2 == -98, "a duplicate bind of the same name is refused (EADDRINUSE)");
        (void)sc1(NR_CLOSE, (long)pair[0]);
        (void)sc1(NR_CLOSE, (long)pair[1]);
    }

    /* ---- 2. connect to the compositor ----------------------------------- */
    rc = sc1(NR_CONNECT, (long)(uintptr_t)"wayland-0");
    g_sock = (int)rc;
    printf("[wlclient]        connect(\"wayland-0\") -> %ld\n", rc);
    check(g_sock >= 0, "connect(\"wayland-0\") returns a client socket fd");
    if (g_sock < 0) {
        printf("[wlclient] RESULT FAIL\n");
        return 1;
    }
    {
        const long bad = sc1(NR_CONNECT, (long)(uintptr_t)"wayland-9");
        printf("[wlclient]        connect(\"wayland-9\") -> %ld (expect -2 "
               "ENOENT)\n", bad);
        check(bad == -2, "connect() to an unbound name fails with -ENOENT");
    }

    /* ---- 3. wl_display.get_registry + roundtrip -------------------------- */
    msg_begin(ID_DISPLAY, 1u);           /* wl_display.get_registry */
    msg_u32(ID_REGISTRY);
    check(msg_send(), "send wl_display.get_registry");
    pump_events(0);
    check(g_globals_seen >= 3u, "registry advertised >= 3 globals");
    check(g_global_name_compositor != 0u && g_global_name_shm != 0u,
          "the compositor and shm globals are present");

    /* ---- 4. bind the interfaces the client needs ------------------------ */
    if (g_global_name_compositor != 0u) {
        msg_begin(ID_REGISTRY, 0u);      /* wl_registry.bind */
        msg_u32(g_global_name_compositor);
        msg_str("wl_compositor");
        msg_u32(1u);
        msg_u32(ID_COMPOSITOR);
        check(msg_send(), "bind wl_compositor");
    }
    if (g_global_name_shm != 0u) {
        msg_begin(ID_REGISTRY, 0u);
        msg_u32(g_global_name_shm);
        msg_str("wl_shm");
        msg_u32(1u);
        msg_u32(ID_SHM);
        check(msg_send(), "bind wl_shm");
    }
    if (g_global_name_seat != 0u) {
        msg_begin(ID_REGISTRY, 0u);
        msg_u32(g_global_name_seat);
        msg_str("wl_seat");
        msg_u32(1u);
        msg_u32(ID_SEAT);
        check(msg_send(), "bind wl_seat");
    }
    check(g_global_name_compositor != 0u && g_global_name_shm != 0u &&
          g_global_name_seat != 0u,
          "bound wl_compositor / wl_shm / wl_seat");

    /* wl_display.sync gives us a real round trip: the callback.done event proves
     * every request above reached the server and every event was queued. */
    g_callback_done = 0;
    msg_begin(ID_DISPLAY, 0u);           /* wl_display.sync */
    msg_u32(ID_CALLBACK);
    check(msg_send(), "send wl_display.sync");
    pump_events(1);
    check(g_callback_done, "sync round trip completed (wl_callback.done)");

    /* ---- 5. shared pixel pool ------------------------------------------- */
    rc = sc1(NR_MEMFD, (long)SURF_BYTES);
    pool_fd = (int)rc;
    printf("[wlclient]        memfd_create(%d) -> %d\n", SURF_BYTES, pool_fd);
    check(pool_fd > 0, "memfd_create() for the wl_shm pool");
    rc = sc2(NR_MAP_SHARED, (long)pool_fd, (long)SURF_BYTES);
    printf("[wlclient]        mmap(MAP_SHARED, %d) -> 0x%lx\n", SURF_BYTES,
           (unsigned long)rc);
    check(rc > 0, "map the pool MAP_SHARED");
    if (pool_fd <= 0 || rc <= 0) {
        printf("[wlclient] RESULT FAIL\n");
        return 1;
    }
    map = (unsigned char *)(uintptr_t)rc;

    /* Deterministic pattern: 32x32 checkerboard blocks with per-axis gradients.
     * The server reads exactly these bytes out of the memfd and logs their
     * FNV-1a hash, and the host recomputes the same hash from WL.BIN. */
    for (y = 0; y < SURF_H; y++) {
        unsigned *row = (unsigned *)(void *)(map + (unsigned long)y * SURF_STRIDE);

        for (x = 0; x < SURF_W; x++) {
            const unsigned gx = (x * 2u) & 0xFFu;
            const unsigned gy = (y * 2u) & 0xFFu;
            unsigned c;

            if ((((x >> 5) + (y >> 5)) & 1u) != 0u) {
                c = 0xFF000000u | (gx << 16) | (gy << 8) | 0x40u;
            } else {
                c = 0xFF000000u | (gy << 16) | 0x20u | (gx << 8);
            }
            row[x] = c;
        }
    }
    hash = fnv1a(map, SURF_BYTES);
    printf("[wlclient]        pattern filled: %dx%d stride=%d, FNV-1a of the "
           "mapped bytes = %08x\n", SURF_W, SURF_H, SURF_STRIDE, hash);

    /* ---- 6. pool / buffer / surface / attach / commit -------------------- */
    msg_begin(ID_SHM, 0u);               /* wl_shm.create_pool(new_id, fd, size) */
    msg_u32(ID_POOL);
    msg_i32(SURF_BYTES);
    check(msg_send_with_fd(pool_fd), "create_pool with the memfd over SCM_RIGHTS");

    msg_begin(ID_POOL, 0u);              /* wl_shm_pool.create_buffer */
    msg_u32(ID_BUFFER);
    msg_i32(0);                          /* offset  */
    msg_i32(SURF_W);
    msg_i32(SURF_H);
    msg_i32(SURF_STRIDE);
    msg_u32(SURF_WL_SHM_FORMAT_XRGB8888);
    check(msg_send(), "create_buffer (XRGB8888)");

    msg_begin(ID_COMPOSITOR, 0u);        /* wl_compositor.create_surface */
    msg_u32(ID_SURFACE);
    check(msg_send(), "create_surface");

    msg_begin(ID_SURFACE, 1u);           /* wl_surface.attach(buffer, 0, 0) */
    msg_i32((int)ID_BUFFER);
    msg_i32(0);
    msg_i32(0);
    check(msg_send(), "attach the buffer to the surface");

    /* ---- 7. commit #1: publish the shared-memory frame ------------------- */
    g_buffer_released = 0;
    g_releases = 0;
    msg_begin(ID_SURFACE, 6u);           /* wl_surface.commit */
    check(msg_send(), "commit the surface (frame 1)");
    pump_events(0);
    check(g_buffer_released, "wl_buffer.release received after commit #1");

    /* ---- 8. the same pixels through the VFS, for the host-side check -----
     * This copy is frame #1 (the canonical checkerboard) on purpose: the host
     * verifier compares WL.BIN byte-for-byte against that pattern, and writing
     * 250 KiB costs ~13 s under TCG, so only one frame is exported. Frame #2's
     * bytes are verified on the host by recomputing the modified pattern and
     * comparing it with the second `[wl] commit ... hash=` line. */
    printf("[wlclient]        writing %d bytes to %s\n", SURF_BYTES, TEXT_BIN);
    out = r3_open(TEXT_BIN, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) {
        r3_fail("wlclient", TEXT_BIN, out);
        check(0, "open " TEXT_BIN);
    } else {
        while (written < (unsigned)SURF_BYTES) {
            const unsigned chunk = (unsigned)SURF_BYTES - written > 4096u
                                       ? 4096u
                                       : (unsigned)SURF_BYTES - written;
            const long w = r3_write((int)out, map + written, chunk);

            if (w <= 0) {
                r3_fail("wlclient", "write " TEXT_BIN, w);
                break;
            }
            written += (unsigned)w;
        }
        if (r3_close((int)out) < 0) {
            r3_fail("wlclient", "close " TEXT_BIN, -1);
        }
        (void)r3_sync();                 /* flush the FAT32 free-cluster count */
        printf("[wlclient]        wrote %u byte(s)\n", written);
        check(written == (unsigned)SURF_BYTES,
              "the pixel bytes reached " TEXT_BIN " through the VFS");
    }

    /* ---- 9. wl_surface.frame(): completed by the *next* commit ---------- */
    g_frame_done = 0;
    msg_begin(ID_SURFACE, 3u);           /* wl_surface.frame(new_id) */
    msg_u32(ID_FRAME_CB);
    check(msg_send(), "request wl_surface.frame (wl_callback 10)");

    /* ---- 10. commit #2: change one 32x32 block + a damage rect ---------- */
    for (y = DAMAGE_Y; y < (unsigned)(DAMAGE_Y + DAMAGE_H); y++) {
        unsigned *row = (unsigned *)(void *)(map + (unsigned long)y * SURF_STRIDE);

        for (x = DAMAGE_X; x < (unsigned)(DAMAGE_X + DAMAGE_W); x++)
            row[x] = DAMAGE_COLOR;
    }
    /* Re-attach before the second commit: a release was received for the first
     * one, so the buffer has to be handed back in before it can be committed
     * again (one release per attach cycle). */
    msg_begin(ID_SURFACE, 1u);           /* wl_surface.attach(buffer, 0, 0) */
    msg_i32((int)ID_BUFFER);
    msg_i32(0);
    msg_i32(0);
    check(msg_send(), "re-attach the buffer for frame 2");

    msg_begin(ID_SURFACE, 2u);           /* wl_surface.damage(x, y, w, h) */
    msg_i32(DAMAGE_X);
    msg_i32(DAMAGE_Y);
    msg_i32(DAMAGE_W);
    msg_i32(DAMAGE_H);
    check(msg_send(), "wl_surface.damage(32,32 32x32)");
    msg_begin(ID_SURFACE, 6u);           /* wl_surface.commit */
    check(msg_send(), "commit the surface (frame 2)");
    pump_events(0);
    check(g_frame_done, "wl_callback.done arrived for the frame request");
    check(g_releases >= 2u, "second wl_buffer.release (commit #2 processed)");
    if (g_releases >= 2u)
        printf("[wlclient] second commit ok\n");

    /* ---- 11. destroy the callback: the server answers with delete_id ---- */
    msg_begin(ID_FRAME_CB, 0u);          /* wl_callback.destroy */
    check(msg_send(), "destroy the frame callback (wl_callback.destroy)");
    pump_events(0);
    check(g_delete_ids >= 1u, "wl_display.delete_id after the destroy");

    /* ---- 12. wl_seat.get_pointer -> a real wl_pointer + pointer events -- */
    g_pointer_enter = 0;
    g_pointer_motion = 0;
    msg_begin(ID_SEAT, 0u);              /* wl_seat.get_pointer(new_id) */
    msg_u32(ID_POINTER);
    check(msg_send(), "wl_seat.get_pointer (wl_pointer 11)");
    pump_events(0);
    check(g_pointer_enter, "wl_pointer.enter received");
    check(g_pointer_motion, "wl_pointer.motion received");

    msg_begin(ID_POINTER, 1u);           /* wl_pointer.release */
    check(msg_send(), "wl_pointer.release");
    pump_events(0);

    /* ---- 13. wl_display.error on a request for an unknown object -------- */
    g_display_error_seen = 0;
    msg_begin(ID_BOGUS, 0u);
    msg_u32(0u);
    check(msg_send(), "send a request for an unknown object id (99)");
    pump_events(0);
    check(g_display_error_seen, "wl_display.error reported the unknown object");
    check(g_display_error_seen && g_display_error_object == ID_BOGUS,
          "the error names object 99 (code 0 = invalid object)");

    printf("[wlclient] pixels hash=%08x bytes=%d path=" TEXT_BIN "\n", hash,
           (int)SURF_BYTES);
    printf("[wlclient] protocol summary: releases=%u frame_serial=%u "
           "delete_ids=%u pointer_enter=%d,%d pointer_motion=%d,%d "
           "display_error=%u/%u\n", g_releases, g_frame_serial, g_delete_ids,
           g_pointer_enter_x, g_pointer_enter_y, g_pointer_motion_x,
           g_pointer_motion_y, g_display_error_object, g_display_error_code);
    printf("[wlclient] ring-3 Wayland client: %d ok, %d FAIL\n", g_ok, g_fail);
    printf(g_fail == 0 ? "[wlclient] RESULT PASS\n" : "[wlclient] RESULT FAIL\n");
    return g_fail == 0 ? 0 : 1;
}
