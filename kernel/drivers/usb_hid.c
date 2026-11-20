/* SutureOS module: USB HID boot keyboard / boot mouse on the EHCI driver.
 *
 * Provenance: written for SutureOS (GPL-3.0). No third-party HID decoder was
 * copied; the only references used were:
 *   - USB HID 1.11 appendix B "Boot Interface Descriptors": the 8-byte keyboard
 *     report (modifier byte, reserved byte, six key codes) and the 3-byte mouse
 *     report (button mask, signed dx, signed dy), and the two class requests
 *     SET_IDLE (0x0A, duration 0 = report only on change) and SET_PROTOCOL
 *     (0x0B, value 0 = boot protocol);
 *   - USB 2.0 chapter 9: GET_DESCRIPTOR / SET_ADDRESS / SET_CONFIGURATION,
 *     which are issued through the EHCI control path that already exists in
 *     kernel/drivers/usb_ehci.c (no new transport code here);
 *   - EHCI 1.0 3.x: an interrupt-IN qTD that the device NAKs simply stays
 *     active, so a bounded timeout means "no report pending" - this driver
 *     treats that as NO DATA, not as an error.
 *
 * Design notes
 *   - Polled. SutureOS keeps USBINTR = 0 (see usb_ehci.c): there is no USB
 *     interrupt route, so usb_hid_poll() / usb_hid_idle() do the polling.
 *   - The report decoder is deliberately separated from the transport: every
 *     transfer is one single-qTD interrupt-IN through usb_ehci_intr_in(), and
 *     the hooks this file needs (usb_ehci_*) live at the bottom of
 *     usb_ehci.c so the storage driver - the code that already worked - keeps
 *     its own QH/qTD/bounce buffers untouched.
 *   - Hot-plug: usb_hid_scan() only reads PORTSC (six MMIO reads) when nothing
 *     changed, so it is safe to call from the shell idle loop. A real change
 *     runs the same port-reset + SET_ADDRESS + descriptor walk that the boot
 *     scan uses. Ports owned by the mass-storage driver are never touched.
 *
 * Limitations (kept honest on purpose)
 *   - Boot devices only are decoded. A HID device whose interface is not
 *     bInterfaceSubClass=1 gets its report descriptor length logged and its
 *     slot claimed, but no reports are decoded (parsing arbitrary report
 *     descriptors needs a HID report parser, which this port does not have).
 *   - High speed only: FS/LS devices on an EHCI port are handed back to the
 *     companion controller by usb_ehci_port_reset()/usb_ehci_port_release(),
 *     exactly like the storage driver does.
 *   - No LED/indicator reports (SET_REPORT), no NKRO, no consumer keys.
 */
#include <stitch/types.h>
#include <stitch/string.h>
#include <stitch/console.h>

#include "usb_hid.h"
#include "usb_ehci.h"

#define HID_MAX_DEV       USB_HID_MAX_DEV
#define HID_EVQ_LEN       32
#define HID_CHARQ_LEN     16
#define HID_REPORT_MAX    8
#define HID_MAX_PORTS     16

/* A NAKed interrupt-IN qTD never completes, so this is the "nothing pending"
 * budget. It also bounds one idle-loop iteration and, with the fine-grained
 * delay in usb_ehci.c, costs ~2-3 ms instead of ~40 ms - short enough that a
 * key press (held ~100 ms) is still sampled. */
#define HID_POLL_TIMEOUT_MS 3u

/* Serial noise budget: the first N key / mouse events are logged, the rest only
 * reach usb_hid_poll(). */
#define HID_LOG_LIMIT     32u

/* USB HID class requests (HID 1.11 7.2). */
#define HID_REQ_SET_IDLE      0x0Au
#define HID_REQ_SET_PROTOCOL  0x0Bu
#define HID_PROTO_BOOT        0x0000u

/* bmRequestType receiver field for GET_DESCRIPTOR (0 = device, 1 = interface;
 * the HID report descriptor is an interface-level request, HID 1.11 7.1.1). */
#define HID_RECIP_DEVICE      0x00u
#define HID_RECIP_INTERFACE   0x01u

/* Descriptor types. */
#define USB_DESC_DEVICE       0x01u
#define USB_DESC_CONFIG       0x02u
#define USB_DESC_INTERFACE    0x04u
#define USB_DESC_ENDPOINT     0x05u
#define USB_DESC_HID          0x21u
#define USB_DESC_HID_REPORT   0x22u

/* bInterfaceClass of a HID interface. */
#define USB_CLASS_HID         0x03u

#define HID_SUBCLASS_BOOT     0x01u
#define HID_PROTO_KEYBOARD    0x01u
#define HID_PROTO_MOUSE       0x02u

