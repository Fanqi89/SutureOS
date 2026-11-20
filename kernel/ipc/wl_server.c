/* SutureOS: minimal Wayland server. See wl_server.h for scope and limits.
 *
 * Provenance: written for SutureOS (GPL-3.0) from the Wayland protocol
 * specification (wayland.xml: wl_display, wl_registry, wl_callback,
 * wl_compositor, wl_surface, wl_shm, wl_shm_pool, wl_buffer, wl_seat) and the
 * published wire format (u32 object id, u16 opcode, u16 size, little endian,
 * strings as u32 length including NUL + 4-byte padding). No third-party server
 * code was copied: libwayland/Smithay are not part of this tree.
 */
#include "wl_server.h"

#include <stitch/console.h>
#include <stitch/string.h>

#include "memfd.h"
#include "scm_fd.h"
#include "unix_socket.h"
#include "wl_sock.h"

/* ===================== wire constants ===================== */
#define WL_HDR_SIZE     8u
#define WL_DISPLAY_ID   1u
#define WL_FMT_XRGB8888 1u

/* object types tracked per connection */
enum {
    OBJ_NONE = 0,
    OBJ_DISPLAY,
    OBJ_REGISTRY,
    OBJ_CALLBACK,
    OBJ_COMPOSITOR,
    OBJ_SURFACE,
    OBJ_SHM,
    OBJ_POOL,
    OBJ_BUFFER,
    OBJ_SEAT,
    OBJ_POINTER,
    OBJ_KEYBOARD,
    OBJ_REGION
};

struct wl_global_def {
    const char *iface;
    uint32_t    version;
    uint32_t    name;          /* the number wl_registry.bind() takes */
    int         type;
};

static const struct wl_global_def g_globals[] = {
    { "wl_compositor", 1u, 1u, OBJ_COMPOSITOR },
    { "wl_shm",        1u, 2u, OBJ_SHM },
    { "wl_seat",       1u, 3u, OBJ_SEAT },
};
#define WL_GLOBAL_COUNT ((int)(sizeof g_globals / sizeof g_globals[0]))

struct wl_obj {
    uint32_t id;
    int      type;
    /* wl_shm_pool */
    int      pool_fd;          /* memfd the client handed over, -1 when none */
    int32_t  pool_size;
    /* wl_buffer */
    int32_t  pool_id;
    int32_t  offset;
    int32_t  w, h, stride, format;
    /* wl_surface */
    int32_t  buffer_id;
    int32_t  attach_x, attach_y;
};

struct wl_conn {
    int      used;
    int      server_fd;
    struct wl_obj objs[WL_SERVER_MAX_OBJECTS];
    uint8_t  in[WL_SERVER_IN_MAX];
    uint32_t in_len;
    uint8_t  out[WL_SERVER_OUT_MAX];
    uint32_t out_len;
};

static struct wl_conn g_conns[WL_SOCK_MAX_CONNS];

/* last committed surface: kept after the client exits (the compositor blits it) */
static uint8_t  g_last_px[WL_SURFACE_MAX_BYTES];
static int      g_last_w, g_last_h, g_last_stride;
static int      g_last_valid;
static uint32_t g_last_hash;
static uint32_t g_commits;
static int      g_started;

/* ===================== little-endian helpers ===================== */
static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint16_t rd_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static void wr_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}

/* ===================== outgoing event buffer ===================== */
struct wbuf {
    uint8_t *p;
    uint32_t len;
    uint32_t cap;
    int      overflow;
};

static void wb_u32(struct wbuf *b, uint32_t v)
{
    if (b->len + 4u > b->cap) { b->overflow = 1; return; }
    b->p[b->len++] = (uint8_t)(v & 0xFFu);
    b->p[b->len++] = (uint8_t)((v >> 8) & 0xFFu);
    b->p[b->len++] = (uint8_t)((v >> 16) & 0xFFu);
    b->p[b->len++] = (uint8_t)((v >> 24) & 0xFFu);
}

static void wb_u16(struct wbuf *b, uint16_t v)
{
    if (b->len + 2u > b->cap) { b->overflow = 1; return; }
    b->p[b->len++] = (uint8_t)(v & 0xFFu);
    b->p[b->len++] = (uint8_t)(v >> 8);
}

