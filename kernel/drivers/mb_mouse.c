/* SutureOS: PS/2 mouse driver (8042 aux port).
 *
 * The GUI used to be keyboard-only ("there is no mouse driver" was literally the
 * comment in gui.c), so there was no pointer on screen and nothing was
 * clickable. This driver brings up the auxiliary device and exposes a polling
 * API in the same style as the keyboard driver (mb_kbd.c): the 8042 is polled
 * once per GUI frame instead of relying on IRQ12, because legacy IRQ delivery
 * through the 8259/IOAPIC cascade proved unreliable on this machine (the
 * keyboard's IRQ1 only ever arrives a couple of times).
 *
 * The polling path matters for coexistence with the keyboard: the 8042 output
 * buffer is shared, and status bit 5 says whether the byte came from the aux
 * (mouse) device. mb_kbd_poll() ignores aux bytes and this driver ignores
 * keyboard bytes, so neither steals the other's data.
 *
 * Packets are the classic 3-byte PS/2 mouse format:
 *   byte0: bit0 L, bit1 R, bit2 M, bit3 always 1, bit4 X sign, bit5 Y sign,
 *          bit6 X overflow, bit7 Y overflow
 *   byte1: dx (9-bit two's complement via byte0 bit4)
 *   byte2: dy (9-bit two's complement via byte0 bit5, positive = up)
 */
#include <stitch/types.h>
#include <stitch/string.h>
#include <stitch/console.h>
#include <stitch/io.h>

#include "mb_mouse.h"

#define PS2_DATA 0x60u
#define PS2_STAT 0x64u
#define PS2_CMD  0x64u

#define STAT_OBF 0x01u   /* output buffer full          */
#define STAT_IBF 0x02u   /* input  buffer full          */
#define STAT_AUX 0x20u   /* byte came from the aux port */

/* Bounded waits: the 8042 is not allowed to hang the boot. */
#define PS2_SPIN 200000u

static int      g_present;          /* 1 once the mouse answered           */
static int      g_x = 512, g_y = 384;   /* cursor position (pixels)        */
static int      g_buttons;          /* current button mask (bit0/1/2)      */
/* v0.65: an IntelliMouse (device id 3/4) sends 4-byte packets; the 4th byte
 * carries the wheel nibble (bits 4..7, 4-bit two's complement) and the extra
 * buttons. A plain 3-byte mouse keeps working - g_packet_size decides. */
static uint8_t  g_packet[4];
static int      g_packet_len;
static int      g_packet_size = 3;
static int      g_device_id = -1;   /* 0 = 3-byte, 3 = wheel, 4 = 5-button */
static int      g_wheel;            /* accumulated Z since the last take() */
static uint32_t g_wheel_events;     /* diagnostics                          */

/* v0.65: button transitions are queued instead of only sampled. The compositor
 * pump replays the queue, so a press+release pair between two pump iterations is
 * no longer invisible (it was: the QMP monitor sends both edges in one burst). */
#define BTN_QUEUE 16
static int      g_btnq[BTN_QUEUE];
static int      g_btnq_head, g_btnq_tail;
static uint32_t g_btnq_dropped;

/* Edge-triggered clicks, so a click is never lost between two GUI frames. */
#define CLICK_QUEUE 16
struct click_ev { int x, y, button; };
static struct click_ev g_clicks[CLICK_QUEUE];
static int g_click_head, g_click_tail;

static uint32_t g_packets;          /* diagnostics                         */
static uint32_t g_dropped;
static uint32_t g_obf_seen;         /* status polls that saw the OBF bit    */
static uint32_t g_aux_seen;         /* ... of which came from the aux port  */

uint32_t mbos_mouse_obf_seen(void) { return g_obf_seen; }
uint32_t mbos_mouse_aux_seen(void) { return g_aux_seen; }

/* ------------------------------- 8042 plumbing ------------------------------ */

static int ps2_wait_input_clear(void)
{
    for (uint32_t i = 0; i < PS2_SPIN; i++) {
        if ((inb(PS2_STAT) & STAT_IBF) == 0) {
            return 0;
        }
        __asm__ volatile("pause");
    }
    return -1;
}