/* Endpoint attributes: transfer type mask in bmAttributes bits 1:0. */
#define USB_EP_TYPE_MASK      0x03u
#define USB_EP_TYPE_INTR      0x03u

struct usb_hid_dev {
    int      used;
    int      kind;                  /* USB_HID_KIND_*                    */
    int      index;                 /* slot index (for events)           */
    uint8_t  port;                  /* EHCI root port (1 based)          */
    uint8_t  addr;                  /* USB device address                */
    uint8_t  iface;
    uint8_t  ep;                    /* interrupt IN endpoint number      */
    uint16_t mps;                   /* wMaxPacketSize of that endpoint   */
    uint8_t  interval;
    uint8_t  proto;                 /* bInterfaceProtocol                */
    uint8_t  report_len;            /* boot report size we decode        */
    uint32_t toggle;                /* data toggle (DATA0 after config)  */
    uint8_t  last[HID_REPORT_MAX];  /* previous report, for key de-dup   */
    uint8_t  last_buttons;
    uint32_t errors;
    char     name[32];
};

static struct usb_hid_dev g_dev[HID_MAX_DEV];
static usb_hid_event_t    g_evq[HID_EVQ_LEN];
static int  g_ev_head;
static int  g_ev_tail;
/* Decoded characters (Enter = '\n', Backspace = '\b', Tab = '\t') for the
 * shell's input ring: without this a machine that has only a USB keyboard has
 * no way to type at all. Small on purpose - the shell drains it every
 * iteration and dropping is better than blocking the poller. */
static char g_charq[HID_CHARQ_LEN];
static int  g_ch_head;
static int  g_ch_tail;
static int  g_inited;
static int  g_no_controller;
static uint8_t  g_port_ccs[HID_MAX_PORTS + 1u];   /* last seen CCS per port */
static uint8_t  g_port_retry[HID_MAX_PORTS + 1u];
static uint32_t g_idle_calls;
static uint32_t g_idle_next;
static uint32_t g_key_logs;
static uint32_t g_mouse_logs;
static uint32_t g_ev_drops;
static usb_hid_event_t g_last_ev;
static int  g_have_last;

/* ===================== small helpers ===================== */

static uint32_t hid_min_u32(uint32_t a, uint32_t b)
{
    return a < b ? a : b;
}

/* Boot-keyboard usage id -> ASCII for the current modifier byte.
 * A small in-code map, not a full HID keymap: only the usages a boot keyboard
 * can produce on a 101/104 key layout are covered. */
static char hid_kbd_ascii(uint8_t usage, uint8_t mods)
{
    int shift = ((mods & 0x22u) != 0u);          /* LeftShift | RightShift */

    if (usage >= 0x04u && usage <= 0x1Du) {      /* a .. z */
        char c = (char)('a' + (usage - 0x04u));
        return shift ? (char)(c - 'a' + 'A') : c;
    }
    if (usage >= 0x1Eu && usage <= 0x27u) {      /* 1 .. 9, 0 */
        static const char plain[10] = "1234567890";
        static const char sh[10]    = "!@#$%^&*()";
        return shift ? sh[usage - 0x1Eu] : plain[usage - 0x1Eu];
    }
    switch (usage) {
    case 0x28u: return '\n';                     /* Enter      */
    case 0x29u: return 0;                        /* Escape     */
    case 0x2Au: return '\b';                     /* Backspace  */
    case 0x2Bu: return '\t';                     /* Tab        */
    case 0x2Cu: return ' ';                      /* Space      */
    case 0x2Du: return shift ? '_' : '-';
    case 0x2Eu: return shift ? '+' : '=';
    case 0x2Fu: return shift ? '{' : '[';
    case 0x30u: return shift ? '}' : ']';
    case 0x31u: return shift ? '|' : '\\';
    case 0x32u: return shift ? '~' : '#';
    case 0x33u: return shift ? ':' : ';';
    case 0x34u: return shift ? '"' : '\'';
    case 0x35u: return shift ? '~' : '`';
    case 0x36u: return shift ? '<' : ',';
    case 0x37u: return shift ? '>' : '.';
    case 0x38u: return shift ? '?' : '/';
    default:    return 0;
    }
}

static char hid_printable(char c)
{
    return (c >= 32 && c < 127) ? c : '-';
}

/* ===================== event queue ===================== */

static void hid_emit(const usb_hid_event_t *ev)
{
    int next;

    g_last_ev = *ev;
    g_have_last = 1;

    next = (g_ev_head + 1) % HID_EVQ_LEN;
    if (next == g_ev_tail) {         /* queue full: drop, never block */
        g_ev_drops++;
        return;
    }
    g_evq[g_ev_head] = *ev;
    g_ev_head = next;
}

