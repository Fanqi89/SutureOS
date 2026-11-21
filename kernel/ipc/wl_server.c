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

#include "mb_mouse.h"      /* the kernel's PS/2 pointer state for wl_pointer    */
#include "mb_kbd.h"        /* the decoded-scancode mirror: wl_keyboard source   */
#include "time_source.h"   /* monotonic ms for wl_pointer/wl_keyboard time      */
#include "memfd.h"
#include "scm_fd.h"
#include "unix_socket.h"
#include "wl_sock.h"

/* ===================== wire constants ===================== */
#define WL_HDR_SIZE     8u
#define WL_DISPLAY_ID   1u
#define WL_FMT_XRGB8888 1u

/* wl_display event opcodes (server -> client) */
#define WL_DISPLAY_EV_ERROR      0u
#define WL_DISPLAY_EV_DELETE_ID  1u

/* wl_display.error codes (wayland.xml, wl_display.error enum) */
#define WL_ERR_INVALID_OBJECT 0u
#define WL_ERR_INVALID_METHOD 1u

/* wl_pointer event opcodes (server -> client) */
#define WL_POINTER_EV_ENTER   0u
#define WL_POINTER_EV_LEAVE   1u
#define WL_POINTER_EV_MOTION  2u
#define WL_POINTER_EV_BUTTON  3u
#define WL_POINTER_EV_FRAME   5u          /* wl_pointer version >= 5          */

/* wl_keyboard event opcodes (server -> client) */
#define WL_KB_EV_KEYMAP       0u
#define WL_KB_EV_ENTER        1u
#define WL_KB_EV_LEAVE        2u
#define WL_KB_EV_KEY          3u
#define WL_KB_EV_MODIFIERS    4u
#define WL_KB_EV_REPEAT_INFO  5u          /* wl_keyboard version >= 4          */

/* wl_keyboard.repeat_info(rate, delay): rate is characters per second (a
 * negative rate disables repeat), delay is the milliseconds before the first
 * repeat.  30 chars/s and 400 ms is a typical desktop default. */
#define WL_KB_REPEAT_RATE   30
#define WL_KB_REPEAT_DELAY  400u

/* wl_keyboard.keymap format: the spec's own "no keymap" value.  This kernel has
 * no XKB data, so the keymap event carries no descriptor and says so, and the
 * evdev keycodes in wl_keyboard.key are the mapping the client must use. */
#define WL_KB_KEYMAP_NO_KEYMAP 0u
#define WL_KB_KEY_STATE_RELEASED 0u
#define WL_KB_KEY_STATE_PRESSED  1u

/* wl_surface request opcodes that this server implements (wayland.xml) */
#define WL_SURFACE_SET_OPAQUE_REGION 4u
#define WL_SURFACE_SET_INPUT_REGION  5u
#define WL_SURFACE_COMMIT            6u
#define WL_SURFACE_SET_BUFFER_TRANSFORM 7u
#define WL_SURFACE_SET_BUFFER_SCALE  8u
#define WL_SURFACE_DAMAGE_BUFFER     9u   /* NOT 7: 7 is set_buffer_transform   */
#define WL_SURFACE_OFFSET            10u

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
    OBJ_REGION,
    OBJ_SUBCOMPOSITOR,
    OBJ_SUBSURFACE
};

struct wl_global_def {
    const char *iface;
    uint32_t    version;
    uint32_t    name;          /* the number wl_registry.bind() takes */
    int         type;
};

/* Advertised versions are the ones this server actually honours:
 *   wl_compositor v4 - v3 adds wl_surface.set_buffer_scale (implemented);
 *   wl_seat       v7 - v5 adds wl_pointer.frame (emitted only when the client
 *                      bound the pointer at >= 5, which is what the protocol
 *                      requires);
 *   wl_subcompositor v1 - get_subsurface/set_position/set_sync. */
static const struct wl_global_def g_globals[] = {
    { "wl_compositor",    4u, 1u, OBJ_COMPOSITOR },
    { "wl_shm",           1u, 2u, OBJ_SHM },
    { "wl_seat",          7u, 3u, OBJ_SEAT },
    { "wl_subcompositor", 1u, 4u, OBJ_SUBCOMPOSITOR },
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
    /* wl_surface: pending wl_surface.frame callback + accumulated damage */
    uint32_t frame_cb_id;      /* 0 = no frame callback requested             */
    int32_t  damage_x, damage_y, damage_w, damage_h;
    int      damage_valid;
    /* wl_surface: set_buffer_scale (v3) and the two region slots (v1) */
    int32_t  scale;            /* 1 unless the client set another value        */
    uint32_t opaque_region;    /* wl_region id, 0 = none                       */
    uint32_t input_region;     /* wl_region id, 0 = none                       */
    int32_t  off_x, off_y;     /* wl_surface.offset (v5)                       */
    int32_t  transform;        /* set_buffer_transform: recorded, not applied  */
    /* wl_callback */
    int      cb_done;
    uint32_t cb_serial;
    /* wl_pointer / wl_keyboard: the live event stream state.  `pointer_sent`
     * means "enter was emitted for the surface this device is on". */
    int      pointer_sent;
    uint32_t version;          /* version the client bound this object at      */
    uint32_t entered_surface;  /* surface the device entered (0 = not entered) */
    int32_t  last_x, last_y;   /* last pointer position (surface-local)        */
    int32_t  last_buttons;
    uint32_t last_mods;        /* last modifier mask sent to wl_keyboard       */
    /* wl_region: running rectangle bookkeeping (count / area / bounding box) */
    int32_t  rects, sub_rects;
    int32_t  area;             /* sum(add) - sum(subtract), clamped at 0        */
    int32_t  r_x, r_y, r_w, r_h;   /* bounding box of the added rects          */
    /* wl_subsurface: parent/child link, position, sync mode, place-below     */
    uint32_t child_id;         /* wl_surface id this subsurface belongs to     */
    uint32_t parent_id;        /* wl_surface id of the parent                  */
    uint32_t parent_sub_id;    /* the OBJ_SUBSURFACE this surface belongs to   */
    int32_t  pos_x, pos_y;
    int      sync_mode;
    int      place_below;
    /* wl_keyboard: a real XKB keymap is sent once per keyboard object          */
    int      keymap_sent;
    /* wl_buffer lifecycle: one release per attach cycle                        */
    int      released;
    /* wl_shm_pool: destroy keeps the descriptor alive for existing buffers     */
    int      destroyed;
    int32_t  child_buffers;    /* live wl_buffer objects created from this pool  */
};

struct wl_conn {
    int      used;
    int      server_fd;
    struct wl_obj objs[WL_SERVER_MAX_OBJECTS];
    uint8_t  in[WL_SERVER_IN_MAX];
    uint32_t in_len;
    uint8_t  out[WL_SERVER_OUT_MAX];
    uint32_t out_len;
    uint32_t kb_events;
    uint32_t ptr_events;
};

static struct wl_conn g_conns[WL_SERVER_MAX_CONNS];

/* v0.62 helpers, defined with the other memfd accounting code further down: they
 * are also used by conn_release/every pool path above their definition. */
static void log_memfd_live(const char *why);
static void keymap_unuse(const char *why);

/* v0.61: drop a connection slot once its client is gone. The retained surfaces in
 * g_keep[] are deliberately left alone - the desktop keeps showing the last frame
 * of an exited client (that is what "retained" means here). */
static void conn_release(struct wl_conn *c)
{
    int i;

    if (c == NULL || !c->used) return;
    console_printf("[wl] client disconnected server_fd=%d "
                   "(connection slot %d released, retained surfaces kept)\n",
                   (int64_t)c->server_fd, (int64_t)(c - g_conns));
    /* Hand every descriptor this client made us keep back to the memfd layer. */
    for (i = 0; i < WL_SERVER_MAX_OBJECTS; i++) {
        struct wl_obj *o = &c->objs[i];

        if (o->type == OBJ_POOL && o->pool_fd >= 0) {
            (void)memfd_unref(o->pool_fd);
            o->pool_fd = -1;
            log_memfd_live("pool released (connection gone)");
        } else if (o->type == OBJ_KEYBOARD) {
            keymap_unuse("connection gone");
        }
    }
    memset(c, 0, sizeof(*c));
}

/* 1 when the peer end is gone (unix_poll_fd reports HUP, or the fd no longer
 * decodes because the endpoint was already torn down). */
static int conn_peer_dead(const struct wl_conn *c)
{
    const int ev = unix_poll_fd(c->server_fd);
    if (ev == 0) return 1;
    return (ev & UNIX_POLLHUP) != 0;
}

/* ===================== per-connection retained surfaces =====================
 * The commit that wins for a *connection* keeps its own pixel copy here, so a
 * second client's commit can no longer clobber the first one's frame.  This is
 * the whole point of the multi-client work: the server used to keep one global
 * g_last_px, which is why a second client silently replaced the first.
 *
 * The buffer is not released when a client exits: the compositor still blits the
 * last commit through wl_server_latest_surface(), exactly as before. */
struct wl_keep {
    int      used;
    int      conn;                     /* index into g_conns                  */
    int      w, h, stride, scale;
    uint32_t serial, hash, surface_id;
    uint8_t  px[WL_SURFACE_MAX_BYTES];
};

static struct wl_keep g_keep[WL_SERVER_MAX_CONNS];
static int      g_latest_keep = -1;    /* index of the most recent commit       */
static uint32_t g_commits;
static uint32_t g_serial;              /* monotonic Wayland serial (>= 1)       */
static uint32_t g_events_dropped;      /* events dropped because a ring was full */
static uint32_t g_kb_events;           /* wl_keyboard.key events emitted        */
static uint32_t g_ptr_events;          /* wl_pointer events emitted             */
static uint32_t g_ptr_frames;          /* wl_pointer.frame events emitted       */
static struct wl_obj *g_subsurfaces[8];  /* live wl_subsurface objects (export)  */
static int      g_started;

/* ===================== window placement + focus gate (v0.61) =================
 * The desktop shell owns the Wayland window; these weak hooks are how it tells
 * the server where that window is and whether it holds the keyboard.  The weak
 * definitions right below are the fallbacks for a kernel that does not link the
 * Rust GUI, so this file keeps building (and keeps the pre-0.61 semantics) on
 * every tree.
 *
 * No #include of the Rust header on purpose: this file must not depend on it. */
extern int suture_gui_wayland_rect(int *x, int *y, int *w, int *h)
    __attribute__((weak));
extern int suture_gui_wayland_focused(void) __attribute__((weak));

#define WL_WIN_DEF_X 250
#define WL_WIN_DEF_Y 330
#define WL_WIN_DEF_W 440
#define WL_WIN_DEF_H 330

/* Fallback window rectangle: the documented default geometry of the "Wayland
 * client" window, so the placement semantics stay exercisable when there is no
 * Rust GUI to ask. */
__attribute__((weak)) int suture_gui_wayland_rect(int *x, int *y, int *w, int *h)
{
    if (x) *x = WL_WIN_DEF_X;
    if (y) *y = WL_WIN_DEF_Y;
    if (w) *w = WL_WIN_DEF_W;
    if (h) *h = WL_WIN_DEF_H;
    return 1;
}

/* Fallback focus: no window manager information available, so nothing is gated
 * (this is the pre-0.61 behaviour that the terminal acceptance scripts use). */
__attribute__((weak)) int suture_gui_wayland_focused(void)
{
    return 1;
}

static int      g_win_ok;              /* a window rectangle is currently known */
static int      g_win_x, g_win_y, g_win_w, g_win_h;
static int      g_kb_focused = 1;      /* last focus state acted on             */
static uint32_t g_kb_gated;            /* key events dropped by the focus gate   */
static uint32_t g_ptr_leaves;          /* leaves because the pointer left        */
static uint32_t g_win_changes;         /* window rectangle changes seen         */
static int      g_win_from_shell;      /* 1 = the rectangle came from the shell  */

/* desktop-side stream state (no live connection: the window is showing a
 * surface that a client committed earlier) */
static int      g_desk_calls;          /* frames the shell pumped               */
static int      g_desk_inside;         /* desktop pointer is on the surface      */
static int      g_desk_focused = 1;    /* desktop keyboard focus state           */
static int      g_desk_lx = -1, g_desk_ly = -1;
static int      g_desk_buttons;
static uint32_t g_desk_events;         /* enter/motion/button records            */
static uint32_t g_desk_keys;           /* keys delivered while focused           */
static uint32_t g_desk_last_beat_calls;