static void wb_bytes(struct wbuf *b, const void *src, uint32_t n)
{
    if (b->len + n > b->cap) { b->overflow = 1; return; }
    memcpy(b->p + b->len, src, n);
    b->len += n;
}

/* Wayland string: u32 length including the NUL terminator, bytes, pad to 4. */
static void wb_str(struct wbuf *b, const char *s)
{
    uint32_t n = (uint32_t)strlen(s) + 1u;

    wb_u32(b, n);
    wb_bytes(b, s, n);
    while (b->len & 3u) {
        if (b->len + 1u > b->cap) { b->overflow = 1; return; }
        b->p[b->len++] = 0;
    }
}

/* ===================== connection book-keeping ===================== */
static struct wl_conn *conn_for(int server_fd)
{
    int i;

    for (i = 0; i < WL_SOCK_MAX_CONNS; i++) {
        if (g_conns[i].used && g_conns[i].server_fd == server_fd)
            return &g_conns[i];
    }
    return NULL;
}

static struct wl_obj *find_obj(struct wl_conn *c, uint32_t id)
{
    int i;

    if (id == 0) return NULL;
    for (i = 0; i < WL_SERVER_MAX_OBJECTS; i++) {
        if (c->objs[i].type != OBJ_NONE && c->objs[i].id == id)
            return &c->objs[i];
    }
    return NULL;
}

static struct wl_obj *add_obj(struct wl_conn *c, uint32_t id, int type)
{
    int i;
    struct wl_obj *slot = NULL;

    if (id == 0) return NULL;
    if (find_obj(c, id) != NULL) {
        console_printf("[wl] protocol error: object id %u already in use\n",
                       (uint64_t)id);
        return NULL;
    }
    for (i = 0; i < WL_SERVER_MAX_OBJECTS; i++) {
        if (c->objs[i].type == OBJ_NONE) { slot = &c->objs[i]; break; }
    }
    if (slot == NULL) {
        console_printf("[wl] protocol error: object table full (id=%u)\n",
                       (uint64_t)id);
        return NULL;
    }
    memset(slot, 0, sizeof(*slot));
    slot->id = id;
    slot->type = type;
    slot->pool_fd = -1;
    return slot;
}

static void free_obj(struct wl_obj *o)
{
    memset(o, 0, sizeof(*o));           /* type == OBJ_NONE releases the slot */
}

/* ===================== request argument reader ===================== */
struct rbuf {
    const uint8_t *p;
    uint32_t len;
    uint32_t off;
    int      err;
};

static uint32_t rb_u32(struct rbuf *r)
{
    uint32_t v;

    if (r->off + 4u > r->len) { r->err = 1; return 0; }
    v = rd_u32(r->p + r->off);
    r->off += 4u;
    return v;
}

static int32_t rb_i32(struct rbuf *r)
{
    return (int32_t)rb_u32(r);
}

/* Reads a string into `dst` (NUL terminated). Returns 1 on success. */
static int rb_str(struct rbuf *r, char *dst, uint32_t cap)
{
    uint32_t n = rb_u32(r);
    uint32_t padded;

    if (r->err || n == 0u || n > cap) { r->err = 1; return 0; }
    if (r->off + n > r->len) { r->err = 1; return 0; }
    memcpy(dst, r->p + r->off, n);
    dst[cap - 1u] = '\0';
    r->off += n;
    padded = (n + 3u) & ~3u;
    if (r->off + (padded - n) > r->len) { r->err = 1; return 0; }
    r->off += padded - n;
    return 1;
}

/* ===================== event emission ===================== */
static void conn_flush(struct wl_conn *c)
{
    uint32_t off = 0;

    while (off < c->out_len) {
        const int n = unix_write_fd(c->server_fd, c->out + off, c->out_len - off);
        if (n <= 0) break;               /* client ring full: keep the rest queued */
        off += (uint32_t)n;
    }
    if (off > 0) {
        memmove(c->out, c->out + off, c->out_len - off);
        c->out_len -= off;
    }
}