static int hid_pop(usb_hid_event_t *out)
{
    if (g_ev_tail == g_ev_head)
        return 0;
    if (out)
        *out = g_evq[g_ev_tail];
    g_ev_tail = (g_ev_tail + 1) % HID_EVQ_LEN;
    return 1;
}

/* Queue one decoded character for the shell (see usb_hid_take_char()). */
static void hid_push_char(char c)
{
    int next = (g_ch_head + 1) % HID_CHARQ_LEN;

    if (next == g_ch_tail)
        return;                     /* full: drop, never block the poller */
    g_charq[g_ch_head] = c;
    g_ch_head = next;
}

int usb_hid_take_char(void)
{
    char c;

    if (g_ch_tail == g_ch_head)
        return 0;
    c = g_charq[g_ch_tail];
    g_ch_tail = (g_ch_tail + 1) % HID_CHARQ_LEN;
    return (int)(unsigned char)c;
}

/* ===================== report decoding ===================== */

/* Boot keyboard report (8 bytes): [0] modifiers, [1] reserved, [2..7] usages.
 * Keys present now but absent in the previous report are the "key down"
 * events: that is the de-duplication a boot keyboard needs, because the device
 * re-sends the whole key set on every change. */
static void hid_decode_keyboard(struct usb_hid_dev *d, const uint8_t *rep,
                                uint32_t len)
{
    uint8_t full[HID_REPORT_MAX];
    uint8_t mods;
    uint32_t i;
    uint32_t j;

    memset(full, 0, sizeof(full));
    memcpy(full, rep, hid_min_u32(len, HID_REPORT_MAX));
    mods = full[0];

    for (i = 2; i < HID_REPORT_MAX; i++) {
        uint8_t k = full[i];
        int was = 0;

        if (k == 0u || k == 1u)          /* 0 = empty slot, 1 = rollover */
            continue;
        for (j = 2; j < HID_REPORT_MAX; j++) {
            if (d->last[j] == k) {
                was = 1;
                break;
            }
        }
        if (was)
            continue;

        {
            usb_hid_event_t ev;
            char ascii = hid_kbd_ascii(k, mods);

            memset(&ev, 0, sizeof(ev));
            ev.kind = USB_HID_EV_KEY;
            ev.index = d->index;
            ev.keycode = k;
            ev.mods = mods;
            ev.ascii = (ascii >= 32 && ascii < 127) ? ascii : 0;
            hid_emit(&ev);
            /* Enter/Backspace/Tab/letters also become shell input. */
            if (ascii != 0)
                hid_push_char(ascii);

            if (g_key_logs < HID_LOG_LIMIT) {
                g_key_logs++;
                console_printf("[usb-hid] key down=0x%02x ascii=%c mods=0x%02x\n",
                               (uint64_t)k,
                               (uint64_t)(uint8_t)hid_printable(ev.ascii),
                               (uint64_t)mods);
            }
        }
    }

    memcpy(d->last, full, sizeof(d->last));
}

/* Boot mouse report (3 bytes): [0] buttons, [1] signed dx, [2] signed dy. */
static void hid_decode_mouse(struct usb_hid_dev *d, const uint8_t *rep,
                             uint32_t len)
{
    uint8_t buttons;
    int8_t dx;
    int8_t dy;

    if (len < 3u)
        return;
    buttons = rep[0];
    dx = (int8_t)rep[1];
    dy = (int8_t)rep[2];

    if (dx != 0 || dy != 0 || buttons != d->last_buttons) {
        usb_hid_event_t ev;

        memset(&ev, 0, sizeof(ev));
        ev.kind = USB_HID_EV_MOUSE;
        ev.index = d->index;
        ev.buttons = buttons;
        ev.dx = dx;
        ev.dy = dy;
        hid_emit(&ev);

        if (g_mouse_logs < HID_LOG_LIMIT) {
            g_mouse_logs++;
            console_printf("[usb-hid] mouse dx=%d dy=%d btn=%u\n",
                           (int64_t)dx, (int64_t)dy, (uint64_t)buttons);
        }
    }
    d->last_buttons = buttons;
    d->last[0] = buttons;
    d->last[1] = rep[1];
    d->last[2] = rep[2];
}

/* ===================== one interrupt IN transfer ===================== */
static void hid_poll_dev(struct usb_hid_dev *d)
{
    uint8_t rep[HID_REPORT_MAX];
    int n;

    if (!d->used || d->kind == USB_HID_KIND_OTHER || d->report_len == 0u)
        return;

    memset(rep, 0, sizeof(rep));
    n = usb_ehci_intr_in(d->addr, d->mps, d->ep, &d->toggle, rep,
                         (uint32_t)d->report_len, HID_POLL_TIMEOUT_MS);
    if (n < 0) {
        if (n != -2)                 /* -2 = NAK: no report pending */
            d->errors++;
        return;
    }
    if (n == 0)
        return;

    if (d->kind == USB_HID_KIND_KEY)
        hid_decode_keyboard(d, rep, (uint32_t)n);
    else if (d->kind == USB_HID_KIND_MOUSE)
        hid_decode_mouse(d, rep, (uint32_t)n);
}