/* wl_buffer lifecycle accounting (exported through wl_server_buffer_stats()) */
static uint32_t g_buf_created;
static uint32_t g_buf_released;
static uint32_t g_buf_destroyed;
static uint32_t g_buf_release_skipped;

/* One line per lifecycle change so the acceptance can read the counters off the
 * serial log (wl_server_buffer_stats() is a kernel API with no ring-3 caller). */
static void log_buffer_stats(void)
{
    console_printf("[wl] buffer stats created=%u released=%u destroyed=%u "
                   "skipped=%u\n", (uint64_t)g_buf_created,
                   (uint64_t)g_buf_released, (uint64_t)g_buf_destroyed,
                   (uint64_t)g_buf_release_skipped);
}

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
        g_events_dropped++;
        console_printf("[wl] event dropped: client is not reading (%u bytes "
                       "queued, %u dropped in total)\n", (uint64_t)c->out_len,
                       (uint64_t)g_events_dropped);
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

    (void)c;                          /* size is patched by ev_end() */
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

/* wl_display.error(object_id, code, message): the protocol's own way of saying
 * "that object/opcode is not valid". The id reported is the offending object, as
 * wayland.xml specifies (it may be an id the server does not know). */
static void emit_display_error(struct wl_conn *c, uint32_t object_id,
                               uint32_t code, const char *message)
{
    uint8_t scratch[128];
    struct wbuf b = ev_begin(c, WL_DISPLAY_ID, WL_DISPLAY_EV_ERROR, scratch,
                             sizeof(scratch));

    wb_u32(&b, object_id);
    wb_u32(&b, code);
    wb_str(&b, message);
    ev_end(c, &b);
    console_printf("[wl] display.error object=%u code=%u message=\"%s\"\n",
                   (uint64_t)object_id, (uint64_t)code, message);
}

/* wl_display.delete_id(id): tells the client the server released an id the client
 * allocated, so it may be reused. Sent for every client-initiated destroy. */
static void emit_display_delete_id(struct wl_conn *c, uint32_t id)
{
    uint8_t scratch[16];
    struct wbuf b = ev_begin(c, WL_DISPLAY_ID, WL_DISPLAY_EV_DELETE_ID, scratch,
                             sizeof(scratch));

    wb_u32(&b, id);
    ev_end(c, &b);
    console_printf("[wl] display.delete_id id=%u\n", (uint64_t)id);
}

/* Release an object the client asked to destroy: Wayland expects the server to
 * answer with wl_display.delete_id for the client-allocated id. */
static void destroy_obj(struct wl_conn *c, struct wl_obj *o)
{
    emit_display_delete_id(c, o->id);
    free_obj(o);
}

/* wl_pointer events, fixed-point 24.8 for the surface coordinates. */
static void emit_pointer_enter(struct wl_conn *c, uint32_t pointer_id,
                               uint32_t serial, uint32_t surface_id,
                               int x, int y)
{
    uint8_t scratch[64];
    struct wbuf b = ev_begin(c, pointer_id, WL_POINTER_EV_ENTER, scratch,
                             sizeof(scratch));

    wb_u32(&b, serial);
    wb_u32(&b, surface_id);
    wb_u32(&b, (uint32_t)(x << 8));
    wb_u32(&b, (uint32_t)(y << 8));
    ev_end(c, &b);
}

static void emit_pointer_motion(struct wl_conn *c, uint32_t pointer_id,
                                uint32_t time_ms, int x, int y)
{
    uint8_t scratch[64];
    struct wbuf b = ev_begin(c, pointer_id, WL_POINTER_EV_MOTION, scratch,
                             sizeof(scratch));

    wb_u32(&b, time_ms);
    wb_u32(&b, (uint32_t)(x << 8));
    wb_u32(&b, (uint32_t)(y << 8));
    ev_end(c, &b);
}

static void emit_pointer_button(struct wl_conn *c, uint32_t pointer_id,
                                uint32_t serial, uint32_t time_ms,
                                uint32_t button, uint32_t state)
{
    uint8_t scratch[64];
    struct wbuf b = ev_begin(c, pointer_id, WL_POINTER_EV_BUTTON, scratch,
                             sizeof(scratch));

    wb_u32(&b, serial);
    wb_u32(&b, time_ms);
    wb_u32(&b, button);
    wb_u32(&b, state);
    ev_end(c, &b);
}

/* wl_pointer.leave(serial, surface): sent when the client releases the pointer
 * (and when the surface it entered is replaced by a commit that names another
 * one), so a client always sees enter/leave paired. */
static void emit_pointer_leave(struct wl_conn *c, uint32_t pointer_id,
                               uint32_t serial, uint32_t surface_id)
{
    uint8_t scratch[32];
    struct wbuf b = ev_begin(c, pointer_id, WL_POINTER_EV_LEAVE, scratch,
                             sizeof(scratch));

    wb_u32(&b, serial);
    wb_u32(&b, surface_id);
    ev_end(c, &b);
}

/* wl_pointer.frame(): "this group of events is complete".  Only legal from
 * wl_pointer version 5 on, so it is emitted only when the client bound the
 * pointer at >= 5 (the version requested at registry.bind decides). */
static void emit_pointer_frame(struct wl_conn *c, uint32_t pointer_id)
{
    uint8_t scratch[16];
    struct wbuf b = ev_begin(c, pointer_id, WL_POINTER_EV_FRAME, scratch,
                             sizeof(scratch));

    ev_end(c, &b);
    g_ptr_frames++;
    console_printf("[wl] pointer frame id=%u\n", (uint64_t)pointer_id);
}

/* ===================== wl_keyboard events ===================== */
/* The real keymap.  wl_keyboard.keymap carries an XKB keymap as a memfd handed
 * over with SCM_RIGHTS; format 1 is WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 and the text
 * is a complete mini keymap for the keycodes this server emits (an X11 keycode is
 * the evdev code, which is exactly what wl_keyboard.key carries).  Kept small
 * (~1.4 KiB) and syntactically complete: keycodes/types/compatibility/symbols.
 *
 * The identical literal lives in scripts/wl_win_semantics_acceptance.py, which
 * recomputes the FNV-1a of the descriptor content on the host side; if the two
 * ever drift, the acceptance fails on the hash comparison instead of passing. */
static const char g_keymap_text[] =
    "xkb_keymap {\n"
    "  xkb_keycodes \"sutureos\" {\n"
    "    minimum=8; maximum=255;\n"
    "    <ESC>=9;\n"
    "    <AE01>=10; <AE02>=11; <AE03>=12; <AE04>=13; <AE05>=14; <AE06>=15;\n"
    "    <AE07>=16; <AE08>=17; <AE09>=18; <AE10>=19; <AE11>=20; <AE12>=21;\n"
    "    <BKSP>=22; <TAB>=23;\n"
    "    <AD01>=24; <AD02>=25; <AD03>=26; <AD04>=27; <AD05>=28; <AD06>=29;\n"
    "    <AD07>=30; <AD08>=31; <AD09>=32; <AD10>=33; <AD11>=34; <AD12>=35;\n"
    "    <RTRN>=36; <LCTL>=37;\n"
    "    <AC01>=38; <AC02>=39; <AC03>=40; <AC04>=41; <AC05>=42; <AC06>=43;\n"
    "    <AC07>=44; <AC08>=45; <AC09>=46; <AC10>=47; <AC11>=48;\n"
    "    <TLDE>=49; <LFSH>=50; <BKSL>=51;\n"
    "    <AB01>=52; <AB02>=53; <AB03>=54; <AB04>=55; <AB05>=56; <AB06>=57;\n"
    "    <AB07>=58; <AB08>=59; <AB09>=60; <AB10>=61;\n"
    "    <RTSH>=62; <LALT>=64; <SPCE>=65; <CAPS>=66;\n"
    "  };\n"
    "  xkb_types \"complete\" {\n"
    "    type \"ONE_LEVEL\" { modifiers=none; map[none]=Level1; };\n"
    "    type \"TWO_LEVEL\" { modifiers=Shift; map[Shift]=Level2; map[none]=Level1; };\n"
    "    type \"ALPHABETIC\" {\n"
    "      modifiers=Shift+Lock; map[Shift]=Level2; map[Lock]=Level2;\n"
    "      map[Shift+Lock]=Level2; map[none]=Level1;\n"
    "    };\n"
    "    type \"KEYPAD\" { modifiers=NumLock; map[NumLock]=Level2; map[none]=Level1; };\n"
    "  };\n"
    "  xkb_compatibility \"complete\" {\n"
    "    interpret Shift_L { action=SetMods(modifiers=Shift); };\n"
    "    interpret Shift_R { action=SetMods(modifiers=Shift); };\n"
    "    interpret Caps_Lock { action=LockMods(modifiers=Lock); };\n"
    "    interpret Control_L { action=SetMods(modifiers=Control); };\n"
    "    interpret Alt_L { action=SetMods(modifiers=Mod1); };\n"
    "    interpret Any+Any { action=SetMods(modifiers=modMapMods); };\n"
    "  };\n"
    "  xkb_symbols \"sutureos-us\" {\n"
    "    name[Group1]=\"SutureOS full US\";\n"
    "    key.type[Group1]=\"ALPHABETIC\";\n"
    "    key <ESC> { [ Escape ] };\n"
    "    key <AE01> { type=\"TWO_LEVEL\", [ 1, exclam ] };\n"
    "    key <AE02> { type=\"TWO_LEVEL\", [ 2, at ] };\n"
    "    key <AE03> { type=\"TWO_LEVEL\", [ 3, numbersign ] };\n"
    "    key <AE04> { type=\"TWO_LEVEL\", [ 4, dollar ] };\n"
    "    key <AE05> { type=\"TWO_LEVEL\", [ 5, percent ] };\n"
    "    key <AE06> { type=\"TWO_LEVEL\", [ 6, asciicircum ] };\n"
    "    key <AE07> { type=\"TWO_LEVEL\", [ 7, ampersand ] };\n"
    "    key <AE08> { type=\"TWO_LEVEL\", [ 8, asterisk ] };\n"
    "    key <AE09> { type=\"TWO_LEVEL\", [ 9, parenleft ] };\n"
    "    key <AE10> { type=\"TWO_LEVEL\", [ 0, parenright ] };\n"
    "    key <AE11> { type=\"TWO_LEVEL\", [ minus, underscore ] };\n"
    "    key <AE12> { type=\"TWO_LEVEL\", [ equal, plus ] };\n"
    "    key <BKSP> { [ BackSpace ] };\n"
    "    key <TAB> { [ Tab, ISO_Left_Tab ] };\n"
    "    key <AD01> { [ q, Q ] };\n"
    "    key <AD02> { [ w, W ] };\n"
    "    key <AD03> { [ e, E ] };\n"
    "    key <AD04> { [ r, R ] };\n"
    "    key <AD05> { [ t, T ] };\n"
    "    key <AD06> { [ y, Y ] };\n"
    "    key <AD07> { [ u, U ] };\n"
    "    key <AD08> { [ i, I ] };\n"
    "    key <AD09> { [ o, O ] };\n"
    "    key <AD10> { [ p, P ] };\n"
    "    key <AD11> { type=\"TWO_LEVEL\", [ bracketleft, braceleft ] };\n"
    "    key <AD12> { type=\"TWO_LEVEL\", [ bracketright, braceright ] };\n"
    "    key <RTRN> { [ Return ] };\n"
    "    key <LCTL> { [ Control_L ] };\n"
    "    key <AC01> { [ a, A ] };\n"
    "    key <AC02> { [ s, S ] };\n"
    "    key <AC03> { [ d, D ] };\n"
    "    key <AC04> { [ f, F ] };\n"
    "    key <AC05> { [ g, G ] };\n"
    "    key <AC06> { [ h, H ] };\n"
    "    key <AC07> { [ j, J ] };\n"
    "    key <AC08> { [ k, K ] };\n"
    "    key <AC09> { [ l, L ] };\n"
    "    key <AC10> { type=\"TWO_LEVEL\", [ semicolon, colon ] };\n"
    "    key <AC11> { type=\"TWO_LEVEL\", [ apostrophe, quotedbl ] };\n"
    "    key <TLDE> { type=\"TWO_LEVEL\", [ grave, asciitilde ] };\n"
    "    key <LFSH> { [ Shift_L ] };\n"
    "    key <BKSL> { type=\"TWO_LEVEL\", [ backslash, bar ] };\n"
    "    key <AB01> { [ z, Z ] };\n"
    "    key <AB02> { [ x, X ] };\n"
    "    key <AB03> { [ c, C ] };\n"
    "    key <AB04> { [ v, V ] };\n"
    "    key <AB05> { [ b, B ] };\n"
    "    key <AB06> { [ n, N ] };\n"
    "    key <AB07> { [ m, M ] };\n"
    "    key <AB08> { type=\"TWO_LEVEL\", [ comma, less ] };\n"
    "    key <AB09> { type=\"TWO_LEVEL\", [ period, greater ] };\n"
    "    key <AB10> { type=\"TWO_LEVEL\", [ slash, question ] };\n"
    "    key <RTSH> { [ Shift_R ] };\n"
    "    key <LALT> { [ Alt_L ] };\n"
    "    key <SPCE> { [ space ] };\n"
    "    key <CAPS> { [ Caps_Lock ] };\n"
    "  };\n"
    "};\n"