static int ps2_write_cmd(uint8_t cmd)
{
    if (ps2_wait_input_clear() != 0) {
        return -1;
    }
    outb(PS2_CMD, cmd);
    return 0;
}

static int ps2_write_data(uint8_t data)
{
    if (ps2_wait_input_clear() != 0) {
        return -1;
    }
    outb(PS2_DATA, data);
    return 0;
}

/* Read one byte from the 8042 (any device). Returns -1 on timeout. */
static int ps2_read_byte(uint8_t *out)
{
    for (uint32_t i = 0; i < PS2_SPIN; i++) {
        if ((inb(PS2_STAT) & STAT_OBF) != 0) {
            *out = inb(PS2_DATA);
            return 0;
        }
        __asm__ volatile("pause");
    }
    return -1;
}

/* Send one command byte to the aux device and wait for its ACK (0xFA). */
static int mouse_cmd(uint8_t cmd)
{
    uint8_t ack = 0;

    if (ps2_write_cmd(0xD4) != 0)        /* next byte goes to the aux port */
        return -1;
    if (ps2_write_data(cmd) != 0)
        return -1;
    if (ps2_read_byte(&ack) != 0)
        return -1;
    return (ack == 0xFA) ? 0 : -1;       /* 0xFE = resend, treated as failure */
}

/* Send one data byte to the aux device and wait for its ACK. */
static int mouse_data(uint8_t data)
{
    uint8_t ack = 0;

    if (ps2_write_cmd(0xD4) != 0)
        return -1;
    if (ps2_write_data(data) != 0)
        return -1;
    if (ps2_read_byte(&ack) != 0)
        return -1;
    return (ack == 0xFA) ? 0 : -1;
}

/* Read the aux device id (0xF2). Returns -1 when the reply is unusable. */
static int mouse_read_id(void)
{
    uint8_t ack = 0, id = 0;

    if (ps2_write_cmd(0xD4) != 0)
        return -1;
    if (ps2_write_data(0xF2) != 0)
        return -1;
    if (ps2_read_byte(&ack) != 0 || ack != 0xFA)
        return -1;
    if (ps2_read_byte(&id) != 0)
        return -1;
    return (int)id;
}

int mbos_mouse_init(void)
{
    uint8_t cfg = 0;

    g_present = 0;
    g_packet_len = 0;
    g_click_head = g_click_tail = 0;
    g_buttons = 0;

    /* 1) enable the aux port (controller command 0xA8) and the aux DEVICE
     *    (command 0xAE). Firmware sometimes leaves the device disabled; 0xA8
     *    only opens the port, so both are needed for streaming to work. */
    if (ps2_write_cmd(0xA8) != 0)
        return 0;
    (void)ps2_write_cmd(0xAE);

    /* 2) read the controller configuration byte, enable the aux interrupt line
     *    and enable the aux clock (clear bit 5), then write it back. Polling is
     *    used for the data path, but the aux clock must be enabled or the
     *    device never sends anything. */
    if (ps2_write_cmd(0x20) != 0)
        return 0;
    if (ps2_read_byte(&cfg) != 0)
        return 0;
    {
        const uint8_t want = (uint8_t)((cfg | 0x02u) & (uint8_t)~0x20u);
        if (want != cfg) {                 /* only rewrite when needed */
            if (ps2_write_cmd(0x60) != 0)
                return 0;
            if (ps2_write_data(want) != 0)
                return 0;
            cfg = want;
        }
    }

    /* 3) set defaults and enable data reporting. The default 3-byte packet
     *    format, 4 counts/mm resolution and 100 samples/s are exactly what the
     *    decoder below expects, so no argument-taking commands are needed. */
    {
        const int r_defaults = mouse_cmd(0xF6);
        const int r_report   = mouse_cmd(0xF4);
        console_printf("[mouse] cfg=0x%x set-defaults=%s enable-reporting=%s\n",
                       (uint64_t)cfg,
                       (r_defaults == 0) ? "ACK" : "no-ack",
                       (r_report == 0) ? "ACK" : "no-ack");
        if (r_report != 0) {
            return 0;
        }
    }

    /* 3b) v0.65: try to switch a wheel mouse into 4-byte packets.
     *
     * The standard IntelliMouse handshake is "disable reporting, then send the
     * magic sample rates 200/100/80"; a device that understands it changes its
     * id to 3 (wheel) or 4 (5-button) and starts sending a 4th byte. A plain
     * 3-byte mouse ignores all of it and keeps id 0 - in that case we stay on
     * 3-byte packets instead of mis-framing every packet. */
    {
        int id = -1;

        if (mouse_cmd(0xF5) == 0
            && mouse_cmd(0xF3) == 0 && mouse_data(200) == 0
            && mouse_cmd(0xF3) == 0 && mouse_data(100) == 0
            && mouse_cmd(0xF3) == 0 && mouse_data(80) == 0) {
            id = mouse_read_id();
        }
        g_device_id = id;
        g_packet_size = (id == 3 || id == 4) ? 4 : 3;
        /* restore a sane sample rate + resolution and re-enable reporting */
        (void)mouse_cmd(0xF3);
        (void)mouse_data(100);
        (void)mouse_cmd(0xE8);
        (void)mouse_data(0);
        (void)mouse_cmd(0xE6);          /* 1:1 scaling */
        (void)mouse_cmd(0xF4);
        console_printf("[mouse] device id=0x%x (%s) packet=%d bytes\n",
                       (uint64_t)(id < 0 ? 0xFF : id),
                       (id == 3) ? "wheel" : (id == 4) ? "5-button" :
                       (id == 0) ? "3-byte" : "unknown",
                       g_packet_size);
    }

    /* 4) drain anything the device already queued (identity reply, moves) */
    for (int i = 0; i < 64 && (inb(PS2_STAT) & STAT_OBF) != 0; i++) {
        (void)inb(PS2_DATA);
    }

    g_present = 1;
    console_printf("[mouse] PS/2 aux mouse online (polling, config=0x%x)\n",
                   (uint64_t)cfg);
    return 1;
}