/* ===================== enumeration ===================== */

struct hid_cand {
    int      valid;
    int      boot;              /* subclass 1 and protocol 1/2           */
    uint8_t  iface;
    uint8_t  sub;
    uint8_t  proto;
    uint8_t  ep;
    uint16_t mps;
    uint8_t  interval;
    uint16_t report_len;        /* wDescriptorLength from the HID desc.  */
};

/* GET_DESCRIPTOR. `recipient` is the bmRequestType receiver field: 0 = device
 * (device/configuration descriptors), 1 = interface (the HID report
 * descriptor - HID 1.11 7.1.1). Getting this wrong makes real devices and
 * QEMU STALL the request, which shows up as
 * "[usb] control req=6 addr=N len=64 in FAILED (tok=...40)" (qTD halted). */
static int hid_get_descriptor(uint8_t addr, uint16_t mps, uint8_t recipient,
                              uint8_t type, uint8_t index, uint16_t len,
                              uint32_t *actual)
{
    uint8_t setup[8];
    int n;

    setup[0] = (uint8_t)(0x80u | (recipient & 0x1Fu));
    setup[1] = 6;                       /* GET_DESCRIPTOR                   */
    setup[2] = 0;
    setup[3] = type;
    setup[4] = index;
    setup[5] = 0;
    setup[6] = (uint8_t)(len & 0xFFu);
    setup[7] = (uint8_t)((len >> 8) & 0xFFu);
    n = usb_ehci_ctl(addr, mps, setup, (uint32_t)len, 1, NULL);
    if (n < 0)
        return -1;
    if (actual)
        *actual = (uint32_t)n;
    return 0;
}

static int hid_set_address(uint16_t mps, uint8_t addr)
{
    uint8_t setup[8] = { 0x00u, 5u, addr, 0u, 0u, 0u, 0u, 0u };

    return usb_ehci_ctl(0, mps, setup, 0, 0, NULL) < 0 ? -1 : 0;
}

static int hid_set_config(uint8_t addr, uint16_t mps, uint8_t value)
{
    uint8_t setup[8] = { 0x00u, 9u, value, 0u, 0u, 0u, 0u, 0u };

    return usb_ehci_ctl(addr, mps, setup, 0, 0, NULL) < 0 ? -1 : 0;
}

/* HID class request with no data stage (SET_IDLE / SET_PROTOCOL). */
static int hid_class_req(uint8_t addr, uint16_t mps, uint8_t req,
                         uint16_t value, uint16_t index)
{
    uint8_t setup[8];

    setup[0] = 0x21u;                   /* host-to-device, class, interface */
    setup[1] = req;
    setup[2] = (uint8_t)(value & 0xFFu);
    setup[3] = (uint8_t)((value >> 8) & 0xFFu);
    setup[4] = (uint8_t)(index & 0xFFu);
    setup[5] = (uint8_t)((index >> 8) & 0xFFu);
    setup[6] = 0;
    setup[7] = 0;
    return usb_ehci_ctl(addr, mps, setup, 0, 0, NULL) < 0 ? -1 : 0;
}

/* Walk a configuration descriptor and pick the HID interface to use: a boot
 * interface (subclass 1, protocol 1 or 2) wins over any other HID interface,
 * which is what "boot subclass first" means in practice. */