static void conn_queue(struct wl_conn *c, const uint8_t *buf, uint32_t len)
{
    if (c->out_len + len > WL_SERVER_OUT_MAX) {
        console_printf("[wl] event dropped: client is not reading (%u bytes "
                       "queued)\n", (uint64_t)c->out_len);
        return;
    }
    memcpy(c->out + c->out_len, buf, len);
    c->out_len += len;
}

/* Begin an event: writes the header with a size placeholder and returns the
 * buffer; ev_end() patches the size and queues it. */
static struct wbuf ev_begin(struct wl_conn *c, uint32_t obj_id, uint16_t opcode,
                     uint8_t *scratch, uint32_t cap)
{
    struct wbuf b;

    b.p = scratch;
    b.len = 0;
    b.cap = cap;
    b.overflow = 0;
    wb_u32(&b, obj_id);
    wb_u16(&b, opcode);
    wb_u16(&b, 0);                       /* size patched by ev_end() */
    return b;
}

static void ev_end(struct wl_conn *c, struct wbuf *b)
{
    if (b->overflow) {
        console_printf("[wl] event too large, dropped\n");
        return;
    }
    if (b->len < WL_HDR_SIZE || (b->len & 3u) != 0u) {
        console_printf("[wl] internal: malformed event (%u bytes)\n",
                       (uint64_t)b->len);
        return;
    }
    wr_u16(b->p + 6, (uint16_t)b->len);
    conn_queue(c, b->p, b->len);
}

static void emit_registry_global(struct wl_conn *c, uint32_t registry_id,
                                 const struct wl_global_def *g)
{
    uint8_t scratch[128];
    struct wbuf b = ev_begin(c, registry_id, 0u, scratch, sizeof(scratch));

    wb_u32(&b, g->name);
    wb_str(&b, g->iface);
    wb_u32(&b, g->version);
    ev_end(c, &b);
}

static void emit_callback_done(struct wl_conn *c, uint32_t callback_id,
                               uint32_t data)
{
    uint8_t scratch[32];
    struct wbuf b = ev_begin(c, callback_id, 0u, scratch, sizeof(scratch));

    wb_u32(&b, data);
    ev_end(c, &b);
}

static void emit_buffer_release(struct wl_conn *c, uint32_t buffer_id)
{
    uint8_t scratch[16];
    struct wbuf b = ev_begin(c, buffer_id, 0u, scratch, sizeof(scratch));

    ev_end(c, &b);
}

static void emit_seat_capabilities(struct wl_conn *c, uint32_t seat_id,
                                   uint32_t caps)
{
    uint8_t scratch[32];
    struct wbuf b = ev_begin(c, seat_id, 0u, scratch, sizeof(scratch));

    wb_u32(&b, caps);
    ev_end(c, &b);
}

/* ===================== FNV-1a ===================== */
#define FNV_OFFSET 2166136261u
#define FNV_PRIME  16777619u

static uint32_t fnv1a_update(uint32_t h, const uint8_t *p, uint32_t n)
{
    uint32_t i;

    for (i = 0; i < n; i++) {
        h ^= (uint32_t)p[i];
        h *= FNV_PRIME;
    }
    return h;
}