;

#define WL_KB_KEYMAP_XKB_V1 1u

static int      g_keymap_fd = -1;
static uint32_t g_keymap_users;    /* live wl_keyboard objects holding it */
static uint32_t g_keymap_size;
static uint32_t g_keymap_hash;

/* Create the keymap memfd once; returns 1 when a descriptor is available. */
static int keymap_ensure(void)
{
    const uint32_t len = (uint32_t)(sizeof g_keymap_text - 1u);

    if (g_keymap_fd >= 0) return 1;
    g_keymap_fd = memfd_create_obj();
    if (g_keymap_fd < 0) {
        console_printf("[wl] keymap: memfd_create failed (%d)\n",
                       (int64_t)g_keymap_fd);
        return 0;
    }
    if (memfd_resize(g_keymap_fd, len) != 0 ||
        memfd_write(g_keymap_fd, 0, g_keymap_text, (int)len) != (int)len) {
        console_printf("[wl] keymap: cannot fill the memfd (%u bytes)\n",
                       (uint64_t)len);
        g_keymap_fd = -1;
        return 0;
    }
    g_keymap_size = len;
    {
        /* FNV-1a inline: fnv1a_update() is defined further down (next to
         * commit_surface), and this helper runs earlier in the file. */
        const uint8_t *px = (const uint8_t *)g_keymap_text;
        uint32_t i;
        uint32_t h = 2166136261u;

        for (i = 0; i < len; i++) {
            h ^= (uint32_t)px[i];
            h *= 16777619u;
        }
        g_keymap_hash = h;
    }
    console_printf("[wl] keymap prepared: %u bytes in memfd fd=%d fnv1a=%08x "
                   "(format=%u XKB_V1)\n", (uint64_t)len, (int64_t)g_keymap_fd,
                   (uint64_t)g_keymap_hash, (uint64_t)WL_KB_KEYMAP_XKB_V1);
    log_memfd_live("keymap memfd created");
    return 1;
}


/* keymap(format, fd, size): the descriptor travels in SCM_RIGHTS and therefore
 * occupies no bytes on the wire, so the message body is only format + size.  A
 * real XKB keymap is sent (format 1); only when the memfd cannot be created does
 * the server fall back to the spec's NO_KEYMAP escape hatch, and it says so. */
static void emit_kb_keymap(struct wl_conn *c, uint32_t kb_id)
{
    uint8_t scratch[32];
    struct wbuf b;

    if (!keymap_ensure()) {
        b = ev_begin(c, kb_id, WL_KB_EV_KEYMAP, scratch, sizeof(scratch));
        wb_u32(&b, WL_KB_KEYMAP_NO_KEYMAP);
        wb_u32(&b, 0u);
        ev_end(c, &b);
        console_printf("[wl] keyboard keymap id=%u: NO_KEYMAP fallback (no memfd)\n",
                       (uint64_t)kb_id);
        return;
    }

    /* Queue the descriptor first: the pump flushes the event bytes at its end, so
     * the client's next recvmsg() sees the keymap event *and* its descriptor. */
    if (unix_send_fds(c->server_fd, &g_keymap_fd, 1) != 0)
        console_printf("[wl] keyboard keymap id=%u: SCM_RIGHTS queue is full\n",
                       (uint64_t)kb_id);

    b = ev_begin(c, kb_id, WL_KB_EV_KEYMAP, scratch, sizeof(scratch));
    wb_u32(&b, WL_KB_KEYMAP_XKB_V1);
    wb_u32(&b, g_keymap_size);
    ev_end(c, &b);
    console_printf("[wl] keyboard keymap id=%u format=%u size=%u fd=%d fnv1a=%08x "
                   "(SCM_RIGHTS)\n", (uint64_t)kb_id,
                   (uint64_t)WL_KB_KEYMAP_XKB_V1, (uint64_t)g_keymap_size,
                   (int64_t)g_keymap_fd, (uint64_t)g_keymap_hash);
}

/* enter(serial, surface, keys): the keys array is u32 size + payload; it is
 * empty here because the server has no "currently held keys" bookkeeping. */
static void emit_kb_enter(struct wl_conn *c, uint32_t kb_id, uint32_t serial,
                          uint32_t surface_id)
{
    uint8_t scratch[48];
    struct wbuf b = ev_begin(c, kb_id, WL_KB_EV_ENTER, scratch, sizeof(scratch));

    wb_u32(&b, serial);
    wb_u32(&b, surface_id);
    wb_u32(&b, 0u);                      /* array size: no pressed keys */
    ev_end(c, &b);
}

static void emit_kb_leave(struct wl_conn *c, uint32_t kb_id, uint32_t serial,
                          uint32_t surface_id)
{
    uint8_t scratch[32];
    struct wbuf b = ev_begin(c, kb_id, WL_KB_EV_LEAVE, scratch, sizeof(scratch));

    wb_u32(&b, serial);
    wb_u32(&b, surface_id);
    ev_end(c, &b);
}

/* key(serial, time, key, state): `key` is an evdev keycode (PS/2 set-1 make
 * code + 8), state 1 = pressed, 0 = released. */
static void emit_kb_key(struct wl_conn *c, uint32_t kb_id, uint32_t serial,
                        uint32_t time_ms, uint32_t key, uint32_t state)
{
    uint8_t scratch[48];
    struct wbuf b = ev_begin(c, kb_id, WL_KB_EV_KEY, scratch, sizeof(scratch));

    wb_u32(&b, serial);
    wb_u32(&b, time_ms);
    wb_u32(&b, key);
    wb_u32(&b, state);
    ev_end(c, &b);
}