static int hid_parse_config(const uint8_t *p, uint32_t total,
                            uint8_t *cfg_value, struct hid_cand *out)
{
    const uint8_t *end = p + total;
    struct hid_cand cur_if;
    struct hid_cand best_boot;
    struct hid_cand best_any;

    memset(&cur_if, 0, sizeof(cur_if));
    memset(&best_boot, 0, sizeof(best_boot));
    memset(&best_any, 0, sizeof(best_any));

    while (p + 2 <= end && p[0] >= 2u && p + p[0] <= end) {
        uint8_t blen = p[0];
        uint8_t btype = p[1];

        if (btype == USB_DESC_CONFIG && blen >= 9u) {
            *cfg_value = p[5];
        } else if (btype == USB_DESC_INTERFACE && blen >= 9u) {
            /* Bank the interface we just finished, then start the next one. */
            if (cur_if.valid) {
                if (cur_if.boot && !best_boot.valid)
                    best_boot = cur_if;
                if (!best_any.valid)
                    best_any = cur_if;
            }
            memset(&cur_if, 0, sizeof(cur_if));
            cur_if.iface = p[2];
            cur_if.sub = p[6];
            cur_if.proto = p[7];
            if (p[5] == USB_CLASS_HID) {
                cur_if.valid = 1;
                cur_if.boot = (cur_if.sub == HID_SUBCLASS_BOOT &&
                               (cur_if.proto == HID_PROTO_KEYBOARD ||
                                cur_if.proto == HID_PROTO_MOUSE)) ? 1 : 0;
            }
        } else if (btype == USB_DESC_HID && blen >= 9u && cur_if.valid) {
            /* HID descriptor: wDescriptorLength = report descriptor size. */
            cur_if.report_len = (uint16_t)p[7] | ((uint16_t)p[8] << 8);
        } else if (btype == USB_DESC_ENDPOINT && blen >= 7u && cur_if.valid) {
            if ((p[2] & 0x80u) &&
                ((p[3] & USB_EP_TYPE_MASK) == USB_EP_TYPE_INTR)) {
                if (!cur_if.ep) {
                    cur_if.ep = (uint8_t)(p[2] & 0x0Fu);
                    cur_if.mps = (uint16_t)p[4] | ((uint16_t)p[5] << 8);
                    cur_if.interval = p[6];
                }
            }
        }
        p += blen;
    }
    if (cur_if.valid) {
        if (cur_if.boot && !best_boot.valid)
            best_boot = cur_if;
        if (!best_any.valid)
            best_any = cur_if;
    }

    if (best_boot.valid) {
        *out = best_boot;
        return 0;
    }
    if (best_any.valid) {
        *out = best_any;
        return 0;
    }
    return -1;
}

/* Slot index of the device on `port`, -1 when that port has no HID device. */
static int hid_slot_on_port(uint32_t port)
{
    int i;

    for (i = 0; i < HID_MAX_DEV; i++) {
        if (g_dev[i].used && g_dev[i].port == (uint8_t)port)
            return i;
    }
    return -1;
}

/* Reset + enumerate the device on one root port. Returns
 *   0  a HID device was claimed (slot filled in),
 *  -1  enumeration/transport error,
 *  -2  the port has a high-speed device but it is not a HID device. */