int mbos_mouse_present(void)
{
    return g_present;
}

static void click_push(int x, int y, int button)
{
    int next = (g_click_head + 1) % CLICK_QUEUE;

    if (next == g_click_tail) {          /* queue full: drop the oldest */
        g_click_tail = (g_click_tail + 1) % CLICK_QUEUE;
        g_dropped++;
    }
    g_clicks[g_click_head].x = x;
    g_clicks[g_click_head].y = y;
    g_clicks[g_click_head].button = button;
    g_click_head = next;
}

int mbos_mouse_take_click(int *x, int *y, int *button)
{
    if (g_click_tail == g_click_head) {
        return 0;
    }
    if (x) *x = g_clicks[g_click_tail].x;
    if (y) *y = g_clicks[g_click_tail].y;
    if (button) *button = g_clicks[g_click_tail].button;
    g_click_tail = (g_click_tail + 1) % CLICK_QUEUE;
    return 1;
}

/* Decode one 3-byte packet. Shared by the 8042 polling path and the self-test
 * hook, so `startgui test` exercises exactly the code real hardware drives. */
static void mouse_handle_packet(const uint8_t* p, int len)
{
    const uint8_t flags = p[0];
    if ((flags & 0x08u) == 0) {          /* byte0 bit3 must be set in a packet */
        g_dropped++;
        return;
    }
    if ((flags & 0xC0u) != 0) {          /* X/Y overflow: discard the packet */
        g_dropped++;
        return;
    }

    int dx = (int)p[1];
    int dy = (int)p[2];
    if (flags & 0x10u) dx -= 256;        /* X sign bit  */
    if (flags & 0x20u) dy -= 256;        /* Y sign bit  */

    g_x += dx;
    g_y -= dy;                           /* PS/2 Y grows upwards, screen grows down */

    /* v0.65: 4th byte = | Z (4..7, two's complement) | extra buttons (0..3) */
    if (len >= 4) {
        const int z = (int)((int8_t)p[3] >> 4);
        if (z != 0) {
            /* keep the raw notch visible in the serial log: the shell only sees
             * whatever survives the 8042 queue and its own one-per-frame take */
            console_printf("[mouse] wheel z=%d\n", (int64_t)z);
            g_wheel += z;
            if (g_wheel > 64) g_wheel = 64;
            if (g_wheel < -64) g_wheel = -64;
            g_wheel_events++;
        }
    }

    const int buttons = flags & 0x07u;
    const int pressed  = buttons & ~g_buttons;
    if (buttons != g_buttons) {
        const int next = (g_btnq_head + 1) % BTN_QUEUE;
        if (next == g_btnq_tail) {      /* full: drop the oldest transition */
            g_btnq_tail = (g_btnq_tail + 1) % BTN_QUEUE;
            g_btnq_dropped++;
        }
        g_btnq[g_btnq_head] = buttons;
        g_btnq_head = next;
    }
    g_buttons = buttons;

    if (pressed != 0) {
        int btn = (pressed & 0x01u) ? 0 : ((pressed & 0x02u) ? 1 : 2);
        click_push(g_x, g_y, btn);
    }
    g_packets++;
}

