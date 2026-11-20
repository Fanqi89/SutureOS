/* SutureOS: AF_UNIX named endpoints. See wl_sock.h for the design notes.
 *
 * Provenance: written for SutureOS (GPL-3.0); no third-party code was copied.
 * Only the AF_UNIX naming concept of the Wayland protocol is used (the client
 * looks up "wayland-0" and gets a connected socket), implemented on top of the
 * socketpair layer this kernel already had.
 */
#include "wl_sock.h"

#include <stitch/console.h>
#include <stitch/string.h>

#include "unix_socket.h"
#include "wl_server.h"

#define EINVAL_     22
#define ENOENT_     2
#define EAGAIN_     11
#define EADDRINUSE_ 98

typedef struct {
    int  used;
    char name[WL_SOCK_NAME_MAX];
    int  fd;                 /* the named endpoint (server side of a pair) */
} wl_name_t;

typedef struct {
    int used;
    int server_fd;
    int client_fd;
    char name[WL_SOCK_NAME_MAX];
} wl_conn_t;

static wl_name_t g_names[WL_SOCK_MAX_NAMES];
static wl_conn_t g_conns[WL_SOCK_MAX_CONNS];
static int g_pumping;        /* re-entrancy guard (the pump never recurses)     */

/* Drop bindings whose endpoint was closed (the ring-3 ABI test binds a
 * temporary pair and closes it, and a finished client releases its end). */
static void wl_sock_purge_stale(void)
{
    int i;

    for (i = 0; i < WL_SOCK_MAX_NAMES; i++) {
        if (!g_names[i].used) continue;
        if (unix_is_fd(g_names[i].fd)) continue;
        console_printf("[wl] name \"%s\" released (endpoint %d closed)\n",
                       g_names[i].name, (int64_t)g_names[i].fd);
        memset(&g_names[i], 0, sizeof(g_names[i]));
    }
    for (i = 0; i < WL_SOCK_MAX_CONNS; i++) {
        if (!g_conns[i].used) continue;
        /* A connection stays usable for the protocol as long as its server end
         * is open; the client end may already be closed (client exited). */
        if (!unix_is_fd(g_conns[i].server_fd)) {
            memset(&g_conns[i], 0, sizeof(g_conns[i]));
        }
    }
}

int wl_named_bind(int fd, const char *name)
{
    int i;
    int free_slot = -1;

    if (name == NULL || name[0] == '\0' || strlen(name) >= WL_SOCK_NAME_MAX)
        return -EINVAL_;
    if (!unix_is_fd(fd))
        return -EINVAL_;

    wl_sock_purge_stale();

    for (i = 0; i < WL_SOCK_MAX_NAMES; i++) {
        if (g_names[i].used && strcmp(g_names[i].name, name) == 0)
            return -EADDRINUSE_;          /* a live endpoint owns the name */
        if (!g_names[i].used && free_slot < 0)
            free_slot = i;
    }
    if (free_slot < 0)
        return -EAGAIN_;

    g_names[free_slot].used = 1;
    strcpy(g_names[free_slot].name, name);
    g_names[free_slot].fd = fd;
    console_printf("[wl] named endpoint \"%s\" bound fd=%d\n",
                   g_names[free_slot].name, (int64_t)fd);
    return 0;
}

const char *wl_sock_name_at(int idx, int *fd)
{
    if (idx < 0 || idx >= WL_SOCK_MAX_NAMES || !g_names[idx].used)
        return NULL;
    if (fd) *fd = g_names[idx].fd;
    return g_names[idx].name;
}

int wl_sock_conn_count(void)
{
    int i, n = 0;

    for (i = 0; i < WL_SOCK_MAX_CONNS; i++)
        if (g_conns[i].used) n++;
    return n;
}

int wl_named_connect(const char *name)
{
    int pair[2];
    int slot;
    int bound = 0;
    int i;

    if (name == NULL || name[0] == '\0' || strlen(name) >= WL_SOCK_NAME_MAX)
        return -EINVAL_;

    pair[0] = pair[1] = -1;
    wl_sock_purge_stale();

    for (i = 0; i < WL_SOCK_MAX_NAMES; i++) {
        if (g_names[i].used && strcmp(g_names[i].name, name) == 0) {
            bound = 1;
            break;
        }
    }
    if (!bound && strcmp(name, WL_SERVER_NAME) != 0) {
        console_printf("[wl] connect \"%s\": no such name\n", name);
        return -ENOENT_;
    }
    for (slot = 0; slot < WL_SOCK_MAX_CONNS; slot++) {
        if (!g_conns[slot].used) break;
    }
    if (slot >= WL_SOCK_MAX_CONNS) {
        console_printf("[wl] connect \"%s\": connection table full\n", name);
        return -EAGAIN_;
    }
    if (unix_socketpair(pair) != 0)
        return -EAGAIN_;

    /* First connection to the compositor: start the server lazily and let this
     * pair's server end own the name (there is no listen backlog in this
     * model - one pair *is* the connection). */
    if (!bound) {
        if (wl_server_autostart() != 0) {
            (void)unix_close_fd(pair[0]);
            (void)unix_close_fd(pair[1]);
            return -EAGAIN_;
        }
        if (wl_named_bind(pair[1], name) != 0) {
            (void)unix_close_fd(pair[0]);
            (void)unix_close_fd(pair[1]);
            return -EADDRINUSE_;
        }
    }

    g_conns[slot].used = 1;
    g_conns[slot].server_fd = pair[1];
    g_conns[slot].client_fd = pair[0];
    strcpy(g_conns[slot].name, name);

    wl_server_accept(pair[1]);
    console_printf("[wl] client connected name=\"%s\" client_fd=%d "
                   "server_fd=%d\n", name, (int64_t)pair[0], (int64_t)pair[1]);
    return pair[0];
}

int wl_sock_is_client_fd(int fd)
{
    int i;

    for (i = 0; i < WL_SOCK_MAX_CONNS; i++) {
        if (g_conns[i].used && g_conns[i].client_fd == fd) return 1;
    }
    return 0;
}

int wl_sock_server_fd(int client_fd)
{
    int i;

    for (i = 0; i < WL_SOCK_MAX_CONNS; i++) {
        if (g_conns[i].used && g_conns[i].client_fd == client_fd)
            return g_conns[i].server_fd;
    }
    return -1;
}

void wl_sock_pump_fd(int fd)
{
    int i;

    if (g_pumping) return;               /* the pump is already running */
    for (i = 0; i < WL_SOCK_MAX_CONNS; i++) {
        if (g_conns[i].used && g_conns[i].client_fd == fd) {
            g_pumping = 1;
            wl_server_pump(g_conns[i].server_fd);
            g_pumping = 0;
            return;
        }
    }
}

void wl_sock_reset(void)
{
    memset(g_names, 0, sizeof(g_names));
    memset(g_conns, 0, sizeof(g_conns));
    g_pumping = 0;
}
