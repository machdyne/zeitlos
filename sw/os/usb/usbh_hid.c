/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * USB host stack -- HID class driver.
 *
 * Walks a configuration descriptor looking for a boot-protocol
 * keyboard or mouse, then hands the endpoint to the hardware and stops
 * being involved.
 *
 * -- why this driver is so short --
 *
 * Because it does not carry the reports. Once an auto-poll slot is
 * programmed, the controller repeats the interrupt IN by itself and
 * routes boot-protocol bytes straight into the compat registers and
 * the cursor datapath, with no CPU in the path at all. This file runs
 * once per plug event and then never again.
 *
 * That is the whole reason the cursor survives a busy machine: the
 * kernel masks interrupts in k_hid_read_key() and brackets FatFs with
 * k_fs_enter(), and a software-carried pointer would inherit every one
 * of those stalls as visible jitter. See docs/usb_host.md.
 *
 * -- boot protocol only, deliberately --
 *
 * Boot protocol is a FIXED report layout, which is exactly what lets
 * the hardware decode it with a byte mux instead of a report-descriptor
 * parser. Anything that is not a boot keyboard or mouse gets a RAW
 * poll slot and is decoded in software -- which is a strict
 * improvement on what it replaces, since usb_hid_host guessed among
 * several gamepad layouts by heuristic.
 */

#include "usbh.h"
#include "usbh_hw.h"
#include "usbh_int.h"       // z_usbh_mlay_t

#define DESC_INTERFACE      0x04
#define DESC_ENDPOINT       0x05

#define CLASS_HID           0x03
#define SUBCLASS_BOOT       0x01
#define PROTO_KEYBOARD      0x01
#define PROTO_MOUSE         0x02

#define EP_XFER_INTERRUPT   0x03
#define EP_DIR_IN           0x80

#define REQ_SET_IDLE        0x0a
#define REQ_SET_PROTOCOL    0x0b
#define HID_TYPE_CLASS_OUT  0x21

// Which compat block each device type claims. sw/os/hid.c and
// sw/apps/wm/wm.c already decide which block is which at runtime by
// reading typ, so this only has to be consistent, not fixed -- but a
// keyboard landing in block 0 and a mouse in block 1 matches what
// usb_hid_host produced on a two-port machine and keeps any habit
// anybody has formed intact.
static uint8_t blk_taken[2];

static void hid_set_typ(int blk, int typ)
{
    z_usbh_wr(blk == 0 ? Z_USBH_HID0_INFO : Z_USBH_HID1_INFO,
              Z_USBH_TYP_SET(typ));
}

static int blk_claim(void)
{
    if (!blk_taken[0]) { blk_taken[0] = 1; return 0; }
    if (!blk_taken[1]) { blk_taken[1] = 1; return 1; }
    return -1;
}

void z_usbh_hid_release(int blk)
{
    if (blk >= 0 && blk < 2) {
        blk_taken[blk] = 0;
        hid_set_typ(blk, Z_USBH_TYP_NONE);
    }
}

/*
 * Find a boot-protocol HID interface in a configuration descriptor.
 *
 * Returns its bInterfaceNumber, or -1. Separate from the bind below
 * because SET_PROTOCOL and SET_IDLE have to be issued BEFORE the poll
 * slot starts, and both are addressed to an interface -- so the
 * enumeration state machine needs the number before it can bind.
 */
int z_usbh_hid_probe(const uint8_t *cfg, int cfg_len)
{
    int i = 0;

    while (i + 1 < cfg_len) {
        int len = cfg[i];
        int type = cfg[i + 1];

        if (len < 2) break;

        if (type == DESC_INTERFACE && (i + 8) < cfg_len) {
            if (cfg[i + 5] == CLASS_HID &&
                cfg[i + 6] == SUBCLASS_BOOT &&
                (cfg[i + 7] == PROTO_KEYBOARD ||
                 cfg[i + 7] == PROTO_MOUSE))
                return cfg[i + 2];
        }

        i += len;
    }

    return -1;
}

// -- the report descriptor, for mice --
//
// In boot protocol a mouse reports buttons, X and Y; the wheel in a
// fourth byte is a convention (Linux usbmouse.c, TinyUSB), not a rule,
// and some mice -- a Microsoft 045e:0737 among them -- send it only in
// REPORT protocol. So for a mouse the report descriptor is read and
// parsed, and if its input report is the simple layout -- no report
// ID, buttons from bit 0, then X, Y and a wheel as 8-bit values in
// bytes 1, 2 and 3 -- the mouse is left in report protocol (the default
// after reset): usb_hid_compat.v parses that layout exactly as it
// parses a boot report, wheel included. Anything else, it is put in
// boot protocol as before and has no wheel.

#define DESC_HID        0x21
#define DESC_REPORT     0x22