/* modifiers(serial, depressed, latched, locked, group). */
static void emit_kb_modifiers(struct wl_conn *c, uint32_t kb_id, uint32_t serial,
                              uint32_t depressed, uint32_t locked)
{
    uint8_t scratch[48];
    struct wbuf b = ev_begin(c, kb_id, WL_KB_EV_MODIFIERS, scratch,
                             sizeof(scratch));

    wb_u32(&b, serial);
    wb_u32(&b, depressed);               /* mods_depressed  */
    wb_u32(&b, 0u);                      /* mods_latched    */
    wb_u32(&b, locked);                  /* mods_locked     */
    wb_u32(&b, 0u);                      /* group           */
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

/* ===================== v0.62: memfd accounting =====================
 * Every lifecycle event says how many memfd objects are alive, so the acceptance
 * can follow the count instead of trusting a single final number. */
static void log_memfd_live(const char *why)
{
    console_printf("[wl] memfd live=%d (%s)\n", (int64_t)memfd_live_count(), why);
}

/* wl_keyboard.repeat_info(rate, delay): how a client should auto-repeat a held
 * key.  Sent right after the keymap, before enter. */
static void emit_kb_repeat_info(struct wl_conn *c, uint32_t kb_id)
{
    uint8_t scratch[32];
    struct wbuf b = ev_begin(c, kb_id, WL_KB_EV_REPEAT_INFO, scratch,
                             sizeof(scratch));

    wb_u32(&b, (uint32_t)WL_KB_REPEAT_RATE);
    wb_u32(&b, WL_KB_REPEAT_DELAY);
    ev_end(c, &b);
    console_printf("[wl] keyboard repeat_info id=%u rate=%d delay=%u (chars/s, ms)\n",
                   (uint64_t)kb_id, (int64_t)WL_KB_REPEAT_RATE,
                   (uint64_t)WL_KB_REPEAT_DELAY);
}

/* The keymap memfd is shared by every live wl_keyboard: count the users and drop
 * the descriptor (one reference) when the last one goes away.  A later keyboard
 * simply recreates it through keymap_ensure(). */
static void keymap_unuse(const char *why)
{
    if (g_keymap_users > 0) g_keymap_users--;
    if (g_keymap_users == 0 && g_keymap_fd >= 0) {
        const int was = g_keymap_fd;

        memfd_unref(g_keymap_fd);
        g_keymap_fd = -1;
        g_keymap_size = 0;
        console_printf("[wl] keymap released fd=%d (%s)\n", (int64_t)was, why);
        log_memfd_live("keymap released");
    }
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
    /* A destroyed buffer is gone for good: the client must create a new one. */
    if (buf->destroyed) {
        console_printf("[wl] commit surface=%u refused: buffer %u was destroyed\n",
                       (uint64_t)surf->id, (uint64_t)buf->id);
        surf->buffer_id = 0;
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
     * SCM_RIGHTS + memfd path actually delivering bytes to the server.  They go
     * into *this connection's* retained copy, so a second client cannot
     * overwrite them (the old single g_last_px is what made that possible). */
    {
        const int ci = (int)(c - g_conns);
        struct wl_keep *k = &g_keep[ci];

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
                memcpy(k->px + copied, row, n);
                copied += n;
            }
        }
        if (ci >= 0 && ci < WL_SERVER_MAX_CONNS) {
            k->used = 1;
            k->conn = ci;
            k->w = buf->w;
            k->h = buf->h;
            k->stride = buf->stride;
            k->scale = (surf->scale >= 1) ? surf->scale : 1;
            k->hash = hash;
            k->surface_id = surf->id;
            k->serial = g_serial + 1u;      /* g_serial is bumped just below */
            g_latest_keep = ci;
        }
    }

    g_commits++;
    g_serial++;                          /* one serial per commit, monotonic   */

    /* The damage rect the client accumulated since the previous commit.  It is
     * logged on commit (nothing is repainted before that) and folded into the
     * commit line so a single line tells the whole story. */
    if (surf->damage_valid) {
        console_printf("[wl] damage surface=%u %d,%d %dx%d\n",
                       (uint64_t)surf->id, (int64_t)surf->damage_x,
                       (int64_t)surf->damage_y, (int64_t)surf->damage_w,
                       (int64_t)surf->damage_h);
        console_printf("[wl] commit surface=%u buffer=%u %dx%d stride=%d "
                       "hash=%08x (%u bytes from memfd fd=%d) damage=%d,%d %dx%d "
                       "serial=%u\n", (uint64_t)surf->id, (uint64_t)buf->id,
                       (int64_t)buf->w, (int64_t)buf->h, (int64_t)buf->stride,
                       (uint64_t)hash, (uint64_t)copied, (int64_t)pool->pool_fd,
                       (int64_t)surf->damage_x, (int64_t)surf->damage_y,
                       (int64_t)surf->damage_w, (int64_t)surf->damage_h,
                       (uint64_t)g_serial);
        surf->damage_valid = 0;
    } else {
        console_printf("[wl] commit surface=%u buffer=%u %dx%d stride=%d "
                       "hash=%08x (%u bytes from memfd fd=%d) serial=%u\n",
                       (uint64_t)surf->id, (uint64_t)buf->id, (int64_t)buf->w,
                       (int64_t)buf->h, (int64_t)buf->stride, (uint64_t)hash,
                       (uint64_t)copied, (int64_t)pool->pool_fd,
                       (uint64_t)g_serial);
    }

    /* scale / regions / subsurface are reported on their own line (appended
     * fields would break the existing commit-line parsers, and the acceptance
     * scripts read that line verbatim). */
    {
        const uint32_t scale = (surf->scale >= 1) ? (uint32_t)surf->scale : 1u;
        struct wl_obj *op = find_obj(c, surf->opaque_region);
        struct wl_obj *in = find_obj(c, surf->input_region);
        struct wl_obj *sub = (surf->parent_sub_id != 0u)
                                 ? find_obj(c, surf->parent_sub_id) : NULL;

        console_printf("[wl] surface state surface=%u scale=%u logical=%dx%d "
                       "opaque_area=%d input_area=%d subsurface_id=%u\n",
                       (uint64_t)surf->id, (uint64_t)scale,
                       (int64_t)(buf->w / (int32_t)scale),
                       (int64_t)(buf->h / (int32_t)scale),
                       op ? (int64_t)op->area : (int64_t)0,
                       in ? (int64_t)in->area : (int64_t)0,
                       sub ? (uint64_t)sub->id : 0u);
        if (sub != NULL) {
            console_printf("[wl] subsurface state id=%u surface=%u parent=%u "
                           "pos=%d,%d sync=%d below=%d\n", (uint64_t)sub->id,
                           (uint64_t)surf->id, (uint64_t)sub->parent_id,
                           (int64_t)sub->pos_x, (int64_t)sub->pos_y,
                           (int64_t)sub->sync_mode, (int64_t)sub->place_below);
        }
    }

    /* Every retained surface, so the host can verify that two clients kept
     * *different* pixels instead of the last one winning. */
    {
        int i, n = 0;

        console_printf("[wl] kept surfaces:");
        for (i = 0; i < WL_SERVER_MAX_CONNS; i++) {
            if (!g_keep[i].used) continue;
            n++;
            console_printf(" [%d] conn=%d surface=%u %dx%d stride=%d scale=%d "
                           "serial=%u hash=%08x", (int64_t)i,
                           (int64_t)g_keep[i].conn,
                           (uint64_t)g_keep[i].surface_id, (int64_t)g_keep[i].w,
                           (int64_t)g_keep[i].h, (int64_t)g_keep[i].stride,
                           (int64_t)g_keep[i].scale, (uint64_t)g_keep[i].serial,
                           (uint64_t)g_keep[i].hash);
        }
        console_printf(" total=%d latest=%d\n", (int64_t)n, (int64_t)g_latest_keep);
    }

    surf->attach_x = 0;                  /* commit consumes the attachment offset */
    surf->attach_y = 0;

    /* wl_surface.frame(): the callback requested by the client belongs to this
     * commit, so wl_callback.done(serial) goes out now.  The callback object is
     * kept (marked done) so the client can still destroy it. */
    if (surf->frame_cb_id != 0u) {
        struct wl_obj *cb = find_obj(c, surf->frame_cb_id);

        if (cb != NULL && cb->type == OBJ_CALLBACK) {
            cb->cb_done = 1;
            cb->cb_serial = g_serial;
            emit_callback_done(c, cb->id, g_serial);
            console_printf("[wl] frame callback done id=%u serial=%u\n",
                           (uint64_t)cb->id, (uint64_t)g_serial);
        }
        surf->frame_cb_id = 0;
    }

    /* One release per attach cycle: the attach that put this buffer in use is
     * what earns the release, so a commit that reuses an already-released buffer
     * without a fresh attach does not send a second one. */
    if (buf->released) {
        g_buf_release_skipped++;
        console_printf("[wl] buffer.release id=%u skipped (already released, no "
                       "re-attach)\n", (uint64_t)buf->id);
    } else {
        emit_buffer_release(c, buf->id);
        buf->released = 1;
        g_buf_released++;
        console_printf("[wl] buffer.release id=%u\n", (uint64_t)buf->id);
        log_buffer_stats();
    }
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
        emit_display_error(c, WL_DISPLAY_ID, WL_ERR_INVALID_METHOD,
                           "wl_display request is not implemented by this server");
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
    /* The client asks for a version; the server grants min(requested, global).
     * That granted version is what gates version-dependent events
     * (wl_pointer.frame is v5, wl_surface.set_buffer_scale is v3). */
    o->version = (version < g->version) ? version : g->version;
    if (o->version == 0u) o->version = 1u;
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
        struct wl_obj *reg;

        if (r->err) return;
        reg = add_obj(c, id, OBJ_REGION);
        if (reg == NULL) return;
        /* Semantics only: the rectangle list is accumulated and reported, but
         * nothing is clipped on screen (this server has no composite pass). */
        console_printf("[wl] create_region id=%u (rectangles are recorded and "
                       "reported, not composited)\n", (uint64_t)id);
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
        /* v0.62: the descriptor is *handed over* (SCM_RIGHTS here passes the same
         * encoded number, there is no per-fd refcount on the socket side), so the
         * server takes over the creator's reference and releases it when the pool
         * is done.  That is what makes memfd live fall back to 0. */
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
        /* A destroyed pool cannot mint new buffers, but the buffers it already
         * made stay usable (their descriptor is still owned by the server). */
        if (pool->destroyed) {
            console_printf("[wl] create_buffer id=%u refused: pool %u was "
                           "destroyed\n", (uint64_t)id, (uint64_t)pool->id);
            return;
        }
        o = add_obj(c, id, OBJ_BUFFER);
        if (o == NULL) return;
        o->pool_id = (int32_t)pool->id;
        o->offset = offset;
        o->w = w;
        o->h = h;
        o->stride = stride;
        o->format = (int32_t)format;
        o->released = 0;
        o->destroyed = 0;
        o->child_buffers = 0;
        pool->child_buffers++;
        g_buf_created++;
        console_printf("[wl] create_buffer id=%u pool=%u offset=%d %dx%d "
                       "stride=%d format=%u\n", (uint64_t)id, (uint64_t)pool->id,
                       (int64_t)offset, (int64_t)w, (int64_t)h, (int64_t)stride,
                       (uint64_t)format);
        log_buffer_stats();
        if (format != WL_FMT_XRGB8888)
            console_printf("[wl] create_buffer id=%u: format %u is not XRGB8888,"
                           " the compositor will not blit it\n", (uint64_t)id,
                           (uint64_t)format);
        return;
    }
    if (opcode == 1u) {                  /* wl_shm_pool.destroy */
        if (pool->destroyed) {
            console_printf("[wl] shm_pool.destroy id=%u ignored (already "
                           "destroyed)\n", (uint64_t)pool->id);
            return;
        }
        pool->destroyed = 1;
        pool->pool_size = 0;
        console_printf("[wl] shm_pool.destroy id=%u (fd=%d kept for %u live "
                       "buffer(s); create_buffer refused from now on)\n",
                       (uint64_t)pool->id, (int64_t)pool->pool_fd,
                       (uint64_t)pool->child_buffers);
        emit_display_delete_id(c, pool->id);
        if (pool->child_buffers <= 0) {
            console_printf("[wl] shm_pool.destroy id=%u: no live buffers, object "
                           "released\n", (uint64_t)pool->id);
            (void)memfd_unref(pool->pool_fd);
            log_memfd_live("pool released (no live buffers)");
            free_obj(pool);
        }
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
        destroy_obj(c, surf);
        break;
    case 1u: {                           /* wl_surface.attach(buffer, x, y) */
        const int32_t buffer_id = rb_i32(r);
        const int32_t x = rb_i32(r);
        const int32_t y = rb_i32(r);

        if (r->err) return;
        if (buffer_id != 0) {
            struct wl_obj *b = find_obj(c, (uint32_t)buffer_id);

            if (b == NULL || b->type != OBJ_BUFFER) {
                console_printf("[wl] attach surface=%u refused: buffer %d is not "
                               "a wl_buffer\n", (uint64_t)surf->id,
                               (int64_t)buffer_id);
                surf->buffer_id = 0;
                break;
            }
            if (b->destroyed) {
                console_printf("[wl] attach surface=%u refused: buffer %d was "
                               "destroyed\n", (uint64_t)surf->id,
                               (int64_t)buffer_id);
                surf->buffer_id = 0;
                break;
            }
            b->released = 0;               /* in use again, so a new release is due */
        }
        surf->buffer_id = buffer_id;
        surf->attach_x = x;
        surf->attach_y = y;
        console_printf("[wl] attach surface=%u buffer=%d x=%d y=%d\n",
                       (uint64_t)surf->id, (int64_t)buffer_id, (int64_t)x,
                       (int64_t)y);
        break;
    }
    case 2u:                             /* wl_surface.damage(x,y,w,h) */
    case WL_SURFACE_DAMAGE_BUFFER:       /* wl_surface.damage_buffer(x,y,w,h) */
    {
        const int32_t x = rb_i32(r);
        const int32_t y = rb_i32(r);
        const int32_t w = rb_i32(r);
        const int32_t h = rb_i32(r);

        if (r->err) return;
        /* Accumulate: several damage calls before one commit merge into the
         * bounding box, which is all this server needs (it repaints the whole
         * surface on commit anyway, the rect is protocol evidence + logging). */
        if (w > 0 && h > 0) {
            if (!surf->damage_valid) {
                surf->damage_x = x;
                surf->damage_y = y;
                surf->damage_w = w;
                surf->damage_h = h;
                surf->damage_valid = 1;
            } else {
                const int32_t x0 = surf->damage_x < x ? surf->damage_x : x;
                const int32_t y0 = surf->damage_y < y ? surf->damage_y : y;
                const int32_t x1 = (surf->damage_x + surf->damage_w) > (x + w)
                                       ? (surf->damage_x + surf->damage_w) : (x + w);
                const int32_t y1 = (surf->damage_y + surf->damage_h) > (y + h)
                                       ? (surf->damage_y + surf->damage_h) : (y + h);

                surf->damage_x = x0;
                surf->damage_y = y0;
                surf->damage_w = x1 - x0;
                surf->damage_h = y1 - y0;
            }
        }
        console_printf("[wl] surface.%s surface=%u %d,%d %dx%d (accumulated "
                       "%s%d,%d %dx%d)\n",
                       (opcode == 2u) ? "damage" : "damage_buffer",
                       (uint64_t)surf->id, (int64_t)x, (int64_t)y, (int64_t)w,
                       (int64_t)h, surf->damage_valid ? "" : "none ",
                       (int64_t)surf->damage_x, (int64_t)surf->damage_y,
                       (int64_t)surf->damage_w, (int64_t)surf->damage_h);
        break;
    }
    case WL_SURFACE_SET_OPAQUE_REGION:   /* wl_surface.set_opaque_region(region) */
    case WL_SURFACE_SET_INPUT_REGION:    /* wl_surface.set_input_region(region)  */
    {
        const uint32_t region_id = rb_u32(r);
        struct wl_obj *reg = find_obj(c, region_id);

        if (r->err) return;
        if (region_id != 0u && (reg == NULL || reg->type != OBJ_REGION)) {
            console_printf("[wl] surface %s surface=%u: region %u is not a "
                           "wl_region\n",
                           (opcode == WL_SURFACE_SET_OPAQUE_REGION) ? "opaque"
                                                                     : "input",
                           (uint64_t)surf->id, (uint64_t)region_id);
            return;
        }
        if (opcode == WL_SURFACE_SET_OPAQUE_REGION)
            surf->opaque_region = region_id;
        else
            surf->input_region = region_id;
        console_printf("[wl] surface.%s surface=%u region=%u (area=%d rects=%d)\n",
                       (opcode == WL_SURFACE_SET_OPAQUE_REGION) ? "set_opaque_region"
                                                                : "set_input_region",
                       (uint64_t)surf->id, (uint64_t)region_id,
                       reg ? (int64_t)reg->area : (int64_t)0,
                       reg ? (int64_t)reg->rects : (int64_t)0);
        break;
    }
    case WL_SURFACE_SET_BUFFER_TRANSFORM: /* recorded, not applied */
    {
        const int32_t tr = rb_i32(r);

        if (r->err) return;
        surf->transform = tr;
        console_printf("[wl] surface.set_buffer_transform surface=%u "
                       "transform=%d (recorded, not applied)\n",
                       (uint64_t)surf->id, (int64_t)tr);
        break;
    }
    case WL_SURFACE_SET_BUFFER_SCALE:    /* wl_surface.set_buffer_scale(scale) */
    {
        const int32_t scale = rb_i32(r);

        if (r->err) return;
        if (scale < 1) {
            console_printf("[wl] surface.set_buffer_scale surface=%u: invalid "
                           "scale %d (must be >= 1)\n", (uint64_t)surf->id,
                           (int64_t)scale);
            emit_display_error(c, surf->id, WL_ERR_INVALID_METHOD,
                               "wl_surface.set_buffer_scale needs scale >= 1");
            return;
        }
        surf->scale = scale;
        console_printf("[wl] surface.set_buffer_scale surface=%u scale=%d\n",
                       (uint64_t)surf->id, (int64_t)scale);
        break;
    }
    case WL_SURFACE_OFFSET:              /* wl_surface.offset(x, y) */
    {
        const int32_t x = rb_i32(r);
        const int32_t y = rb_i32(r);

        if (r->err) return;
        surf->off_x = x;
        surf->off_y = y;
        console_printf("[wl] surface.offset surface=%u %d,%d\n",
                       (uint64_t)surf->id, (int64_t)x, (int64_t)y);
        break;
    }
    case 3u: {                           /* wl_surface.frame(new_id) */
        const uint32_t id = rb_u32(r);
        struct wl_obj *cb;

        if (r->err) return;
        cb = add_obj(c, id, OBJ_CALLBACK);
        if (cb == NULL) return;
        cb->cb_done = 0;
        surf->frame_cb_id = id;
        console_printf("[wl] surface.frame surface=%u callback=%u (done on the "
                       "next commit)\n", (uint64_t)surf->id, (uint64_t)id);
        break;
    }
    case WL_SURFACE_COMMIT:              /* wl_surface.commit */
        commit_surface(c, surf);
        break;
    default:
        console_printf("[wl] wl_surface: unsupported opcode %u\n",
                       (uint64_t)opcode);
        emit_display_error(c, surf->id, WL_ERR_INVALID_METHOD,
                           "wl_surface request is not implemented by this server");
        break;
    }
}