/* ===================== wl_surface.commit ===================== */
static void commit_surface(struct wl_conn *c, struct wl_obj *surf)
{
    struct wl_obj *buf;
    struct wl_obj *pool;
    uint32_t need;
    int size;
    int y;
    uint32_t hash = FNV_OFFSET;
    static uint8_t row[WL_SURFACE_MAX_W * 4];
    uint32_t copied = 0;

    if (surf->buffer_id == 0) {
        console_printf("[wl] commit surface=%u: no buffer attached\n",
                       (uint64_t)surf->id);
        return;
    }
    buf = find_obj(c, (uint32_t)surf->buffer_id);
    if (buf == NULL || buf->type != OBJ_BUFFER) {
        console_printf("[wl] commit surface=%u: buffer %d unknown\n",
                       (uint64_t)surf->id, (int64_t)surf->buffer_id);
        return;
    }
    pool = find_obj(c, (uint32_t)buf->pool_id);
    if (pool == NULL || pool->type != OBJ_POOL || pool->pool_fd < 0) {
        console_printf("[wl] commit surface=%u: buffer %u has no pool\n",
                       (uint64_t)surf->id, (uint64_t)buf->id);
        return;
    }
    size = memfd_size(pool->pool_fd);
    if (size <= 0) {
        console_printf("[wl] commit surface=%u: pool fd=%d is not a usable "
                       "memfd\n", (uint64_t)surf->id, (int64_t)pool->pool_fd);
        return;
    }
    need = (uint32_t)buf->offset + (uint32_t)buf->h * (uint32_t)buf->stride;
    if (need > (uint32_t)size) {
        console_printf("[wl] commit surface=%u: buffer needs %u bytes but the "
                       "pool is %d\n", (uint64_t)surf->id, (uint64_t)need,
                       (int64_t)size);
        return;
    }
    if (buf->w <= 0 || buf->h <= 0 || buf->w > WL_SURFACE_MAX_W ||
        buf->h > WL_SURFACE_MAX_H || buf->stride < buf->w * 4 ||
        (uint32_t)(buf->stride * buf->h) > WL_SURFACE_MAX_BYTES) {
        console_printf("[wl] commit surface=%u: buffer geometry %dx%d stride=%d "
                       "is outside this server's limit (%dx%d)\n",
                       (uint64_t)surf->id, (int64_t)buf->w, (int64_t)buf->h,
                       (int64_t)buf->stride, (uint64_t)WL_SURFACE_MAX_W,
                       (uint64_t)WL_SURFACE_MAX_H);
        return;
    }

    /* Read the pixels out of the shared pool the client handed us: this is the
     * SCM_RIGHTS + memfd path actually delivering bytes to the server. */
    for (y = 0; y < buf->h; y++) {
        const uint32_t off = (uint32_t)buf->offset +
                             (uint32_t)y * (uint32_t)buf->stride;
        const uint32_t n = (uint32_t)buf->w * 4u;

        if (memfd_read(pool->pool_fd, off, row, n) != (int)n) {
            console_printf("[wl] commit surface=%u: memfd read failed at y=%d\n",
                           (uint64_t)surf->id, (int64_t)y);
            return;
        }
        hash = fnv1a_update(hash, row, n);
        if (copied + n <= WL_SURFACE_MAX_BYTES) {
            memcpy(g_last_px + copied, row, n);
            copied += n;
        }
    }

    g_last_w = buf->w;
    g_last_h = buf->h;
    g_last_stride = buf->stride;
    g_last_hash = hash;
    g_last_valid = 1;
    g_commits++;

    console_printf("[wl] commit surface=%u buffer=%u %dx%d stride=%d hash=%08x "
                   "(%u bytes from memfd fd=%d)\n",
                   (uint64_t)surf->id, (uint64_t)buf->id, (int64_t)buf->w,
                   (int64_t)buf->h, (int64_t)buf->stride, (uint64_t)hash,
                   (uint64_t)copied, (int64_t)pool->pool_fd);

    surf->attach_x = 0;                  /* commit consumes the attachment offset */
    surf->attach_y = 0;

    emit_buffer_release(c, buf->id);
    console_printf("[wl] buffer.release id=%u\n", (uint64_t)buf->id);
}

/* ===================== request dispatch ===================== */
static void dispatch_display(struct wl_conn *c, uint16_t opcode, struct rbuf *r)
{
    switch (opcode) {
    case 0: {                            /* wl_display.sync(new_id) */
        const uint32_t id = rb_u32(r);
        if (r->err || add_obj(c, id, OBJ_CALLBACK) == NULL) return;
        console_printf("[wl] sync id=%u -> callback.done\n", (uint64_t)id);
        emit_callback_done(c, id, 0x5e770000u | (id & 0xFFFFu));
        break;
    }
    case 1: {                            /* wl_display.get_registry(new_id) */
        const uint32_t id = rb_u32(r);
        int i;

        if (r->err || add_obj(c, id, OBJ_REGISTRY) == NULL) return;
        console_printf("[wl] get_registry id=%u (%d globals)\n", (uint64_t)id,
                       (int64_t)WL_GLOBAL_COUNT);
        for (i = 0; i < WL_GLOBAL_COUNT; i++)
            emit_registry_global(c, id, &g_globals[i]);
        break;
    }
    default:
        console_printf("[wl] wl_display: unsupported opcode %u\n",
                       (uint64_t)opcode);
        break;
    }
}