/* Hand one raw aux-port byte to the packet decoder.
 *
 * The 8042 output buffer is shared between the keyboard and the mouse. The
 * keyboard poller reads whatever is pending; when the byte is an aux byte it
 * MUST consume it (otherwise the buffer stays full and the keyboard starves -
 * typing died on the first character after boot) and pass it here. */
void mbos_mouse_feed_byte(uint8_t b)
{
    if (!g_present) {
        return;                         /* no mouse: drop it, keep the keyboard alive */
    }
    g_packet[g_packet_len++] = b;
    if (g_packet_len < g_packet_size) {
        return;
    }
    g_packet_len = 0;
    mouse_handle_packet(g_packet, g_packet_size);
}

/* Self-test hook: feed a synthetic packet through the real decoder. */
void mbos_mouse_inject_packet(const uint8_t* p)
{
    if (p == NULL) {
        return;
    }
    /* the self-test hook always feeds classic 3-byte packets */
    mouse_handle_packet(p, 3);
}

/* Called once per frame (and from any idle loop that wants live input). */
void mbos_mouse_poll(void)
{
    int guard = 0;

    if (!g_present) {
        return;
    }

    while ((inb(PS2_STAT) & STAT_OBF) != 0 && guard++ < 64) {
        uint8_t st = inb(PS2_STAT);
        g_obf_seen++;

        /* A byte that is not from the aux port belongs to the keyboard: leave
         * it in the buffer (mb_kbd_poll() owns that path). */
        if ((st & STAT_AUX) == 0) {
            break;
        }
        g_aux_seen++;

        uint8_t b = inb(PS2_DATA);
        g_packet[g_packet_len++] = b;
        if (g_packet_len < g_packet_size) {
            continue;
        }
        g_packet_len = 0;
        mouse_handle_packet(g_packet, g_packet_size);
    }
}

/* v0.65: one-shot wheel delta (positive = away from the user / scroll up),
 * accumulated since the last call. 0 when the device has no wheel. */
int mbos_mouse_take_wheel(void)
{
    const int d = g_wheel;
    g_wheel = 0;
    return d;
}

/* v0.65: syntactic packet self-test. QEMU's PS/2 model only delivers
 * wheel-up to the guest (three probes: wheel-down produces no packet at all),
 * so the +1 direction is proven here through the exact same decoder that real
 * hardware drives. */