/* wl_region: rectangles are recorded, not composited (this server has no
 * clipping path).  The running totals are what the commit line and the
 * acceptance verify: add grows the area, subtract shrinks it. */
static void dispatch_region(struct wl_conn *c, struct wl_obj *reg,
                            uint16_t opcode, struct rbuf *r)
{
    if (opcode == 0u) {                  /* wl_region.destroy */
        console_printf("[wl] region.destroy id=%u (rects=%d subs=%d area=%d)\n",
                       (uint64_t)reg->id, (int64_t)reg->rects,
                       (int64_t)reg->sub_rects, (int64_t)reg->area);
        destroy_obj(c, reg);
        return;
    }
    if (opcode == 1u || opcode == 2u) {  /* add / subtract (x, y, w, h) */
        const int32_t x = rb_i32(r);
        const int32_t y = rb_i32(r);
        const int32_t w = rb_i32(r);
        const int32_t h = rb_i32(r);
        const int32_t area = (w > 0 && h > 0) ? (w * h) : 0;

        if (r->err) return;
        if (opcode == 1u) {
            const int32_t x1 = (reg->rects == 0) ? x + w
                                                 : ((reg->r_x + reg->r_w > x + w)
                                                        ? reg->r_x + reg->r_w : x + w);
            const int32_t y1 = (reg->rects == 0) ? y + h
                                                 : ((reg->r_y + reg->r_h > y + h)
                                                        ? reg->r_y + reg->r_h : y + h);
            const int32_t x0 = (reg->rects == 0) ? x
                                                 : (reg->r_x < x ? reg->r_x : x);
            const int32_t y0 = (reg->rects == 0) ? y
                                                 : (reg->r_y < y ? reg->r_y : y);

            reg->rects++;
            reg->area += area;
            reg->r_x = x0;
            reg->r_y = y0;
            reg->r_w = x1 - x0;
            reg->r_h = y1 - y0;
        } else {
            reg->sub_rects++;
            reg->area -= area;
            if (reg->area < 0) reg->area = 0;
        }
        console_printf("[wl] region id=%u %s %d,%d %dx%d -> rects=%d subs=%d "
                       "area=%d bbox=%d,%d %dx%d\n", (uint64_t)reg->id,
                       (opcode == 1u) ? "add" : "subtract", (int64_t)x,
                       (int64_t)y, (int64_t)w, (int64_t)h, (int64_t)reg->rects,
                       (int64_t)reg->sub_rects, (int64_t)reg->area,
                       (int64_t)reg->r_x, (int64_t)reg->r_y, (int64_t)reg->r_w,
                       (int64_t)reg->r_h);
        return;
    }
    console_printf("[wl] wl_region(id=%u): unsupported opcode %u\n",
                   (uint64_t)reg->id, (uint64_t)opcode);
    emit_display_error(c, reg->id, WL_ERR_INVALID_METHOD,
                       "wl_region request is not implemented by this server");
}

/* wl_subcompositor: get_subsurface(child_surface, parent_surface).  The link is
 * recorded and exported; pixels are not composited (no compositor yet). */
static void dispatch_subcompositor(struct wl_conn *c, struct wl_obj *comp,
                                   uint16_t opcode, struct rbuf *r)
{
    if (opcode == 0u) {                  /* get_subsurface(new_id, surface, parent) */
        const uint32_t new_id = rb_u32(r);
        const uint32_t surface_id = rb_u32(r);
        const uint32_t parent_id = rb_u32(r);
        struct wl_obj *child, *parent, *sub;
        int i, slot = -1;

        if (r->err) return;
        child = find_obj(c, surface_id);
        parent = find_obj(c, parent_id);
        if (child == NULL || child->type != OBJ_SURFACE ||
            parent == NULL || parent->type != OBJ_SURFACE) {
            console_printf("[wl] get_subsurface: surface=%u parent=%u: both must "
                           "be live wl_surface objects\n", (uint64_t)surface_id,
                           (uint64_t)parent_id);
            emit_display_error(c, comp->id, WL_ERR_INVALID_METHOD,
                               "get_subsurface needs two live wl_surface objects");
            return;
        }
        sub = add_obj(c, new_id, OBJ_SUBSURFACE);
        if (sub == NULL) return;
        sub->version = comp->version;
        sub->child_id = surface_id;
        sub->parent_id = parent_id;
        sub->pos_x = 0;
        sub->pos_y = 0;
        sub->sync_mode = 1;              /* subsurfaces start in sync mode */
        child->parent_sub_id = new_id;
        for (i = 0; i < (int)(sizeof g_subsurfaces / sizeof g_subsurfaces[0]); i++) {
            if (g_subsurfaces[i] == NULL) { slot = i; break; }
        }
        if (slot >= 0) g_subsurfaces[slot] = sub;
        console_printf("[wl] subcompositor.get_subsurface id=%u surface=%u "
                       "parent=%u pos=0,0 sync=1\n", (uint64_t)new_id,
                       (uint64_t)surface_id, (uint64_t)parent_id);
        return;
    }
    if (opcode == 1u) {                  /* wl_subcompositor.destroy */
        console_printf("[wl] subcompositor.destroy id=%u\n", (uint64_t)comp->id);
        destroy_obj(c, comp);
        return;
    }
    console_printf("[wl] wl_subcompositor(id=%u): unsupported opcode %u\n",
                   (uint64_t)comp->id, (uint64_t)opcode);
    emit_display_error(c, comp->id, WL_ERR_INVALID_METHOD,
                       "wl_subcompositor request is not implemented");
}

static void sub_forget(struct wl_obj *sub)
{
    int i;

    for (i = 0; i < (int)(sizeof g_subsurfaces / sizeof g_subsurfaces[0]); i++) {
        if (g_subsurfaces[i] == sub) g_subsurfaces[i] = NULL;
    }
}

static void dispatch_subsurface(struct wl_conn *c, struct wl_obj *sub,
                                uint16_t opcode, struct rbuf *r)
{
    switch (opcode) {
    case 0u:                             /* wl_subsurface.destroy */
        console_printf("[wl] subsurface.destroy id=%u surface=%u parent=%u "
                       "pos=%d,%d\n", (uint64_t)sub->id,
                       (uint64_t)sub->child_id, (uint64_t)sub->parent_id,
                       (int64_t)sub->pos_x, (int64_t)sub->pos_y);
        sub_forget(sub);
        destroy_obj(c, sub);
        return;
    case 1u: {                           /* set_position(x, y) */
        const int32_t x = rb_i32(r);
        const int32_t y = rb_i32(r);

        if (r->err) return;
        sub->pos_x = x;
        sub->pos_y = y;
        console_printf("[wl] subsurface.set_position id=%u surface=%u parent=%u "
                       "pos=%d,%d\n", (uint64_t)sub->id,
                       (uint64_t)sub->child_id, (uint64_t)sub->parent_id,
                       (int64_t)x, (int64_t)y);
        return;
    }
    case 2u:                             /* place_above(sibling) */
    case 3u: {                           /* place_below(sibling) */
        const uint32_t sibling = rb_u32(r);

        if (r->err) return;
        sub->place_below = (opcode == 3u) ? 1 : 0;
        console_printf("[wl] subsurface.%s id=%u sibling=%u (recorded)\n",
                       (opcode == 3u) ? "place_below" : "place_above",
                       (uint64_t)sub->id, (uint64_t)sibling);
        return;
    }
    case 4u:                             /* set_sync */
        sub->sync_mode = 1;
        console_printf("[wl] subsurface.set_sync id=%u surface=%u\n",
                       (uint64_t)sub->id, (uint64_t)sub->child_id);
        return;
    case 5u:                             /* set_desync */
        sub->sync_mode = 0;
        console_printf("[wl] subsurface.set_desync id=%u surface=%u\n",
                       (uint64_t)sub->id, (uint64_t)sub->child_id);
        return;
    default:
        console_printf("[wl] wl_subsurface(id=%u): unsupported opcode %u\n",
                       (uint64_t)sub->id, (uint64_t)opcode);
        emit_display_error(c, sub->id, WL_ERR_INVALID_METHOD,
                           "wl_subsurface request is not implemented");
        return;
    }
}

static void dispatch_seat(struct wl_conn *c, struct wl_obj *seat,
                          uint16_t opcode, struct rbuf *r)
{
    const uint32_t id = rb_u32(r);

    if (r->err) return;
    if (opcode == 0u) {                  /* wl_seat.get_pointer(new_id) */
        struct wl_obj *p = add_obj(c, id, OBJ_POINTER);

        if (p == NULL) return;
        p->pointer_sent = 0;
        p->version = seat->version;
        console_printf("[wl] seat.get_pointer id=%u (real wl_pointer stream: "
                       "enter/motion/button from the kernel mouse, frame=%s)\n",
                       (uint64_t)id,
                       (seat->version >= 5u) ? "yes" : "no (bound < v5)");
        return;
    }
    if (opcode == 1u) {                  /* wl_seat.get_keyboard(new_id) */
        struct wl_obj *kb = add_obj(c, id, OBJ_KEYBOARD);

        if (kb == NULL) return;
        kb->version = seat->version;
        kb->entered_surface = 0;
        /* A fresh client starts from a fresh key stream: drop whatever the
         * mirror ring collected before it asked for a keyboard (boot-time key
         * activity, the shell's own typing). */
        mbos_kbd_mirror_reset();
        console_printf("[wl] seat.get_keyboard id=%u (real wl_keyboard: evdev "
                       "keycodes from the kernel's decoded scancode mirror; "
                       "keymap=NO_KEYMAP)\n", (uint64_t)id);
        return;
    }
    if (opcode == 2u) {                  /* wl_seat.release */
        console_printf("[wl] seat.release id=%u\n", (uint64_t)seat->id);
        destroy_obj(c, seat);
        return;
    }
    console_printf("[wl] wl_seat(id=%u): unsupported opcode %u\n",
                   (uint64_t)seat->id, (uint64_t)opcode);
    emit_display_error(c, seat->id, WL_ERR_INVALID_METHOD,
                       "wl_seat request is not implemented by this server");
}

/* wl_pointer requests: this server does not track a cursor, so set_cursor is
 * accepted and ignored (the client is allowed to send it), release destroys the
 * object like any other. */
static void dispatch_pointer(struct wl_conn *c, struct wl_obj *ptr,
                             uint16_t opcode, struct rbuf *r)
{
    if (opcode == 0u) {                  /* wl_pointer.set_cursor(serial, ...) */
        const uint32_t serial = rb_u32(r);

        if (r->err) return;
        console_printf("[wl] pointer.set_cursor id=%u serial=%u (no cursor "
                       "surface on this server)\n", (uint64_t)ptr->id,
                       (uint64_t)serial);
        return;
    }
    if (opcode == 1u) {                  /* wl_pointer.release */
        /* enter and leave are paired: a client that entered the surface always
         * sees the leave before the object goes away. */
        if (ptr->pointer_sent && ptr->entered_surface != 0u) {
            emit_pointer_leave(c, ptr->id, ++g_serial, ptr->entered_surface);
            console_printf("[wl] pointer leave id=%u surface=%u serial=%u\n",
                           (uint64_t)ptr->id, (uint64_t)ptr->entered_surface,
                           (uint64_t)g_serial);
            if (ptr->version >= 5u) emit_pointer_frame(c, ptr->id);
            ptr->pointer_sent = 0;
            ptr->entered_surface = 0;
            c->ptr_events++;
            g_ptr_events++;
        }
        console_printf("[wl] pointer.release id=%u\n", (uint64_t)ptr->id);
        destroy_obj(c, ptr);
        return;
    }
    console_printf("[wl] wl_pointer(id=%u): unsupported opcode %u\n",
                   (uint64_t)ptr->id, (uint64_t)opcode);
    emit_display_error(c, ptr->id, WL_ERR_INVALID_METHOD,
                       "wl_pointer request is not implemented by this server");
}