static void dispatch_registry(struct wl_conn *c, uint16_t opcode, struct rbuf *r)
{
    char iface[64];
    uint32_t name;
    uint32_t version;
    uint32_t new_id;
    const struct wl_global_def *g = NULL;
    struct wl_obj *o;
    int i;

    if (opcode != 0u) {
        console_printf("[wl] wl_registry: unsupported opcode %u\n",
                       (uint64_t)opcode);
        return;
    }
    name = rb_u32(r);
    if (!rb_str(r, iface, sizeof(iface))) return;
    version = rb_u32(r);
    new_id = rb_u32(r);
    if (r->err) return;

    for (i = 0; i < WL_GLOBAL_COUNT; i++) {
        if (g_globals[i].name == name) { g = &g_globals[i]; break; }
    }
    if (g == NULL || strcmp(g->iface, iface) != 0) {
        console_printf("[wl] wl_registry.bind: no global %u named \"%s\"\n",
                       (uint64_t)name, iface);
        return;
    }
    o = add_obj(c, new_id, g->type);
    if (o == NULL) return;
    console_printf("[wl] bind id=%u interface=\"%s\" version=%u\n",
                   (uint64_t)new_id, g->iface, (uint64_t)version);
    if (g->type == OBJ_SEAT) {
        /* WL_SEAT_CAPABILITY_POINTER(1) | WL_SEAT_CAPABILITY_KEYBOARD(2) */
        console_printf("[wl] seat capabilities id=%u caps=3\n",
                       (uint64_t)new_id);
        emit_seat_capabilities(c, new_id, 3u);
    }
}

static void dispatch_compositor(struct wl_conn *c, uint16_t opcode,
                                struct rbuf *r)
{
    if (opcode == 0u) {                  /* wl_compositor.create_surface */
        const uint32_t id = rb_u32(r);
        if (r->err || add_obj(c, id, OBJ_SURFACE) == NULL) return;
        console_printf("[wl] create_surface id=%u\n", (uint64_t)id);
        return;
    }
    if (opcode == 1u) {                  /* wl_compositor.create_region */
        const uint32_t id = rb_u32(r);
        if (r->err) return;
        console_printf("[wl] create_region id=%u (regions are not composited "
                       "by this server)\n", (uint64_t)id);
        (void)add_obj(c, id, OBJ_REGION);   /* inert: regions are not composited */
        return;
    }
    console_printf("[wl] wl_compositor: unsupported opcode %u\n",
                   (uint64_t)opcode);
}

static void dispatch_shm(struct wl_conn *c, uint16_t opcode, struct rbuf *r)
{
    uint32_t id;
    int32_t size;
    int fd = -1;

    if (opcode != 0u) {                  /* wl_shm.create_pool */
        console_printf("[wl] wl_shm: unsupported opcode %u\n", (uint64_t)opcode);
        return;
    }
    id = rb_u32(r);
    size = rb_i32(r);
    if (r->err) return;

    /* The pool descriptor travels next to the message: SCM_RIGHTS, not the wire. */
    if (unix_recv_fds(c->server_fd, &fd, 1) != 1) {
        console_printf("[wl] create_pool id=%u: no descriptor in the "
                       "SCM_RIGHTS queue\n", (uint64_t)id);
        return;
    }
    if (!memfd_is_fd(fd)) {
        console_printf("[wl] create_pool id=%u: fd=%d is not a memfd\n",
                       (uint64_t)id, (int64_t)fd);
        return;
    }
    {
        struct wl_obj *o = add_obj(c, id, OBJ_POOL);

        if (o == NULL) return;
        o->pool_fd = fd;
        o->pool_size = size;
        console_printf("[wl] create_pool id=%u fd=%d size=%d (memfd %d bytes)\n",
                       (uint64_t)id, (int64_t)fd, (int64_t)size,
                       (int64_t)memfd_size(fd));
    }
}