static int hid_attach(uint32_t port, int announce)
{
    uint16_t mps = 8;
    uint8_t addr;
    uint32_t actual = 0;
    uint32_t total = 0;
    const uint8_t *d;
    uint8_t cfg_value = 1;
    struct hid_cand cand;
    int slot;
    int kind;
    uint16_t vid = 0;
    uint16_t pid = 0;

    if (usb_ehci_port_reset(port) != 0) {
        /* No PED after reset => full/low-speed device: hand the port back to
         * the companion controller (this driver does no split transactions). */
        usb_ehci_port_release(port);
        return -2;
    }

    /* 1) 8-byte device descriptor at the default address for bMaxPacketSize0. */
    if (hid_get_descriptor(0, 8, HID_RECIP_DEVICE, USB_DESC_DEVICE, 0, 8, &actual) != 0) {
        console_printf("[usb-hid] port %u: GET_DESCRIPTOR(device,8) failed\n",
                       (uint64_t)port);
        return -1;
    }
    d = usb_ehci_in_data();
    if (d[1] != USB_DESC_DEVICE || d[0] < 8u) {
        console_printf("[usb-hid] port %u: bad device descriptor "
                       "(bLength=%u bDescriptorType=%u)\n",
                       (uint64_t)port, (uint64_t)d[0], (uint64_t)d[1]);
        return -1;
    }
    mps = (uint16_t)d[7];
    if (mps != 8u && mps != 16u && mps != 32u && mps != 64u)
        mps = 8u;

    /* 2) SET_ADDRESS, then everything else at the new address. */
    addr = (uint8_t)usb_ehci_alloc_address();
    if (hid_set_address(mps, addr) != 0) {
        console_printf("[usb-hid] port %u: SET_ADDRESS(%u) failed\n",
                       (uint64_t)port, (uint64_t)addr);
        return -1;
    }

    /* 3) Full device descriptor (idVendor/idProduct for the log). */
    if (hid_get_descriptor(addr, mps, HID_RECIP_DEVICE, USB_DESC_DEVICE, 0, 18, &actual) != 0) {
        console_printf("[usb-hid] port %u: GET_DESCRIPTOR(device,18) failed\n",
                       (uint64_t)port);
        return -1;
    }
    d = usb_ehci_in_data();
    if (d[1] != USB_DESC_DEVICE || d[0] < 18u) {
        console_printf("[usb-hid] port %u: short device descriptor "
                       "(bLength=%u bDescriptorType=%u)\n",
                       (uint64_t)port, (uint64_t)d[0], (uint64_t)d[1]);
        return -1;
    }
    vid = (uint16_t)d[8] | ((uint16_t)d[9] << 8);
    pid = (uint16_t)d[10] | ((uint16_t)d[11] << 8);

    /* 4) Configuration descriptor: 9 bytes for wTotalLength, then the whole
     *    thing (interfaces + endpoints + HID descriptors).
     * The descriptors are validated by their own bLength/bDescriptorType
     * instead of the transfer's byte count: a qTD that completed without an
     * error status is the authoritative "the request arrived" signal (see the
     * residual note in usb_ehci_ctl()). */
    if (hid_get_descriptor(addr, mps, HID_RECIP_DEVICE, USB_DESC_CONFIG, 0, 9, &actual) != 0) {
        console_printf("[usb-hid] port %u: GET_DESCRIPTOR(config,9) failed\n",
                       (uint64_t)port);
        return -1;
    }
    d = usb_ehci_in_data();
    if (d[1] != USB_DESC_CONFIG || d[0] < 9u) {
        console_printf("[usb-hid] port %u: bad configuration descriptor "
                       "(bLength=%u bDescriptorType=%u)\n",
                       (uint64_t)port, (uint64_t)d[0], (uint64_t)d[1]);
        return -1;
    }
    total = (uint32_t)d[2] | ((uint32_t)d[3] << 8);
    if (total < 9u || total > 512u) {
        console_printf("[usb-hid] port %u: absurd wTotalLength=%u\n",
                       (uint64_t)port, (uint64_t)total);
        return -1;
    }
    if (hid_get_descriptor(addr, mps, HID_RECIP_DEVICE, USB_DESC_CONFIG, 0, (uint16_t)total,
                           &actual) != 0) {
        console_printf("[usb-hid] port %u: GET_DESCRIPTOR(config,%u) failed\n",
                       (uint64_t)port, (uint64_t)total);
        return -1;
    }
    d = usb_ehci_in_data();

    memset(&cand, 0, sizeof(cand));
    if (hid_parse_config(d, total, &cfg_value, &cand) != 0) {
        /* High-speed device without a HID interface (e.g. a storage stick the
         * storage driver did not claim): not ours. */
        return -2;
    }
    if (cand.ep == 0u) {
        console_printf("[usb-hid] port %u: HID interface %u has no interrupt IN "
                       "endpoint (skipped)\n",
                       (uint64_t)port, (uint64_t)cand.iface);
        return -2;
    }

    /* 5) SET_CONFIGURATION: resets the endpoint data toggles to DATA0. */
    if (hid_set_config(addr, mps, cfg_value) != 0)
        return -1;

    /* 6) SET_IDLE(0): report only on change, and never auto-repeat. */
    (void)hid_class_req(addr, mps, HID_REQ_SET_IDLE, 0u, cand.iface);

    kind = USB_HID_KIND_OTHER;
    if (cand.boot) {
        /* 7) Boot device: make sure it really is in boot protocol. */
        (void)hid_class_req(addr, mps, HID_REQ_SET_PROTOCOL, HID_PROTO_BOOT,
                            cand.iface);
        if (cand.proto == HID_PROTO_KEYBOARD)
            kind = USB_HID_KIND_KEY;
        else if (cand.proto == HID_PROTO_MOUSE)
            kind = USB_HID_KIND_MOUSE;
    }

    for (slot = 0; slot < HID_MAX_DEV; slot++) {
        if (!g_dev[slot].used)
            break;
    }
    if (slot == HID_MAX_DEV) {
        console_printf("[usb-hid] port %u: no free device slot (%u supported)\n",
                       (uint64_t)port, (uint64_t)HID_MAX_DEV);
        return -1;
    }

    memset(&g_dev[slot], 0, sizeof(g_dev[slot]));
    g_dev[slot].used = 1;
    g_dev[slot].kind = kind;
    g_dev[slot].index = slot;
    g_dev[slot].port = (uint8_t)port;
    g_dev[slot].addr = addr;
    g_dev[slot].iface = cand.iface;
    g_dev[slot].ep = cand.ep;
    g_dev[slot].mps = cand.mps ? cand.mps : 8u;
    g_dev[slot].interval = cand.interval;
    g_dev[slot].proto = cand.proto;
    g_dev[slot].toggle = 0;
    if (kind == USB_HID_KIND_KEY)
        g_dev[slot].report_len = 8u;
    else if (kind == USB_HID_KIND_MOUSE)
        g_dev[slot].report_len = 3u;
    else
        g_dev[slot].report_len = 0u;

    if (kind == USB_HID_KIND_KEY) {
        snprintf(g_dev[slot].name, sizeof(g_dev[slot].name),
                 "usb-kbd(addr=%u,ep=0x%x)", (unsigned)addr,
                 (unsigned)(0x80u | cand.ep));
        console_printf("[usb-hid] kbd addr=%u iface=%u ep=0x%x mps=%u\n",
                       (uint64_t)addr, (uint64_t)cand.iface,
                       (uint64_t)(0x80u | cand.ep), (uint64_t)g_dev[slot].mps);
    } else if (kind == USB_HID_KIND_MOUSE) {
        snprintf(g_dev[slot].name, sizeof(g_dev[slot].name),
                 "usb-mouse(addr=%u,ep=0x%x)", (unsigned)addr,
                 (unsigned)(0x80u | cand.ep));
        console_printf("[usb-hid] mouse addr=%u iface=%u ep=0x%x mps=%u\n",
                       (uint64_t)addr, (uint64_t)cand.iface,
                       (uint64_t)(0x80u | cand.ep), (uint64_t)g_dev[slot].mps);
    } else {
        uint32_t rd_len = cand.report_len;
        uint32_t rd_read = 0;

        snprintf(g_dev[slot].name, sizeof(g_dev[slot].name),
                 "usb-hid(addr=%u,iface=%u)", (unsigned)addr,
                 (unsigned)cand.iface);
        /* Not a boot device: at least prove the report descriptor is readable
         * and report its length (HID 1.11 6.2.2). */
        if (rd_len > 0u) {
            uint32_t probe = hid_min_u32(rd_len, 64u);

            if (hid_get_descriptor(addr, mps, HID_RECIP_INTERFACE, USB_DESC_HID_REPORT, cand.iface,
                                   (uint16_t)probe, &rd_read) != 0)
                rd_read = 0;
        }
        console_printf("[usb-hid] hid addr=%u iface=%u class=3 sub=%u proto=%u "
                       "report descriptor len=%u read=%u (not boot: reports "
                       "are not decoded)\n",
                       (uint64_t)addr, (uint64_t)cand.iface,
                       (uint64_t)cand.sub, (uint64_t)cand.proto,
                       (uint64_t)rd_len, (uint64_t)rd_read);
    }

    console_printf("[usb-hid] device addr=%u vid=%04x pid=%04x port=%u kind=%s\n",
                   (uint64_t)addr, (uint64_t)vid, (uint64_t)pid, (uint64_t)port,
                   kind == USB_HID_KIND_KEY ? "kbd" :
                   (kind == USB_HID_KIND_MOUSE ? "mouse" : "other"));

    if (announce)
        console_printf("[usb-hid] hotplug: new HID on port %u\n",
                       (uint64_t)port);
    return 0;
}

