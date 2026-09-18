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

/*
 * Bind a configuration descriptor.
 *
 * Returns the compat block it claimed (0 or 1), or -1.
 *
 * The caller has already issued SET_PROTOCOL(boot) and SET_IDLE(0) to
 * the interface z_usbh_hid_probe() found, so by the time this runs the
 * device is known to be producing the fixed boot report layout the
 * hardware's byte mux expects. Relying on the subclass-1 default
 * instead works for most devices and not all, and the failure is a
 * keyboard whose keycodes land in the wrong bytes.
 */
int z_usbh_hid_bind(int slot, uint8_t addr, uint8_t xa_flags,
                    uint8_t port, const uint8_t *cfg, int cfg_len)
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

    z_usbh_wr(Z_USBH_POLL_B(slot),
              Z_USBH_PB_OFF(Z_USBH_OFF_POLL(slot)) |
              Z_USBH_PB_MODE(mode) |
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