int mbos_mouse_wheel_selftest(void)
{
    static const uint8_t up[4]   = { 0x08, 0x00, 0x00, 0x10 };  /* z = +1 */
    static const uint8_t down[4] = { 0x08, 0x00, 0x00, 0xF0 };  /* z = -1 */
    int ok = 1;

    (void)mbos_mouse_take_wheel();              /* start from a clean slate */
    mouse_handle_packet(up, 4);
    if (mbos_mouse_take_wheel() != 1) {
        ok = 0;
    }
    mouse_handle_packet(down, 4);
    if (mbos_mouse_take_wheel() != -1) {
        ok = 0;
    }
    mouse_handle_packet(up, 4);
    mouse_handle_packet(up, 4);
    if (mbos_mouse_take_wheel() != 2) {         /* accumulation across packets */
        ok = 0;
    }
    console_printf("[mouse] wheel selftest: z=+1, z=-1, accumulate -> %s\n",
                   ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

/* v0.65: pop one queued button mask (one entry per transition), 0 when empty. */
int mbos_mouse_take_button(int *mask)
{
    if (g_btnq_tail == g_btnq_head) {
        return 0;
    }
    if (mask != NULL) {
        *mask = g_btnq[g_btnq_tail];
    }
    g_btnq_tail = (g_btnq_tail + 1) % BTN_QUEUE;
    return 1;
}

int mbos_mouse_device_id(void) { return g_device_id; }
int mbos_mouse_packet_size(void) { return g_packet_size; }
uint32_t mbos_mouse_wheel_events(void) { return g_wheel_events; }

int mbos_mouse_x(void) { return g_x; }
int mbos_mouse_y(void) { return g_y; }
int mbos_mouse_buttons(void) { return g_buttons; }

void mbos_mouse_set_bounds(int w, int h)
{
    (void)w;
    (void)h;
}

void mbos_mouse_move_to(int x, int y)
{
    g_x = x;
    g_y = y;
}

uint32_t mbos_mouse_packets(void) { return g_packets; }
uint32_t mbos_mouse_dropped(void) { return g_dropped; }

/* ------------------------- diagnostics for `mouseinfo` --------------------- *
 * Reads the raw 8042 state so a failure can be attributed to the controller,
 * the device or the firmware instead of guessing:
 *   - status byte (OBF/IBF/AUX/timeout/parity)
 *   - the controller configuration byte (bit1 = aux IRQ, bit5 = aux clock off)
 *   - the aux device id (0xF2: 0 = standard 3-byte mouse, 3 = wheel, 4 = 5-btn)
 *   - the last poll counters. */
void mbos_mouse_diagnose(void)
{
    uint8_t st = inb(PS2_STAT);
    uint8_t cfg = 0xFF, id = 0xFF;
    int cfg_ok = 0, id_ok = 0;

    /* config byte readback */
    if (ps2_write_cmd(0x20) == 0 && ps2_read_byte(&cfg) == 0) {
        cfg_ok = 1;
    }
    /* device id: 0xD4 0xF2 -> ACK, then the id byte */
    if (ps2_write_cmd(0xD4) == 0 && ps2_write_data(0xF2) == 0) {
        uint8_t ack = 0;
        if (ps2_read_byte(&ack) == 0) {
            if (ps2_read_byte(&id) == 0) {
                id_ok = 1;
            }
        }
    }

    console_printf("[mouse] status=0x%x (obf=%d ibf=%d aux=%d timeout=%d parity=%d)\n",
                   (uint64_t)st,
                   (int)(st & 0x01u ? 1 : 0), (int)(st & 0x02u ? 1 : 0),
                   (int)(st & 0x20u ? 1 : 0), (int)(st & 0x40u ? 1 : 0),
                   (int)(st & 0x80u ? 1 : 0));
    if (cfg_ok) {
        console_printf("[mouse] config=0x%x (kbd-irq=%d aux-irq=%d xlate=%d "
                       "kbd-clock-off=%d aux-clock-off=%d)\n",
                       (uint64_t)cfg,
                       (int)(cfg & 0x01u ? 1 : 0), (int)(cfg & 0x02u ? 1 : 0),
                       (int)(cfg & 0x40u ? 1 : 0), (int)(cfg & 0x10u ? 1 : 0),
                       (int)(cfg & 0x20u ? 1 : 0));
    } else {
        console_puts("[mouse] config byte read FAILED\n");
    }
    if (id_ok) {
        console_printf("[mouse] device id=0x%x (%s, packet=%d bytes, wheel=%d)\n",
                       (uint64_t)id,
                       (id == 0) ? "standard 3-byte mouse" :
                       (id == 3) ? "wheel mouse" :
                       (id == 4) ? "5-button mouse" : "other",
                       g_packet_size, g_wheel_events ? 1 : 0);
    } else {
        console_puts("[mouse] device id read FAILED\n");
    }
    console_printf("[mouse] present=%d packets=%u dropped=%u obf=%u aux=%u "
                   "pos=%d,%d buttons=%d btnq_dropped=%u\n",
                   (int64_t)g_present, (uint64_t)g_packets, (uint64_t)g_dropped,
                   (uint64_t)g_obf_seen, (uint64_t)g_aux_seen,
                   (int64_t)g_x, (int64_t)g_y, (int64_t)g_buttons,
                   (uint64_t)g_btnq_dropped);
}