/* Release every slot on `port` and log the unplug. Returns the number of slots
 * released. */
static int hid_detach_port(uint32_t port)
{
    int i;
    int released = 0;

    for (i = 0; i < HID_MAX_DEV; i++) {
        if (!g_dev[i].used || g_dev[i].port != (uint8_t)port)
            continue;
        g_dev[i].used = 0;
        released++;
    }
    if (released)
        console_printf("[usb-hid] unplug: port %u (%d HID device(s) released)\n",
                       (uint64_t)port, (int64_t)released);
    if (port <= HID_MAX_PORTS)
        g_port_retry[port] = 0;
    return released;
}

/* ===================== public API ===================== */

int usb_hid_present(void)
{
    return usb_hid_count() > 0 ? 1 : 0;
}

int usb_hid_count(void)
{
    int i;
    int n = 0;

    for (i = 0; i < HID_MAX_DEV; i++) {
        if (g_dev[i].used)
            n++;
    }
    return n;
}

int usb_hid_kind(int i)
{
    if (i < 0 || i >= HID_MAX_DEV || !g_dev[i].used)
        return USB_HID_KIND_NONE;
    return g_dev[i].kind;
}

const char *usb_hid_name(int i)
{
    if (i < 0 || i >= HID_MAX_DEV || !g_dev[i].used)
        return "-";
    return g_dev[i].name;
}

int usb_hid_last_event(usb_hid_event_t *out)
{
    if (!g_have_last)
        return 0;
    if (out)
        *out = g_last_ev;
    return 1;
}

