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

// Devices at once, root ports and hubs included. USB addresses are
// 1..7 from a bitmap and one more device can sit on address 0 while it
// enumerates. See docs/usb_host.md, "Topology model".
#define Z_USBH_MAX_DEVS     8

// Device classes this stack recognises. Anything else enumerates and
// is left alone -- an unrecognised device is not an error, it is a
// device with no driver.
#define Z_USBH_CLASS_NONE   0
#define Z_USBH_CLASS_HID    1
#define Z_USBH_CLASS_MSC    2
#define Z_USBH_CLASS_HUB    3
#define Z_USBH_CLASS_CDC    4

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

// -- the transaction engine has one owner at a time --
//
// There is ONE engine and two kinds of user: the enumeration state
// machine, which runs from the ISR, and the mass storage driver, which
// runs in process context and blocks. The ISR can preempt the MSC
// driver between any two register accesses, so without an owner it
// could launch a control transaction between bulk transactions --
// taking the bulk result as its own -- or even between the two
// register writes that start one, sending the bulk transfer to the
// wrong device.
//
// z_usbh_bus_reserve() stops the enumeration side STARTING any new
// control transfer and waits until none is on the bus; the caller then
// owns the engine until z_usbh_bus_release(). A control transfer
// already under way is allowed to finish, because stopping one halfway
// leaves its device mid-request. Enumeration simply pauses while the
// bus is held; nothing it does is time-critical at that point.
//
// Process context only. Returns 0 if a control transfer did not finish
// within the bound, in which case the bus is NOT held.
int z_usbh_bus_reserve(void);
void z_usbh_bus_release(void);

// -- wire capture of a failing transaction (rtl/probe.v) --
//
// z_usbh_cap_start() makes the driver arm the built-in logic probe
// before every transaction on root port 0 and freeze it on the first
// that ends in a timeout, CRC error or babble, so the capture begins
// with the failing transaction. While it runs, the hardware's NAK
// retries are off -- each attempt is one transaction, and one capture
// -- because a retried transaction outlasts the 170 us window.
// z_usbh_cap_dump() prints it for tools/usbcap.py. Needs a bitstream
// built with `PROBE; returns 0 without one. The shell's `usbcap` and
// `usbcapd`.
// Hardware NAK retries for control transfers to a hub and to every
// device behind one, 0-15; 0 (the default) retries every NAK from
// software a tick later. The shell's `usbnak N`.
void z_usbh_set_ls_hub_nak(int n);

// mode 0: freeze on the first failing transaction; 1: on the first
// successful IN with data from a low-speed device behind a hub.
int z_usbh_cap_start(int mode);

// Keyboard LEDs, for every attached keyboard: bit 0 Num Lock, bit 1 Caps
// Lock, bit 2 Scroll Lock -- the HID LED output report. Sent with
// SET_REPORT from the enumeration state machine; safe from any context.
// A keyboard that has just bound shows a rolling Num-Caps-Scroll cycle
// first, then this. sw/os/hid.c owns the state.
void z_usbh_kbd_leds(uint8_t leds);

// Low-speed timings (bring-up; usbh_hw.h, Z_USBH_TUNE). show prints
// them; set takes microseconds for the first three and full-speed bit
// times for the last. The shell's `usbtune`.
void z_usbh_tune_show(void);
int z_usbh_tune_set(int tmo_us, int turn_us, int gap_us, int pre_bits);
void z_usbh_cap_dump(void);

// Diagnostics for sh.c: how many devices are attached and what they
// are. Returns the count; fills typ[] with Z_USBH_TYP_* values.
int z_usbh_status(uint8_t *typ, int max);

#endif