static void dispatch_pool(struct wl_conn *c, struct wl_obj *pool, uint16_t opcode,
                          struct rbuf *r)
{
    if (opcode == 0u) {                  /* wl_shm_pool.create_buffer */
        const uint32_t id = rb_u32(r);
        const int32_t offset = rb_i32(r);
        const int32_t w = rb_i32(r);
        const int32_t h = rb_i32(r);
        const int32_t stride = rb_i32(r);
        const uint32_t format = rb_u32(r);
        struct wl_obj *o;

        if (r->err) return;
        o = add_obj(c, id, OBJ_BUFFER);
        if (o == NULL) return;
        o->pool_id = (int32_t)pool->id;
        o->offset = offset;
        o->w = w;
        o->h = h;
        o->stride = stride;
        o->format = (int32_t)format;
        console_printf("[wl] create_buffer id=%u pool=%u offset=%d %dx%d "
                       "stride=%d format=%u\n", (uint64_t)id, (uint64_t)pool->id,
                       (int64_t)offset, (int64_t)w, (int64_t)h, (int64_t)stride,
                       (uint64_t)format);
        if (format != WL_FMT_XRGB8888)
            console_printf("[wl] create_buffer id=%u: format %u is not XRGB8888,"
                           " the compositor will not blit it\n", (uint64_t)id,
                           (uint64_t)format);
        return;
    }
    if (opcode == 1u) {                  /* wl_shm_pool.destroy */
        console_printf("[wl] shm_pool.destroy id=%u\n", (uint64_t)pool->id);
        free_obj(pool);
        return;
    }
    console_printf("[wl] wl_shm_pool: unsupported opcode %u\n", (uint64_t)opcode);
}

static void dispatch_surface(struct wl_conn *c, struct wl_obj *surf,
                             uint16_t opcode, struct rbuf *r)
{
    switch (opcode) {
    case 0u:                             /* wl_surface.destroy */
        console_printf("[wl] surface.destroy id=%u\n", (uint64_t)surf->id);
        free_obj(surf);
        break;
    case 1u: {                           /* wl_surface.attach(buffer, x, y) */
        const int32_t buffer_id = rb_i32(r);
        const int32_t x = rb_i32(r);
        const int32_t y = rb_i32(r);

        if (r->err) return;
        surf->buffer_id = buffer_id;
        surf->attach_x = x;
        surf->attach_y = y;
        console_printf("[wl] attach surface=%u buffer=%d x=%d y=%d\n",
                       (uint64_t)surf->id, (int64_t)buffer_id, (int64_t)x,
                       (int64_t)y);
        break;
    }
    case 2u:                             /* wl_surface.damage(x,y,w,h) */
        (void)rb_u32(r); (void)rb_u32(r); (void)rb_u32(r); (void)rb_u32(r);
        break;
    case 6u:                             /* wl_surface.commit */
        commit_surface(c, surf);
        break;
    default:
        console_printf("[wl] wl_surface: unsupported opcode %u "
                       "(frame/damage-buffer/regions/scaling are not "
                       "implemented)\n", (uint64_t)opcode);
        break;
    }
}

static void dispatch_seat(struct wl_conn *c, struct wl_obj *seat,
                          uint16_t opcode, struct rbuf *r)
{
    const uint32_t id = rb_u32(r);

    if (r->err) return;
    if (opcode == 0u) {
        if (add_obj(c, id, OBJ_POINTER) == NULL) return;
        console_printf("[wl] seat.get_pointer id=%u (inert: this server has no "
                       "input routing yet)\n", (uint64_t)id);
        return;
    }
    if (opcode == 1u) {
        if (add_obj(c, id, OBJ_KEYBOARD) == NULL) return;
        console_printf("[wl] seat.get_keyboard id=%u (inert: this server has no "
                       "input routing yet)\n", (uint64_t)id);
        return;
    }
    console_printf("[wl] wl_seat(id=%u): unsupported opcode %u\n",
                   (uint64_t)seat->id, (uint64_t)opcode);
}