int usb_hid_init(void)
{
    uint32_t port;
    uint32_t nports;

    if (g_inited)
        return usb_hid_count();
    if (g_no_controller)
        return 0;
    if (!usb_ehci_ready()) {
        /* No EHCI controller: stay silent and out of the hot path, exactly
         * like the storage driver's single "[usb] no controller" line. */
        g_no_controller = 1;
        return 0;
    }
    /* Set the flag first: hid_attach() must never recurse into init. */
    g_inited = 1;

    nports = usb_ehci_ports();
    if (nports > HID_MAX_PORTS)
        nports = HID_MAX_PORTS;

    for (port = 1; port <= nports; port++) {
        uint32_t psc;
        int rc;

        if (port == (uint32_t)usb_ehci_storage_port())
            continue;                       /* the storage driver owns it */
        psc = usb_ehci_portsc(port);
        if (!(psc & USB_EHCI_PORTSC_CCS))
            continue;
        usb_ehci_portsc_ack(port);          /* clear the latched change bits */
        rc = hid_attach(port, 0);
        if (rc == -1)
            console_printf("[usb-hid] port %u: HID enumeration failed\n",
                           (uint64_t)port);
    }

    /* Hot-plug baseline: whatever is connected now was handled above, so the
     * first usb_hid_scan() must not report it as a hot-plug event. */
    for (port = 1; port <= nports; port++) {
        uint32_t psc = usb_ehci_portsc(port);

        g_port_ccs[port] = (psc & USB_EHCI_PORTSC_CCS) ? 1u : 0u;
        g_port_retry[port] = 0;
    }
    return usb_hid_count();
}

int usb_hid_scan(void)
{
    uint32_t port;
    uint32_t nports;

    if (!g_inited) {
        (void)usb_hid_init();
        if (!g_inited)
            return 0;
    }

    nports = usb_ehci_ports();
    if (nports > HID_MAX_PORTS)
        nports = HID_MAX_PORTS;

    for (port = 1; port <= nports; port++) {
        uint32_t psc = usb_ehci_portsc(port);
        int ccs = (psc & USB_EHCI_PORTSC_CCS) ? 1 : 0;

        if (psc & USB_EHCI_PORTSC_CSC)
            usb_ehci_portsc_ack(port);      /* W1C: one latch per event */

        if (ccs && !g_port_ccs[port]) {
            int rc;

            g_port_ccs[port] = 1u;
            if (port == (uint32_t)usb_ehci_storage_port())
                continue;                   /* not ours */
            if (hid_slot_on_port(port) >= 0)
                continue;                   /* already claimed */
            rc = hid_attach(port, 1);
            if (rc == -1) {
                /* Enumeration glitch (device still settling): retry on the
                 * next scans, then stop and say so once. */
                if (g_port_retry[port] < 3u) {
                    g_port_retry[port]++;
                    g_port_ccs[port] = 0u;
                } else {
                    g_port_retry[port] = 0;
                    console_printf("[usb-hid] hotplug: port %u enumeration "
                                   "failed (giving up)\n", (uint64_t)port);
                }
            } else if (rc == -2) {
                console_printf("[usb-hid] hotplug: port %u device is not a "
                               "HID device (skipped)\n", (uint64_t)port);
            }
        } else if (!ccs && g_port_ccs[port]) {
            g_port_ccs[port] = 0u;
            if (hid_slot_on_port(port) >= 0)
                (void)hid_detach_port(port);
        }
    }
    return usb_hid_count();
}

int usb_hid_poll(usb_hid_event_t *out)
{
    int i;
    int tries;

    if (!g_inited) {
        (void)usb_hid_init();
        if (!g_inited)
            return -1;
    }
    if (hid_pop(out))
        return 1;

    /* Nothing queued: give each device two chances to complete a report. The
     * second round is what catches a press+release pair that arrived between
     * two polls. */
    for (tries = 0; tries < 2; tries++) {
        for (i = 0; i < HID_MAX_DEV; i++)
            hid_poll_dev(&g_dev[i]);
        if (g_ev_head != g_ev_tail)
            break;
    }
    return hid_pop(out) ? 1 : 0;
}

void usb_hid_idle(void)
{
    if (!g_inited) {
        (void)usb_hid_init();
        if (!g_inited)
            return;                    /* no controller: zero cost forever */
    }

    g_idle_calls++;

    /* Root-port check is six MMIO reads when nothing changed; it is what makes
     * plugging a keyboard in at the prompt show up. */
    if ((g_idle_calls & 0x0Fu) == 0u)
        (void)usb_hid_scan();

    /* One device per poll, round robin, and only every 4th idle iteration.
     * A NAKed interrupt-IN now costs ~2-3 ms (fine-grained delay in
     * usb_ehci.c), so the shell idle loop stays at ~1-2 ms and a key press is
     * sampled several times inside the ~100 ms the host holds it down. */
    if ((g_idle_calls & 0x3u) == 0u) {
        uint32_t k;

        for (k = 0; k < HID_MAX_DEV; k++) {
            uint32_t idx = (g_idle_next + k) % HID_MAX_DEV;

            if (!g_dev[idx].used || g_dev[idx].kind == USB_HID_KIND_OTHER)
                continue;
            hid_poll_dev(&g_dev[idx]);
            g_idle_next = (idx + 1) % HID_MAX_DEV;
            break;
        }
    }
}
