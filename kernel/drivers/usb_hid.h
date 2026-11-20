/* SutureOS module: USB HID (boot keyboard / boot mouse) on top of the EHCI
 * host controller driver (kernel/drivers/usb_ehci.c).
 *
 * Provenance: written for SutureOS; no third-party HID decoder was copied.
 *   - the USB HID 1.11 class specification (boot-protocol report layouts:
 *     8-byte keyboard report = modifiers, reserved, 6 key codes; 3-byte mouse
 *     report = buttons, signed dx, signed dy) is the only reference used;
 *   - the USB 2.0 chapter 9 request sequence (GET_DESCRIPTOR / SET_ADDRESS /
 *     SET_CONFIGURATION) and the HID class requests SET_IDLE (0x0A) and
 *     SET_PROTOCOL (0x0B) are issued through the existing EHCI control path;
 *   - the transport is the polled async-schedule qTD/QH machinery that already
 *     exists in usb_ehci.c (usb_hid.c only asks it for a single interrupt-IN
 *     transfer, and does the report decoding itself).
 * This driver is polled, not interrupt driven: SutureOS owns no USB interrupt
 * route (USBINTR stays 0 in usb_ehci.c), so the shell idle loop and the
 * `usbinfo` shell command drive usb_hid_poll()/usb_hid_scan().
 */
#ifndef STITCH_USB_HID_H
#define STITCH_USB_HID_H

#include <stitch/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Event kinds handed out by usb_hid_poll(). */
#define USB_HID_EV_NONE   0
#define USB_HID_EV_KEY    1     /* a boot-keyboard key went down       */
#define USB_HID_EV_MOUSE  2     /* boot mouse moved and/or changed btn */

/* Device kinds reported by usb_hid_kind()/usb_hid_name(). */
#define USB_HID_KIND_NONE  0
#define USB_HID_KIND_KEY   1
#define USB_HID_KIND_MOUSE 2
#define USB_HID_KIND_OTHER 3    /* HID class, but not a boot kbd/mouse */

/* Number of device slots (index range accepted by usb_hid_name()/kind()). */
#define USB_HID_MAX_DEV    4

typedef struct {
    int     kind;       /* USB_HID_EV_KEY / USB_HID_EV_MOUSE            */
    int     index;      /* device slot (matches usb_hid_name(i))        */
    uint8_t keycode;    /* HID usage id of the key that went down       */
    uint8_t mods;       /* boot keyboard modifier byte of that report   */
    char    ascii;      /* ASCII for that usage, 0 when not printable   */
    uint8_t buttons;    /* mouse: bit0 left, bit1 right, bit2 middle    */
    int8_t  dx;         /* mouse: signed movement                       */
    int8_t  dy;
} usb_hid_event_t;

/* Bring-up: scan/claim every EHCI root port that currently has a high-speed
 * HID device attached. Idempotent and lazy: usb_hid_poll()/usb_hid_scan() call
 * it on the first use, so a kernel that never calls usb_hid_init() explicitly
 * still gets working HID input. Returns the number of HID devices claimed. */
int usb_hid_init(void);

/* Hot-plug rescan: re-read every EHCI root port's PORTSC, acknowledge the
 * change bits, reset/enumerate newly connected high-speed HID devices and
 * release the slots of unplugged ones. Cheap (six MMIO reads) when nothing
 * changed. Returns the number of HID devices present afterwards. */
int usb_hid_scan(void);

/* 1 when at least one HID device is claimed. */
int usb_hid_present(void);

/* Number of claimed HID devices (0 when none). */
int usb_hid_count(void);

/* Pop one pending input event. Returns 1 when *out was filled, 0 when nothing
 * is pending (the devices were polled), -1 when no HID device exists.
 * `out` may be NULL to just drain. */
int usb_hid_poll(usb_hid_event_t *out);

/* Human readable identity of device `i`, e.g. "usb-kbd(addr=2,ep=0x81)";
 * returns "-" for an empty/invalid slot. */
const char *usb_hid_name(int i);

/* Device kind of slot `i` (USB_HID_KIND_*). */
int usb_hid_kind(int i);

/* Rate-limited idle hook for the shell main loop: polls one device per call
 * (round robin) and rescans the root ports every 16 calls. No-op (one
 * comparison) when no HID device was ever claimed. */
void usb_hid_idle(void);

/* Pop one decoded character (letters/digits/symbols, Enter = '\n',
 * Backspace = '\b', Tab = '\t') so a machine whose only keyboard is USB can
 * still type. Returns 0 when nothing is pending. The shell drains this from
 * console_getc(); it is deliberately separate from usb_hid_poll() so an event
 * consumer cannot swallow typed characters. */
int usb_hid_take_char(void);

/* Last decoded event (independent of the queue), for `usbinfo`. Returns 0 when
 * no event has ever been decoded. */
int usb_hid_last_event(usb_hid_event_t *out);

#ifdef __cplusplus
}
#endif

#endif /* STITCH_USB_HID_H */