/* Send + log wl_keyboard.modifiers for one keyboard object. */
static void send_kb_modifiers(struct wl_conn *c, struct wl_obj *o, uint32_t mods,
                              const char *why)
{
    g_serial++;
    emit_kb_modifiers(c, o->id, g_serial, (mods & MBOS_KBD_SHIFT) ? 1u : 0u,
                      (mods & MBOS_KBD_CAPS_BIT) ? 1u : 0u);
    o->last_mods = mods;
    console_printf("[wl] keyboard modifiers id=%u serial=%u shift=%u ctrl=%u "
                   "alt=%u caps=%u (%s)\n", (uint64_t)o->id, (uint64_t)g_serial,
                   (uint64_t)((mods & MBOS_KBD_SHIFT) ? 1u : 0u),
                   (uint64_t)((mods & MBOS_KBD_CTRL) ? 1u : 0u),
                   (uint64_t)((mods & MBOS_KBD_ALT) ? 1u : 0u),
                   (uint64_t)((mods & MBOS_KBD_CAPS_BIT) ? 1u : 0u), why);
}

/* ===================== input pump =====================
 * Two real event streams, sourced from the kernel's PS/2 devices:
 *
 *   wl_keyboard: kernel/drivers/mb_kbd.c mirrors every decoded scancode into a
 *     ring.  The pump first drains the PS/2 controller into the decoder
 *     (mbos_kbd_poll -> mbos_kbd_decode, the same path the shell uses), then
 *     turns each mirrored make/break code into wl_keyboard.key with state 1/0
 *     and an evdev keycode (set-1 scancode + 8, the standard mapping).
 *
 *   wl_pointer: mbos_mouse_poll() updates the kernel's pointer state; the pump
 *     then emits enter once, motion whenever the position changed, button on
 *     both edges (0x110 left / 0x111 right / 0x112 middle) and one
 *     wl_pointer.frame per group when the client bound the pointer at >= 5.
 *
 * Crucially the PS/2 ports are only drained when *this* connection asked for
 * that capability.  A client without a wl_keyboard (e.g. `run wlclient`) can
 * therefore not swallow keys the shell or the GUI is waiting for.
 *
 * Placement (v0.61): the streams are *window-relative*.  The desktop shell
 * reports the Wayland window rectangle through suture_gui_wayland_rect() and the
 * server converts the kernel's screen-space pointer position into surface-local
 * coordinates, ((screen - window origin) / buffer scale).  enter/leave follow the
 * window boundary and motion is only sent inside it; the keyboard stream is gated
 * on suture_gui_wayland_focused() so typing into another window cannot leak into
 * the client.  With no window reported (terminal-only run, F2 window closed) the
 * streams degrade to the original whole-surface semantics, which is what the
 * older acceptance scripts were written against. */

/* Re-read the window rectangle; logs only when it changes.  `allow_default` is
 * set on the live-connection path: a client running in the terminal has no shell
 * window, so its surface is treated as the Wayland window at the documented
 * default geometry.  The desktop pump passes 0 (no window on screen, nothing to
 * place).  Returns 1 with the rectangle filled. */
static int wayland_window(int allow_default, int *x, int *y, int *w, int *h)
{
    int nx = 0, ny = 0, nw = 0, nh = 0;
    int from_shell = 0;

    if (suture_gui_wayland_rect != NULL &&
        suture_gui_wayland_rect(&nx, &ny, &nw, &nh) == 1 && nw > 0 && nh > 0) {
        from_shell = 1;
    } else if (allow_default) {
        nx = WL_WIN_DEF_X;
        ny = WL_WIN_DEF_Y;
        nw = WL_WIN_DEF_W;
        nh = WL_WIN_DEF_H;
    } else {
        if (g_win_ok) {
            g_win_ok = 0;
            console_printf("[wl] wayland window closed\n");
        }
        g_win_from_shell = 0;
        return 0;
    }

    if (!g_win_ok || g_win_from_shell != from_shell || nx != g_win_x ||
        ny != g_win_y || nw != g_win_w || nh != g_win_h) {
        g_win_changes++;
        console_printf("[wl] wayland window %d,%d %dx%d (source=%s, screen -> "
                       "surface-local mapping active, change=%u)\n", (int64_t)nx,
                       (int64_t)ny, (int64_t)nw, (int64_t)nh,
                       from_shell ? "shell" : "default",
                       (uint64_t)g_win_changes);
    }
    g_win_ok = 1;
    g_win_from_shell = from_shell;
    g_win_x = nx;
    g_win_y = ny;
    g_win_w = nw;
    g_win_h = nh;
    if (x) *x = g_win_x;
    if (y) *y = g_win_y;
    if (w) *w = g_win_w;
    if (h) *h = g_win_h;
    return 1;
}

/* Keyboard focus for one pump.  While the shell owns the input devices its answer
 * is authoritative (and drives the gate); without a shell window the client in the
 * terminal is the only keyboard consumer, so keys keep flowing (pre-0.61
 * behaviour). */
static int window_focused(void)
{
    if (!g_win_from_shell) return 1;
    return suture_gui_wayland_focused() != 0;
}

/* Desktop heartbeat: one line per state change and every ~2 s otherwise, so a
 * 60 fps shell cannot flood the serial log. */
static void desktop_beat(int has_win, int x, int y, int lx, int ly, int inside,
                         int force)
{
    if (!force && (g_desk_calls % 120) != 0) return;
    console_printf("[wl] desktop pump calls=%u events=%u keys=%u gated=%u "
                   "window=%s%d,%d %dx%d pointer=%d,%d local=%d,%d inside=%d\n",
                   (uint64_t)g_desk_calls, (uint64_t)g_desk_events,
                   (uint64_t)g_desk_keys, (uint64_t)g_kb_gated,
                   has_win ? "" : "none ", (int64_t)g_win_x, (int64_t)g_win_y,
                   (int64_t)g_win_w, (int64_t)g_win_h, (int64_t)x, (int64_t)y,
                   (int64_t)lx, (int64_t)ly, (int64_t)(inside ? 1 : 0));
    g_desk_last_beat_calls = g_desk_calls;
}

/* ===================== desktop-side input pump (v0.61) =====================
 * Called once per frame by the Rust desktop shell.  There is no live connection
 * while the shell owns the screen, so nothing can be written to a socket; what
 * still matters is *placement*: the window is showing the retained surface of the
 * last commit, so the server maps the pointer/keyboard onto that surface with
 * exactly the same rules as the live path and records the result.  That is what
 * the window-placement acceptance checks, and it is the code a multi-tasking
 * client reuses unchanged. */
void wl_server_desktop_pump(void)
{
    int i, ci, has_win, mx, my, buttons, scale, sw, sh, inside, lx, ly;
    int wx = 0, wy = 0, ww = 0, wh = 0;
    int keys = 0, changed = 0;
    uint8_t sc_byte;
    uint32_t tick;
    struct wl_keep *k;

    /* Exited clients must not look alive: prune their slots first. */
    /* v0.62: while the desktop shell owns the screen no ring-3 client can be
     * pumping (the shell is the single task), so a slot that is still marked
     * connected is stale.  Reclaiming it here is also where the memfd references
     * the client held are dropped (ring-3 exit does not report EOF on the unix
     * peer in this kernel, so conn_peer_dead alone never fires). */
    for (i = 0; i < WL_SERVER_MAX_CONNS; i++) {
        if (!g_conns[i].used) continue;
        if (!conn_peer_dead(&g_conns[i]))
            console_printf("[wl] desktop pump: reclaiming stale connection slot "
                           "%d (no client can own the input devices now)\n",
                           (int64_t)i);
        conn_release(&g_conns[i]);
    }
    if (!g_started) return;

    /* v0.61: decide ownership by what owns the *input devices*. While the desktop
     * window is on screen the shell owns them and no client can be pumping (this
     * kernel runs the GUI in the shell thread), so the desktop face runs even when
     * a dead client's slot was left behind by the ring-3 fd teardown. Without a
     * shell window a live connection owns both streams: its syscalls pump them. */
    has_win = wayland_window(0, &wx, &wy, &ww, &wh);
    if (!has_win) {
        for (i = 0; i < WL_SERVER_MAX_CONNS; i++)
            if (g_conns[i].used) return;
    }

    ci = g_latest_keep;
    if (ci < 0 || ci >= WL_SERVER_MAX_CONNS) return;
    k = &g_keep[ci];
    if (!k->used) return;

    g_desk_calls++;

    /* Keys: the GUI consumes the real PS/2 stream, so nothing is taken from the
     * controller here; the decoder mirrors every scancode it sees into the ring
     * and this only drains the mirror to count/gate them. */
    while (mbos_kbd_mirror_pop(&sc_byte, &tick)) {
        if (sc_byte == 0xE0u) continue;
        keys++;
    }

    if (has_win) {
        const int focused = window_focused();

        if (focused != g_desk_focused) {
            console_printf("[wl] keyboard focus %d\n", (int64_t)(focused ? 1 : 0));
            g_desk_focused = focused;
            changed = 1;
        }
        if (keys) {
            if (focused) {
                g_desk_keys += (uint32_t)keys;
                console_printf("[wl] keyboard key delivered (desktop) count=%d "
                               "total=%u\n", (int64_t)keys, (uint64_t)g_desk_keys);
            } else {
                g_kb_gated += (uint32_t)keys;
                console_printf("[wl] keyboard key gated (window not focused) "
                               "gated=%u\n", (uint64_t)g_kb_gated);
            }
            changed = 1;
        }
    }

    if (!has_win) {
        if (g_desk_inside) {
            g_desk_inside = 0;
            g_desk_lx = g_desk_ly = -1;
            console_printf("[wl] pointer leave id=0 surface=%u serial=%u "
                           "(left the wayland window)\n",
                           (uint64_t)k->surface_id, (uint64_t)++g_serial);
            g_ptr_leaves++;
            g_desk_events++;
            changed = 1;
        }
        desktop_beat(0, 0, 0, 0, 0, 0, changed);
        return;
    }

    mbos_mouse_poll();
    mx = mbos_mouse_x();
    my = mbos_mouse_y();
    buttons = mbos_mouse_buttons();

    scale = (k->scale >= 1) ? k->scale : 1;
    sw = k->w / scale;
    sh = k->h / scale;
    if (sw < 1) sw = 1;
    if (sh < 1) sh = 1;

    inside = (mx >= wx && mx < wx + ww && my >= wy && my < wy + wh);
    lx = (mx - wx) / scale;
    ly = (my - wy) / scale;
    if (lx < 0) lx = 0;
    if (ly < 0) ly = 0;
    if (lx >= sw) lx = sw - 1;
    if (ly >= sh) ly = sh - 1;

    if (!inside) {
        if (g_desk_inside) {
            g_desk_inside = 0;
            g_desk_lx = g_desk_ly = -1;
            g_desk_buttons = buttons;
            console_printf("[wl] pointer leave id=0 surface=%u serial=%u "
                           "(left the wayland window)\n",
                           (uint64_t)k->surface_id, (uint64_t)++g_serial);
            g_ptr_leaves++;
            g_desk_events += 2;
            changed = 1;
        }
        desktop_beat(1, mx, my, lx, ly, 0, changed);
        return;
    }

    g_serial++;
    if (!g_desk_inside) {
        g_desk_inside = 1;
        console_printf("[wl] pointer enter id=0 surface=%u %d,%d (kernel mouse "
                       "%d,%d serial=%u)\n", (uint64_t)k->surface_id, (int64_t)lx,
                       (int64_t)ly, (int64_t)mx, (int64_t)my, (uint64_t)g_serial);
        console_printf("[wl] pointer motion id=0 time=%u %d,%d\n",
                       (uint64_t)(time_source_now_ns() / 1000000ull),
                       (int64_t)lx, (int64_t)ly);
        console_printf("[wl] pointer window surface=%u window=%d,%d %dx%d "
                       "local=%d,%d scale=%d kernel=%d,%d\n",
                       (uint64_t)k->surface_id, (int64_t)wx, (int64_t)wy,
                       (int64_t)ww, (int64_t)wh, (int64_t)lx, (int64_t)ly,
                       (int64_t)scale, (int64_t)mx, (int64_t)my);
        g_desk_lx = lx;
        g_desk_ly = ly;
        g_desk_buttons = buttons;
        g_desk_events += 2;
        changed = 1;
    } else if (lx != g_desk_lx || ly != g_desk_ly) {
        console_printf("[wl] pointer motion id=0 time=%u %d,%d\n",
                       (uint64_t)(time_source_now_ns() / 1000000ull),
                       (int64_t)lx, (int64_t)ly);
        console_printf("[wl] pointer window surface=%u window=%d,%d %dx%d "
                       "local=%d,%d scale=%d kernel=%d,%d\n",
                       (uint64_t)k->surface_id, (int64_t)wx, (int64_t)wy,
                       (int64_t)ww, (int64_t)wh, (int64_t)lx, (int64_t)ly,
                       (int64_t)scale, (int64_t)mx, (int64_t)my);
        g_desk_lx = lx;
        g_desk_ly = ly;
        g_desk_events++;
        changed = 1;
    }

    if (buttons != g_desk_buttons) {
        static const int bit[3] = { 1, 2, 4 };
        static const uint32_t code[3] = { 0x110u, 0x111u, 0x112u };
        int b;

        for (b = 0; b < 3; b++) {
            const int was = (g_desk_buttons & bit[b]) ? 1 : 0;
            const int now = (buttons & bit[b]) ? 1 : 0;

            if (was == now) continue;
            console_printf("[wl] pointer button id=0 button=0x%x state=%u\n",
                           (uint64_t)code[b], (uint64_t)(now ? 1u : 0u));
            g_desk_events++;
        }
        g_desk_buttons = buttons;
        changed = 1;
    }

    desktop_beat(1, mx, my, lx, ly, inside, changed);
}