// The report descriptor's length if interface `iface` is a boot mouse,
// from its HID descriptor; 0 otherwise.
int z_usbh_hid_mouse_rdesc_len(const uint8_t *cfg, int cfg_len, int iface)
{
    int i = 0, in_mouse = 0;
    while (i + 1 < cfg_len) {
        int len = cfg[i], type = cfg[i + 1];
        if (len < 2) break;
        if (type == DESC_INTERFACE && (i + 8) < cfg_len)
            in_mouse = cfg[i + 2] == iface && cfg[i + 5] == CLASS_HID &&
                       cfg[i + 7] == PROTO_MOUSE;
        // bNumDescriptors at 5, then {bDescriptorType, wLength}: the
        // report descriptor is conventionally the first.
        if (type == DESC_HID && in_mouse && (i + 8) < cfg_len &&
            cfg[i + 6] == DESC_REPORT)
            return cfg[i + 7] | (cfg[i + 8] << 8);
        i += len;
    }
    return 0;
}

// Walk a HID report descriptor's short items and find where the first
// input report puts the buttons, X, Y and the wheel. Returns 1 if the
// layout is the simple one described above, with a wheel; 0 otherwise
// (lay says what was found either way). Push/pop and long items end
// the walk -- nothing simple uses them.
int z_usbh_hid_rdesc_parse(const uint8_t *rd, int n, z_usbh_mlay_t *lay)
{
    uint32_t page = 0, rsize = 0, rcount = 0;
    uint32_t usages[16];
    int nusage = 0;
    int32_t umin = -1, umax = -1;
    int bit = 0, i = 0;

    lay->btn_bit = lay->x_bit = lay->y_bit = lay->w_bit = -1;
    lay->btn_n = lay->x_sz = lay->y_sz = lay->w_sz = 0;
    lay->has_id = 0;

    while (i < n) {
        uint8_t p = rd[i];
        int sz = p & 3, type = (p >> 2) & 3, tag = p >> 4, k;
        uint32_t v = 0;
        if (p == 0xfe) break;               // long item
        if (sz == 3) sz = 4;
        if (i + 1 + sz > n) break;
        for (k = 0; k < sz; k++) v |= (uint32_t)rd[i + 1 + k] << (8 * k);
        i += 1 + sz;

        if (type == 1) {                    // global
            if (tag == 0) page = v;
            else if (tag == 7) rsize = v;
            else if (tag == 9) rcount = v;
            else if (tag == 8) lay->has_id = 1;
            else if (tag == 10 || tag == 11) break;     // push / pop
        } else if (type == 2) {             // local
            uint32_t u = (sz == 4) ? v : ((page << 16) | v);
            if (tag == 0 && nusage < 16) usages[nusage++] = u;
            else if (tag == 1) umin = (int32_t)u;
            else if (tag == 2) umax = (int32_t)u;
        } else if (type == 0) {             // main
            if (tag == 8) {                 // input
                uint32_t f;
                for (f = 0; f < rcount && f < 64; f++) {
                    uint32_t u = 0, pg, id;
                    if (nusage > 0)
                        u = usages[f < (uint32_t)nusage ? f : (uint32_t)nusage - 1];
                    else if (umin >= 0) {
                        u = (uint32_t)umin + f;
                        if (umax >= 0 && u > (uint32_t)umax) u = (uint32_t)umax;
                    }
                    pg = u >> 16;
                    id = u & 0xffff;
                    if (!(v & 1)) {         // data, not constant padding
                        if (pg == 1 && id == 0x30 && lay->x_bit < 0) {
                            lay->x_bit = (int16_t)bit; lay->x_sz = (uint8_t)rsize;
                        } else if (pg == 1 && id == 0x31 && lay->y_bit < 0) {
                            lay->y_bit = (int16_t)bit; lay->y_sz = (uint8_t)rsize;
                        } else if (pg == 1 && id == 0x38 && lay->w_bit < 0) {
                            lay->w_bit = (int16_t)bit; lay->w_sz = (uint8_t)rsize;
                        } else if (pg == 9) {
                            if (lay->btn_bit < 0) lay->btn_bit = (int16_t)bit;
                            lay->btn_n++;
                        }
                    }
                    bit += (int)rsize;
                }
            }
            nusage = 0; umin = umax = -1;   // locals end at a main item
        }
    }
    return !lay->has_id && lay->btn_bit == 0 && lay->btn_n <= 8 &&
           lay->x_bit == 8 && lay->x_sz == 8 &&
           lay->y_bit == 16 && lay->y_sz == 8 &&
           lay->w_bit == 24 && lay->w_sz == 8;
}

