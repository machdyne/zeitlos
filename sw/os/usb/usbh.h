/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * USB host stack -- public interface.
 *
 * See docs/usb_host.md. The short version of where the line falls:
 * hardware owns the wire and the cursor, software owns the protocol.
 *
 * -- nothing here blocks --
 *
 * Enumeration spans hundreds of milliseconds: 100 ms attach debounce,
 * 10 ms reset, 10 ms recovery, then several control transfers any of
 * which may NAK. It absolutely must not be busy-waited and must not
 * run inside an ISR.
 *
 * So z_usbh_poll() is a state machine that does AT MOST ONE THING and
 * returns. It is called from two places -- the IRQ 9 handler, when a
 * transaction completes or a port changes, and the kernel timer, so
 * that a state waiting on a timeout still advances. No loops, no
 * waits, no sleeping.
 *
 * That matters more than it sounds. The scheduler is pre-emptive and
 * k_fs_enter()/k_fs_leave() already serialise FatFs; a USB stack that
 * blocked would deadlock against a filesystem operation waiting on a
 * USB disk.
 */

#ifndef Z_USBH_H
#define Z_USBH_H

#include <stdint.h>

#define Z_USBH_MAX_PORTS    2

// Device classes this stack recognises. Anything else enumerates and
// is left alone -- an unrecognised device is not an error, it is a
// device with no driver.
#define Z_USBH_CLASS_NONE   0
#define Z_USBH_CLASS_HID    1
#define Z_USBH_CLASS_MSC    2

// Set up the controller: enable ports, start the frame timer, unmask
// interrupts. Safe to call when Z_FEATURE2_USB_HOST is clear -- it
// checks and does nothing.
void z_usbh_init(void);

// One step. Call from the IRQ 9 handler and from the kernel timer.
void z_usbh_poll(void);

// True once at least one device has finished enumerating.
//
// NOT needed by sw/bios/bios.c: the BIOS is reachable only over the
// serial console and never wants a USB keyboard, so software
// enumeration replacing usb_hid_host's microcode costs it nothing.
int z_usbh_ready(void);

// What is plugged in and where each port got stuck, printed to the
// console. The shell's `lsusb`.
void z_usbh_dump(void);

// Release both ports and report the raw line state: what the BOARD
// does when the controller is provably not driving. The shell's
// `usbidle`.
void z_usbh_probe_idle(void);

// Packet buffer round-trip check plus a dump of the SETUP packet we
// would transmit. The shell's `usbbuf`.
void z_usbh_buftest(void);

// Put every port back to square one so enumeration runs again. The
// driver stops after four failed attempts, so without this there is no
// traffic for the probe to trigger on.
void z_usbh_rescan(void);

// Diagnostics for sh.c: how many devices are attached and what they
// are. Returns the count; fills typ[] with Z_USBH_TYP_* values.
int z_usbh_status(uint8_t *typ, int max);

#endif