static void input_pump(struct wl_conn *c)
{
    int i;
    int has_kb = 0;
    int has_ptr = 0;
    int has_win;
    int wx = 0, wy = 0, ww = 0, wh = 0;
    uint32_t time_ms;

    for (i = 0; i < WL_SERVER_MAX_OBJECTS; i++) {
        if (c->objs[i].type == OBJ_KEYBOARD) has_kb = 1;
        if (c->objs[i].type == OBJ_POINTER) has_ptr = 1;
    }
    if (!has_kb && !has_ptr) return;

    time_ms = (uint32_t)(time_source_now_ns() / 1000000ull);

    /* Placement is re-read once per pump: the shell can move the window (title
     * bar drag) between two events and the local coordinates must follow it. */
    has_win = wayland_window(1, &wx, &wy, &ww, &wh);

    /* ---------------- wl_keyboard ---------------- */
    if (has_kb) {
        int sc;
        uint8_t mirored_sc;
        uint32_t tick;
        const int focused = has_win ? window_focused() : 1;

        if (focused != g_kb_focused) {
            console_printf("[wl] keyboard focus %d\n", (int64_t)(focused ? 1 : 0));
            g_kb_focused = focused;
            if (!focused) {
                /* Focus lost: leave every surface this connection had entered. */
                for (i = 0; i < WL_SERVER_MAX_OBJECTS; i++) {
                    struct wl_obj *o = &c->objs[i];

                    if (o->type != OBJ_KEYBOARD || o->entered_surface == 0u)
                        continue;
                    g_serial++;
                    emit_kb_leave(c, o->id, g_serial, o->entered_surface);
                    console_printf("[wl] keyboard leave id=%u surface=%u serial=%u "
                                   "(window lost focus)\n", (uint64_t)o->id,
                                   (uint64_t)o->entered_surface,
                                   (uint64_t)g_serial);
                    o->entered_surface = 0;
                }
            }
        }

        while ((sc = mbos_kbd_poll()) >= 0)
            (void)mbos_kbd_decode((uint8_t)sc);          /* feeds the mirror */

        while (mbos_kbd_mirror_pop(&mirored_sc, &tick)) {
            /* E0-prefixed keys arrive as two codes; skip the prefix byte itself
             * (it has no key of its own) and forward the rest. */
            if (mirored_sc == 0xE0u) continue;
            if (!focused) {
                /* Focus gate: this key belongs to another window.  Drop it, count
                 * it and say so without flooding the log. */
                g_kb_gated++;
                if (g_kb_gated == 1u || (g_kb_gated % 8u) == 0u)
                    console_printf("[wl] keyboard key gated (window not focused) "
                                   "gated=%u\n", (uint64_t)g_kb_gated);
                continue;
            }
            for (i = 0; i < WL_SERVER_MAX_OBJECTS; i++) {
                struct wl_obj *o = &c->objs[i];
                const uint32_t state = MBOS_KBD_IS_RELEASE(mirored_sc)
                                           ? WL_KB_KEY_STATE_RELEASED
                                           : WL_KB_KEY_STATE_PRESSED;
                const uint32_t key = (uint32_t)(mirored_sc & 0x7Fu) + 8u;
                uint32_t mods;

                if (o->type != OBJ_KEYBOARD) continue;
                if (o->entered_surface == 0u) continue;   /* not entered yet */
                g_serial++;
                emit_kb_key(c, o->id, g_serial, tick, key, state);
                g_kb_events++;
                c->kb_events++;
                console_printf("[wl] keyboard key id=%u serial=%u time=%u "
                               "key=%u (sc=0x%x) state=%u\n", (uint64_t)o->id,
                               (uint64_t)g_serial, (uint64_t)tick, (uint64_t)key,
                               (uint64_t)mirored_sc, (uint64_t)state);
                mods = mbos_kbd_mod_state();
                if (mods != o->last_mods) {
                    send_kb_modifiers(c, o, mods, "changed");
                }
            }
        }

        /* keyboard enter: needs the window focus and a surface this connection
         * committed.  The keymap (a real XKB memfd) goes out ahead of enter. */
        if (focused) {
            for (i = 0; i < WL_SERVER_MAX_OBJECTS; i++) {
                struct wl_obj *o = &c->objs[i];
                const int ci = (int)(c - g_conns);
                const struct wl_keep *k;

                if (o->type != OBJ_KEYBOARD || o->entered_surface != 0u) continue;
                if (ci < 0 || ci >= WL_SERVER_MAX_CONNS) continue;
                k = &g_keep[ci];
                if (!k->used) continue;
                if (!o->keymap_sent) {
                    emit_kb_keymap(c, o->id);        /* format 1 + memfd descriptor */
                    o->keymap_sent = 1;
                    g_keymap_users++;
                    if (o->version >= 4u) emit_kb_repeat_info(c, o->id);
                }
                g_serial++;
                emit_kb_enter(c, o->id, g_serial, k->surface_id);
                o->entered_surface = k->surface_id;
                console_printf("[wl] keyboard enter id=%u surface=%u serial=%u "
                               "(keymap=xkb_v1, evdev keycodes)\n",
                               (uint64_t)o->id, (uint64_t)k->surface_id,
                               (uint64_t)g_serial);
                send_kb_modifiers(c, o, mbos_kbd_mod_state(), "enter");
            }
        }
    }

    /* ---------------- wl_pointer ---------------- */
    if (has_ptr) {
        int mx, my, buttons;

        mbos_mouse_poll();                 /* drain the PS/2 aux port */
        mx = mbos_mouse_x();
        my = mbos_mouse_y();
        buttons = mbos_mouse_buttons();

        for (i = 0; i < WL_SERVER_MAX_OBJECTS; i++) {
            struct wl_obj *o = &c->objs[i];
            const int ci = (int)(c - g_conns);
            const struct wl_keep *k;
            int scale, sw, sh, inside, lx, ly, changed, btn_changed;

            if (o->type != OBJ_POINTER) continue;
            if (ci < 0 || ci >= WL_SERVER_MAX_CONNS) continue;
            k = &g_keep[ci];
            if (!k->used) continue;        /* need this client's own surface */

            /* wl_pointer coordinates are surface-local (logical): the window
             * offset divided by the buffer scale. */
            scale = (k->scale >= 1) ? k->scale : 1;
            sw = k->w / scale;
            sh = k->h / scale;
            if (sw < 1) sw = 1;
            if (sh < 1) sh = 1;

            if (has_win) {
                inside = (mx >= wx && mx < wx + ww && my >= wy && my < wy + wh);
                lx = (mx - wx) / scale;
                ly = (my - wy) / scale;
            } else {
                inside = 1;                /* no window: whole surface, as before */
                lx = mx;
                ly = my;
            }
            if (lx < 0) lx = 0;
            if (ly < 0) ly = 0;
            if (lx >= sw) lx = sw - 1;
            if (ly >= sh) ly = sh - 1;

            /* Outside the window: the pointer has left this surface. */
            if (!inside) {
                if (o->pointer_sent) {
                    g_serial++;
                    emit_pointer_leave(c, o->id, g_serial, o->entered_surface);
                    console_printf("[wl] pointer leave id=%u surface=%u serial=%u "
                                   "(left the wayland window)\n",
                                   (uint64_t)o->id, (uint64_t)o->entered_surface,
                                   (uint64_t)g_serial);
                    if (o->version >= 5u) emit_pointer_frame(c, o->id);
                    o->pointer_sent = 0;
                    o->entered_surface = 0;
                    o->last_buttons = buttons;
                    g_ptr_leaves++;
                    c->ptr_events += 2;
                    g_ptr_events += 2;
                }
                continue;
            }

            if (!o->pointer_sent) {
                g_serial++;
                emit_pointer_enter(c, o->id, g_serial, k->surface_id, lx, ly);
                console_printf("[wl] pointer enter id=%u surface=%u %d,%d "
                               "(kernel mouse %d,%d serial=%u)\n",
                               (uint64_t)o->id, (uint64_t)k->surface_id,
                               (int64_t)lx, (int64_t)ly, (int64_t)mx, (int64_t)my,
                               (uint64_t)g_serial);
                emit_pointer_motion(c, o->id, time_ms, lx, ly);
                console_printf("[wl] pointer motion id=%u time=%u %d,%d\n",
                               (uint64_t)o->id, (uint64_t)time_ms, (int64_t)lx,
                               (int64_t)ly);
                if (has_win)
                    console_printf("[wl] pointer window surface=%u window=%d,%d "
                                   "%dx%d local=%d,%d scale=%d kernel=%d,%d\n",
                                   (uint64_t)k->surface_id, (int64_t)wx,
                                   (int64_t)wy, (int64_t)ww, (int64_t)wh,
                                   (int64_t)lx, (int64_t)ly, (int64_t)scale,
                                   (int64_t)mx, (int64_t)my);
                o->pointer_sent = 1;
                o->entered_surface = k->surface_id;
                o->last_x = lx;
                o->last_y = ly;
                o->last_buttons = buttons;
                c->ptr_events += 2;
                g_ptr_events += 2;
                if (o->version >= 5u) emit_pointer_frame(c, o->id);
                continue;
            }

            changed = (lx != o->last_x || ly != o->last_y);
            btn_changed = (buttons != o->last_buttons);

            if (changed) {
                emit_pointer_motion(c, o->id, time_ms, lx, ly);
                console_printf("[wl] pointer motion id=%u time=%u %d,%d\n",
                               (uint64_t)o->id, (uint64_t)time_ms, (int64_t)lx,
                               (int64_t)ly);
                if (has_win)
                    console_printf("[wl] pointer window surface=%u window=%d,%d "
                                   "%dx%d local=%d,%d scale=%d kernel=%d,%d\n",
                                   (uint64_t)k->surface_id, (int64_t)wx,
                                   (int64_t)wy, (int64_t)ww, (int64_t)wh,
                                   (int64_t)lx, (int64_t)ly, (int64_t)scale,
                                   (int64_t)mx, (int64_t)my);
                o->last_x = lx;
                o->last_y = ly;
                c->ptr_events++;
                g_ptr_events++;
            }
            if (btn_changed) {
                /* Both edges, per button: BTN_LEFT 0x110, BTN_RIGHT 0x111,
                 * BTN_MIDDLE 0x112 (evdev codes), state 1 = pressed. */
                static const int bit[3] = { 1, 2, 4 };
                static const uint32_t code[3] = { 0x110u, 0x111u, 0x112u };
                int b;

                for (b = 0; b < 3; b++) {
                    const int was = (o->last_buttons & bit[b]) ? 1 : 0;
                    const int now = (buttons & bit[b]) ? 1 : 0;

                    if (was == now) continue;
                    g_serial++;
                    emit_pointer_button(c, o->id, g_serial, time_ms, code[b],
                                        now ? 1u : 0u);
                    console_printf("[wl] pointer button id=%u button=0x%x "
                                   "state=%u\n", (uint64_t)o->id,
                                   (uint64_t)code[b], (uint64_t)(now ? 1u : 0u));
                    c->ptr_events++;
                    g_ptr_events++;
                }
                o->last_buttons = buttons;
            }
            if ((changed || btn_changed) && o->version >= 5u)
                emit_pointer_frame(c, o->id);
        }
    }
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
        /* Protocol answer instead of a silent drop: the client gets a real
         * wl_display.error naming the offending id. */
        emit_display_error(c, obj_id, WL_ERR_INVALID_OBJECT, "no such object");
        return;
    }
    switch (o->type) {
    case OBJ_DISPLAY:     dispatch_display(c, opcode, &r); break;
    case OBJ_REGISTRY:    dispatch_registry(c, opcode, &r); break;
    case OBJ_COMPOSITOR:  dispatch_compositor(c, opcode, &r); break;
    case OBJ_SHM:         dispatch_shm(c, opcode, &r); break;
    case OBJ_POOL:        dispatch_pool(c, o, opcode, &r); break;
    case OBJ_SURFACE:     dispatch_surface(c, o, opcode, &r); break;
    case OBJ_REGION:      dispatch_region(c, o, opcode, &r); break;
    case OBJ_SUBCOMPOSITOR: dispatch_subcompositor(c, o, opcode, &r); break;
    case OBJ_SUBSURFACE:  dispatch_subsurface(c, o, opcode, &r); break;
    case OBJ_SEAT:        dispatch_seat(c, o, opcode, &r); break;
    case OBJ_POINTER:     dispatch_pointer(c, o, opcode, &r); break;
    case OBJ_BUFFER:
        if (opcode == 0u) {
            struct wl_obj *pool = find_obj(c, (uint32_t)o->pool_id);
            int si;

            /* Anything still attaching this buffer has to drop it: a destroyed
             * buffer cannot be committed again. */
            for (si = 0; si < WL_SERVER_MAX_OBJECTS; si++) {
                struct wl_obj *sf = &c->objs[si];

                if (sf->type != OBJ_SURFACE) continue;
                if (sf->buffer_id != (int32_t)o->id) continue;
                sf->buffer_id = 0;
                console_printf("[wl] buffer.destroy id=%u: detached from "
                               "surface=%u\n", (uint64_t)o->id,
                               (uint64_t)sf->id);
            }
            o->destroyed = 1;
            o->released = 1;
            g_buf_destroyed++;
            console_printf("[wl] buffer.destroy id=%u (pool=%u, released=%u)\n",
                           (uint64_t)o->id, (uint64_t)o->pool_id,
                           (uint64_t)g_buf_released);
            log_buffer_stats();
            log_memfd_live("buffer destroyed");
            if (pool != NULL && pool->type == OBJ_POOL && pool->child_buffers > 0) {
                pool->child_buffers--;
                if (pool->destroyed && pool->child_buffers == 0) {
                    console_printf("[wl] shm_pool.destroy id=%u: last buffer gone, "
                                   "object released\n", (uint64_t)pool->id);
                    (void)memfd_unref(pool->pool_fd);
                    log_memfd_live("pool released (last buffer gone)");
                    destroy_obj(c, pool);
                }
            }
            destroy_obj(c, o);
        } else {
            console_printf("[wl] wl_buffer(id=%u): unsupported opcode %u\n",
                           (uint64_t)o->id, (uint64_t)opcode);
        }
        break;
    case OBJ_KEYBOARD:
        if (opcode == 0u) {              /* wl_keyboard.release */
            if (o->entered_surface != 0u) {
                emit_kb_leave(c, o->id, ++g_serial, o->entered_surface);
                console_printf("[wl] keyboard leave id=%u surface=%u serial=%u\n",
                               (uint64_t)o->id, (uint64_t)o->entered_surface,
                               (uint64_t)g_serial);
                o->entered_surface = 0;
            }
            console_printf("[wl] keyboard.release id=%u (key events=%u)\n",
                           (uint64_t)o->id, (uint64_t)c->kb_events);
            keymap_unuse("keyboard released");
            destroy_obj(c, o);
        } else {
            console_printf("[wl] wl_keyboard(id=%u): unsupported opcode %u\n",
                           (uint64_t)o->id, (uint64_t)opcode);
            emit_display_error(c, o->id, WL_ERR_INVALID_METHOD,
                               "wl_keyboard only has release");
        }
        break;
    case OBJ_CALLBACK:
        if (opcode == 0u) {              /* wl_callback.destroy */
            console_printf("[wl] callback.destroy id=%u (done=%d serial=%u)\n",
                           (uint64_t)o->id, (int64_t)o->cb_done,
                           (uint64_t)o->cb_serial);
            destroy_obj(c, o);
        } else {
            console_printf("[wl] wl_callback(id=%u): unsupported opcode %u\n",
                           (uint64_t)o->id, (uint64_t)opcode);
            emit_display_error(c, o->id, WL_ERR_INVALID_METHOD,
                               "wl_callback only has destroy");
        }
        break;
    default:
        console_printf("[wl] object %u (type %d): request opcode %u ignored\n",
                       (uint64_t)o->id, (int64_t)o->type, (uint64_t)opcode);
        break;
    }
}