static void dispatch(struct wl_conn *c, const uint8_t *msg, uint32_t size)
{
    struct rbuf r;
    struct wl_obj *o;
    const uint32_t obj_id = rd_u32(msg);
    const uint16_t opcode = rd_u16(msg + 4);

    r.p = msg;
    r.len = size;
    r.off = WL_HDR_SIZE;
    r.err = 0;

    o = find_obj(c, obj_id);
    if (o == NULL) {
        console_printf("[wl] request for unknown object %u (opcode %u)\n",
                       (uint64_t)obj_id, (uint64_t)opcode);
        return;
    }
    switch (o->type) {
    case OBJ_DISPLAY:     dispatch_display(c, opcode, &r); break;
    case OBJ_REGISTRY:    dispatch_registry(c, opcode, &r); break;
    case OBJ_COMPOSITOR:  dispatch_compositor(c, opcode, &r); break;
    case OBJ_SHM:         dispatch_shm(c, opcode, &r); break;
    case OBJ_POOL:        dispatch_pool(c, o, opcode, &r); break;
    case OBJ_SURFACE:     dispatch_surface(c, o, opcode, &r); break;
    case OBJ_SEAT:        dispatch_seat(c, o, opcode, &r); break;
    case OBJ_BUFFER:
        if (opcode == 0u) {
            console_printf("[wl] buffer.destroy id=%u\n", (uint64_t)o->id);
            free_obj(o);
        }
        break;
    case OBJ_CALLBACK:
        console_printf("[wl] wl_callback(id=%u): requests are not allowed\n",
                       (uint64_t)o->id);
        break;
    default:
        break;                           /* inert pointer/keyboard/region */
    }
}

/* ===================== public API ===================== */
int wl_server_autostart(void)
{
    if (g_started) return 0;
    g_started = 1;
    console_printf("[wl] minimal Wayland server ready (lazy init: named "
                   "endpoint \"%s\", %d globals: wl_compositor/wl_shm/wl_seat)\n",
                   WL_SERVER_NAME, (int64_t)WL_GLOBAL_COUNT);
    return 0;
}

void wl_server_accept(int server_fd)
{
    struct wl_conn *c = conn_for(server_fd);
    int i;

    if (c == NULL) {
        for (i = 0; i < WL_SOCK_MAX_CONNS; i++) {
            if (!g_conns[i].used) { c = &g_conns[i]; break; }
        }
    }
    if (c == NULL) {
        console_printf("[wl] accept: connection table full\n");
        return;
    }
    memset(c, 0, sizeof(*c));
    c->used = 1;
    c->server_fd = server_fd;
    c->in_len = 0;
    c->out_len = 0;
    (void)add_obj(c, WL_DISPLAY_ID, OBJ_DISPLAY);
    console_printf("[wl] protocol state reset for server_fd=%d (wl_display id=%u)\n",
                   (int64_t)server_fd, (uint64_t)WL_DISPLAY_ID);
}

void wl_server_pump(int server_fd)
{
    struct wl_conn *c = conn_for(server_fd);

    if (c == NULL) return;

    conn_flush(c);

    /* Drain everything the client has written so far. */
    while (c->in_len < WL_SERVER_IN_MAX) {
        const int n = unix_read_fd(server_fd, c->in + c->in_len,
                                   WL_SERVER_IN_MAX - c->in_len);
        if (n <= 0) break;               /* 0 = client closed, -EAGAIN = empty */
        c->in_len += (uint32_t)n;
    }

    while (c->in_len >= WL_HDR_SIZE) {
        const uint16_t size = rd_u16(c->in + 6);

        if (size < WL_HDR_SIZE || size > WL_SERVER_IN_MAX) {
            console_printf("[wl] protocol error: message size %u (dropping the "
                           "connection buffer)\n", (uint64_t)size);
            c->in_len = 0;
            break;
        }
        if ((uint32_t)size > c->in_len) break;    /* wait for the rest */
        dispatch(c, c->in, (uint32_t)size);
        memmove(c->in, c->in + size, c->in_len - (uint32_t)size);
        c->in_len -= (uint32_t)size;
    }

    conn_flush(c);
}

int wl_server_latest_surface(int *w, int *h, int *stride, const uint8_t **px)
{
    if (!g_last_valid) return 0;
    if (w) *w = g_last_w;
    if (h) *h = g_last_h;
    if (stride) *stride = g_last_stride;
    if (px) *px = g_last_px;
    return 1;
}

uint32_t wl_server_latest_hash(void)
{
    return g_last_hash;
}

uint32_t wl_server_commit_count(void)
{
    return g_commits;
}

int wl_server_active(void)
{
    return g_started;
}
