/* SutureOS ring-3 Wayland-prerequisite probe (v0.56.0).
 *
 * Exercises the four kernel capabilities that Wayland needs, FROM RING 3:
 *   socketpair(AF_UNIX) -> memfd(mmap MAP_SHARED) -> SCM_RIGHTS fd handover.
 *
 * The ported libc speaks the Vimtu64 int 0x80 ABI, so the probe uses five extra
 * ABI numbers added for this purpose (documented in kernel/syscall/int80.c as a
 * probe ABI, not POSIX):
 *   10 socketpair(out2)              11 memfd_create(size)
 *   12 map_shared(fd, len)           13 sock_send_fd(sockfd, fd, buf, len)
 *   14 sock_recv_fd(sockfd, out[] , buf, len)
 */
typedef unsigned long u64;

static long sc1(long nr, long a) { long r; __asm__ volatile("int $0x80" : "=a"(r) : "a"(nr), "D"(a)); return r; }
static long sc2(long nr, long a, long b) { long r; __asm__ volatile("int $0x80" : "=a"(r) : "a"(nr), "D"(a), "S"(b)); return r; }
static long sc3(long nr, long a, long b, long c) { long r; __asm__ volatile("int $0x80" : "=a"(r) : "a"(nr), "D"(a), "S"(b), "d"(c)); return r; }
static long sc4(long nr, long a, long b, long c, long d) { long r; __asm__ volatile("int $0x80" : "=a"(r) : "a"(nr), "D"(a), "S"(b), "d"(c), "r"(d)); return r; }

#define NR_WRITE 1
#define NR_EXIT  2
#define NR_SOCKETPAIR 10
#define NR_MEMFD      11
#define NR_MAP_SHARED 12
#define NR_SEND_FD    13
#define NR_RECV_FD    14

static void puts_(const char *s)
{
    long n = 0;
    while (s[n]) n++;
    (void)sc3(NR_WRITE, 1, (long)(u64)s, n);
}

static int g_ok, g_fail;

static void check(int cond, const char *what)
{
    if (cond) { g_ok++; puts_("[wlprobe]   ok   "); }
    else      { g_fail++; puts_("[wlprobe]   FAIL "); }
    puts_(what);
    puts_("\n");
}

int main(int argc, char **argv)
{
    int pair[2];
    int pool;
    char msg[8];
    char got[8];
    volatile unsigned char *map;
    int rx;
    long rc;

    (void)argc; (void)argv;
    puts_("[wlprobe] ring-3 Wayland prerequisite probe (socketpair + memfd + SCM_RIGHTS)\n");

    /* 1) socketpair in user space */
    rc = sc1(NR_SOCKETPAIR, (long)(u64)&pair[0]);
    check(rc == 0, "ring-3 socketpair(AF_UNIX) succeeds");
    check(pair[0] != pair[1], "the two fds differ");

    /* 2) memfd + MAP_SHARED in user space */
    rc = sc1(NR_MEMFD, 4096);
    pool = (int)rc;
    check(pool > 0, "ring-3 memfd_create(4096) returns an fd");
    rc = sc2(NR_MAP_SHARED, pool, 4096);
    check(rc > 0, "ring-3 mmap(MAP_SHARED) of the memfd succeeds");
    map = (volatile unsigned char *)(u64)rc;
    if (rc > 0) {
        msg[0] = 'w'; msg[1] = 'l'; msg[2] = '-'; msg[3] = 'r';
        msg[4] = '3'; msg[5] = 0;
        for (int i = 0; i < 6; i++) map[i] = (unsigned char)msg[i];
        check(map[0] == 'w' && map[5] == 0, "writes through the shared mapping stick");
    }

    /* 3) hand the pool over the socket with SCM_RIGHTS */
    rc = sc3(NR_SEND_FD, pair[0], ((long)5 << 16) | (long)pool, (long)(u64)"hello");
    check(rc == 5, "ring-3 sendmsg(fd + SCM_RIGHTS) transfers 5 bytes");

    rx = -1;
    rc = sc3(NR_RECV_FD, pair[1], (long)(u64)&rx, (long)(u64)got);
    check(rc == 5, "ring-3 recvmsg returns the 5 data bytes");
    check(got[0] == 'h' && got[4] == 'o', "the data payload is byte-exact");
    check(rx == pool, "recvmsg delivered the passed descriptor");
    check(rx > 0, "the delivered descriptor is usable");

    puts_("[wlprobe] ring-3 probe: ");
    if (g_fail == 0) puts_("PASS\n");
    else             puts_("FAIL\n");
    puts_(g_fail == 0 ? "[wlprobe] RESULT PASS\n" : "[wlprobe] RESULT FAIL\n");
    return g_fail == 0 ? 0 : 1;
}