/* ===================== public API ===================== */
int wl_server_autostart(void)
{
    if (g_started) return 0;
    g_started = 1;
    console_printf("[wl] minimal Wayland server ready (lazy init: named "
                   "endpoint \"%s\", %d globals: wl_compositor/wl_shm/wl_seat/"
                   "wl_subcompositor, %d client slots)\n",
                   WL_SERVER_NAME, (int64_t)WL_GLOBAL_COUNT,
                   (int64_t)WL_SERVER_MAX_CONNS);
    return 0;
}

void wl_server_accept(int server_fd)
{
    struct wl_conn *c = conn_for(server_fd);
    int i;

    if (c == NULL) {
        for (i = 0; i < WL_SERVER_MAX_CONNS; i++) {
            if (!g_conns[i].used) { c = &g_conns[i]; break; }
        }
    }
    if (c == NULL) {
        console_printf("[wl] accept: connection table full (%d slots)\n",
                       (int64_t)WL_SERVER_MAX_CONNS);
        return;
    }
    memset(c, 0, sizeof(*c));
    c->used = 1;
    c->server_fd = server_fd;
    c->in_len = 0;
    c->out_len = 0;
    (void)add_obj(c, WL_DISPLAY_ID, OBJ_DISPLAY);
    console_printf("[wl] protocol state reset for server_fd=%d (wl_display id=%u, "
                   "connection slot %d, %d retained surface(s))\n",
                   (int64_t)server_fd, (uint64_t)WL_DISPLAY_ID,
                   (int64_t)(c - g_conns), (int64_t)wl_server_surface_count());
}

void wl_server_pump(int server_fd)
{
    struct wl_conn *c = conn_for(server_fd);

    if (c == NULL) return;

    conn_flush(c);

    /* Drain everything the client has written so far. */
    int peer_closed = 0;
    while (c->in_len < WL_SERVER_IN_MAX) {
        const int n = unix_read_fd(server_fd, c->in + c->in_len,
                                   WL_SERVER_IN_MAX - c->in_len);
        if (n == 0) {                    /* peer closed: process, then release */
            peer_closed = 1;
            break;
        }
        if (n < 0) break;                /* -EAGAIN = nothing more */
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

    /* Live input: keyboard keys from the PS/2 mirror, pointer motion/buttons
     * from the kernel mouse.  Only for the capabilities this client bound. */
    input_pump(c);

    conn_flush(c);

    if (peer_closed) {
        /* the client is gone: flush whatever it queued, then free the slot so the
         * table does not leak and the desktop pump can take over */
        conn_release(c);
    }
}

int wl_server_latest_surface(int *w, int *h, int *stride, const uint8_t **px)
{
    const struct wl_keep *k;

    if (g_latest_keep < 0 || g_latest_keep >= WL_SERVER_MAX_CONNS) return 0;
    k = &g_keep[g_latest_keep];
    if (!k->used) return 0;
    if (w) *w = k->w;
    if (h) *h = k->h;
    if (stride) *stride = k->stride;
    if (px) *px = k->px;
    return 1;
}

uint32_t wl_server_latest_hash(void)
{
    const struct wl_keep *k;

    if (g_latest_keep < 0 || g_latest_keep >= WL_SERVER_MAX_CONNS) return 0;
    k = &g_keep[g_latest_keep];
    return k->used ? k->hash : 0u;
}

uint32_t wl_server_commit_count(void)
{
    return g_commits;
}

int wl_server_surface_count(void)
{
    int i, n = 0;

    for (i = 0; i < WL_SERVER_MAX_CONNS; i++)
        if (g_keep[i].used) n++;
    return n;
}

int wl_server_surface_at(int idx, int *w, int *h, int *stride, int *scale,
                         uint32_t *serial, const uint8_t **px)
{
    int i, n = 0;

    if (idx < 0) return 0;
    for (i = 0; i < WL_SERVER_MAX_CONNS; i++) {
        if (!g_keep[i].used) continue;
        if (n++ != idx) continue;
        if (w) *w = g_keep[i].w;
        if (h) *h = g_keep[i].h;
        if (stride) *stride = g_keep[i].stride;
        if (scale) *scale = g_keep[i].scale;
        if (serial) *serial = g_keep[i].serial;
        if (px) *px = g_keep[i].px;
        return 1;
    }
    return 0;
}

int wl_server_subsurface_count(void)
{
    int i, n = 0;

    for (i = 0; i < (int)(sizeof g_subsurfaces / sizeof g_subsurfaces[0]); i++)
        if (g_subsurfaces[i] != NULL) n++;
    return n;
}

int wl_server_subsurface_at(int idx, int *surface_id, int *parent_id,
                            int *x, int *y, int *sync_mode)
{
    int i, n = 0;

    if (idx < 0) return 0;
    for (i = 0; i < (int)(sizeof g_subsurfaces / sizeof g_subsurfaces[0]); i++) {
        const struct wl_obj *sub = g_subsurfaces[i];

        if (sub == NULL) continue;
        if (n++ != idx) continue;
        if (surface_id) *surface_id = (int)sub->child_id;
        if (parent_id) *parent_id = (int)sub->parent_id;
        if (x) *x = (int)sub->pos_x;
        if (y) *y = (int)sub->pos_y;
        if (sync_mode) *sync_mode = sub->sync_mode;
        return 1;
    }
    return 0;
}

uint32_t wl_server_key_event_count(void)
{
    return g_kb_events;
}

uint32_t wl_server_pointer_event_count(void)
{
    return g_ptr_events;
}

/* The XKB keymap this server hands to clients: byte count + FNV-1a of the text
 * (0 before the first keyboard was created). */
uint32_t wl_server_keymap_size(void) { return g_keymap_size; }
uint32_t wl_server_keymap_hash(void) { return g_keymap_hash; }

/* Key events the focus gate dropped because the Wayland window was not focused. */
uint32_t wl_server_keys_gated(void) { return g_kb_gated; }

/* Leaves caused by the pointer leaving the Wayland window. */
uint32_t wl_server_pointer_leaves(void) { return g_ptr_leaves; }

/* Window rectangle changes seen (>= 2 means the window was moved). */
uint32_t wl_server_window_changes(void) { return g_win_changes; }

/* wl_buffer lifecycle counters: buffers created, releases sent, buffers
 * destroyed, and releases skipped because the buffer had not been re-attached.
 * Any out pointer may be NULL. */
void wl_server_buffer_stats(int *created, int *released, int *destroyed)
{
    if (created) *created = (int)g_buf_created;
    if (released) *released = (int)g_buf_released;
    if (destroyed) *destroyed = (int)g_buf_destroyed;
}

uint32_t wl_server_buffer_stats_skipped(void) { return g_buf_release_skipped; }

/* memfd objects the Wayland server currently holds (pools + keymap). */
int wl_server_memfd_live(void) { return memfd_live_count(); }

uint32_t wl_server_events_dropped(void)
{
    return g_events_dropped;
}

int wl_server_active(void)
{
    return g_started;
}