/*
 * Bind a configuration descriptor.
 *
 * Returns the compat block it claimed (0 or 1), or -1.
 *
 * The caller has already issued SET_PROTOCOL and SET_IDLE(0) to the
 * interface z_usbh_hid_probe() found, so by the time this runs the
 * device is known to be producing the fixed boot report layout the
 * hardware's byte mux expects -- or, for a mouse whose report
 * descriptor showed that exact layout with a wheel (above), report
 * protocol, which is then the same bytes plus the wheel. Relying on the subclass-1 default
 * instead works for most devices and not all, and the failure is a
 * keyboard whose keycodes land in the wrong bytes.
 */
int z_usbh_hid_bind(int slot, uint8_t addr, uint8_t xa_flags,
                    uint8_t port, const uint8_t *cfg, int cfg_len,
                    int wheel)
{
    int i = 0;
    int proto = 0;
    // The protocol of the interface the ENDPOINT belonged to.
    //
    // Separate from the running `proto` because that one is cleared
    // by every interface descriptor that is not a boot HID -- and a
    // keyboard commonly has a second interface for its media keys,
    // which comes AFTER the one we matched. By the time the walk
    // ended, proto had been reset to 0 and a boot keyboard was
    // reported as a mouse. A single-interface mouse never showed it.
    int found_proto = 0;
    int found = 0;
    uint8_t ep = 0, ep_mps = 8, interval = 10;
    int blk;
    int mode;
    int typ;

    // Walk the descriptor chain. Each entry is length-prefixed, so a
    // zero length would spin forever -- which is a malformed device,
    // not an impossible one.
    while (i + 1 < cfg_len) {
        int len = cfg[i];
        int type = cfg[i + 1];

        if (len < 2) break;

        if (type == DESC_INTERFACE && (i + 8) < cfg_len) {
            if (cfg[i + 5] == CLASS_HID &&
                cfg[i + 6] == SUBCLASS_BOOT &&
                (cfg[i + 7] == PROTO_KEYBOARD ||
                 cfg[i + 7] == PROTO_MOUSE)) {
                proto = cfg[i + 7];
                found = 0;
            } else {
                // A different interface ends the one we were in, so
                // an endpoint after this does not belong to it.
                proto = 0;
            }
        }

        if (type == DESC_ENDPOINT && proto && !found &&
            (i + 6) < cfg_len) {
            if ((cfg[i + 2] & EP_DIR_IN) &&
                (cfg[i + 3] & 0x03) == EP_XFER_INTERRUPT) {
                ep = cfg[i + 2] & 0x0f;
                ep_mps = cfg[i + 4];
                interval = cfg[i + 6];
                found_proto = proto;
                found = 1;
            }
        }

        i += len;
    }

    if (!found) return -1;

    blk = blk_claim();
    if (blk < 0) return -1;

    if (found_proto == PROTO_KEYBOARD) {
        mode = Z_USBH_MODE_BOOT_KBD;
        typ = Z_USBH_TYP_KBD;
    } else {
        mode = Z_USBH_MODE_BOOT_MOUSE;
        typ = Z_USBH_TYP_MOUSE;
    }

    // A boot report is 8 bytes at most, so a device claiming more is
    // clamped rather than trusted -- the hardware captures eight and
    // the rest would be an overrun the poll engine reports as babble.
    if (ep_mps > 8) ep_mps = 8;
    if (interval == 0) interval = 10;

    // typ BEFORE the slot is enabled. The other order leaves a window
    // in which reports are already arriving at a compat block that
    // still reads typ == 0, and every consumer treats that as "nothing
    // on this port" -- so the first few reports would be delivered and
    // ignored.
    hid_set_typ(blk, typ);

    // Mode bit 2 tells the hardware byte 3 is a confirmed wheel
    // (usb_hid_compat.v, in_wheel) -- only for a mouse the caller put in
    // report protocol after reading its descriptor (see
    // z_usbh_hid_rdesc_parse()). Set here, before the slot is enabled
    // below, so no report is ever counted under the wrong rule.
    z_usbh_wr(Z_USBH_POLL_B(slot),
              Z_USBH_PB_OFF(Z_USBH_OFF_POLL(slot)) |
              Z_USBH_PB_MODE(mode | ((wheel && mode == Z_USBH_MODE_BOOT_MOUSE)
                                      ? 4 : 0)) |
              Z_USBH_PB_CTGT(blk));

    z_usbh_wr(Z_USBH_POLL_A(slot), Z_USBH_PA_ADDR(addr) |
                          Z_USBH_PA_ENDP(ep) |
                          ((uint32_t)xa_flags << 11) |
                          Z_USBH_PA_PORT(port) |
                          Z_USBH_PA_MPS(ep_mps) |
                          Z_USBH_PA_INTERVAL(interval) |
                          Z_USBH_PA_ENABLE);

    return blk;
}
