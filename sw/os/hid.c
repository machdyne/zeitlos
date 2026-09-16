/*
 * Zeitlos OS
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * USB HID keyboard interface.
 *
 * reg_usb0_info/reg_usb0_keys and reg_usb1_info/reg_usb1_keys
 * (rtl/usb_hid.v, two independent instances -- see rtl/sysctl.v) each
 * hold the *current* USB HID boot-protocol report for their own port
 * -- level state, not an event queue: up to 4 simultaneously-held
 * non-modifier keys plus a modifier byte, overwritten in place by
 * hardware every time a new report arrives on that port.
 *
 * z_hid_irq0()/z_hid_irq1() (called from z_kernel_entry() on
 * Z_IRQ_HID/Z_IRQ_HID1) are what turn that level state into
 * press/release *edge* events: each diffs its own port's new report
 * against that same port's previous one (hid_port_t below -- entirely
 * separate state per port, since either port might be the keyboard,
 * or might not be, independently of the other) and pushes one event
 * per key/modifier that actually changed into a single SHARED ring
 * buffer -- apps don't care which physical port a keystroke came from,
 * only that it happened, so there's no reason to expose two separate
 * queues. Apps drain it via Z_SYS_HID_READ_KEY (k_hid_read_key()
 * below) -- same non-blocking "pop or -1" shape as k_uart_getc()
 * (uart.c).
 *
 * Both Z_IRQ_HID and Z_IRQ_HID1 (cpu_irq[5]/cpu_irq[6], rtl/sysctl.v)
 * are wired straight from their own usb_hid_host.v instance's
 * `report` pulse -- fired for keyboard, mouse, *and* gamepad reports
 * alike (see rtl/ext/usb_hid_host/src/usb_hid_host.v). Both ISRs below
 * only act on typ==1 (keyboard) for their own port; mouse reports
 * still just update reg_usbN_cursor directly in hardware (rtl/usb_hid.v).
 * hid_irq_common() below unblocks whoever subscribed through
 * Z_SYS_HID_PTR_SUBSCRIBE, so that reader can read the register when
 * it actually changes rather than polling it every tick. It -- like
 * this file -- decides which port is currently "the mouse" by reading
 * both ports' own typ field, since there's no fixed port-to-device
 * mapping (see zeitlos.h).
 * rtl/sysctl.v's LATCHED_IRQ marks both these bits as edge-latched
 * specifically because `report` is only a single 12MHz-domain cycle
 * wide, so it wouldn't reliably still be visible by the time an ISR
 * actually runs a handful of (faster-clock) cycles later -- latching
 * captures the edge in hardware regardless.
 *
 * Deliberately raw here: this layer only knows USB HID usage codes,
 * not ASCII/keysyms -- see sw/common/zkbd.h (used by wm.c) for that
 * translation. Keeping it out of the kernel means keyboard layout
 * knowledge can change without a kernel rebuild+reflash, and keeps
 * these ISRs themselves small and fast.
 */

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>

#include "../common/zeitlos.h"
#include "../common/zkbd.h"
#include "kernel.h"
#include "hid.h"

/*
 * The process to wake when a POINTER report arrives, or 0 for nobody.
 *
 * The pointer is not an event queue and deliberately is not becoming
 * one. rtl/usb_hid.v maintains the cursor position as level state in
 * reg_usbN_cursor, and coalescing is a feature here rather than a
 * loss -- zwm.h already tells apps to act on the LAST mouse sample
 * they find rather than every one. So this wakes the reader and lets
 * it read the current position; it does not deliver anything.
 *
 * That also means no queue to overflow under fast motion, and no
 * allocation in an ISR.
 *
 * Set through Z_SYS_HID_PTR_SUBSCRIBE (k_hid_ptr_subscribe below).
 * One subscriber, because there is one pointer and one wm; a second
 * caller simply replaces the first.
 */
static uint32_t hid_ptr_pid;

// Unblock the subscriber, if there is one. Safe from interrupt
// context and from a syscall alike: k_proc_unblock() records a wakeup
// that arrives before the target has blocked (Z_PROC_FLAG_WAKE), so
// there is no window in which one can be lost.
static inline void hid_wake_subscriber(void) {
	if (hid_ptr_pid) k_proc_unblock(hid_ptr_pid);
}

// Same thing for input that does NOT come from the HID controller --
// see k_hid_inject() below and k_wm_wake() in kernel.c. Both deliver
// input the subscriber must notice, and neither raises an interrupt
// of its own.
void k_hid_wake_subscriber(void) {
	hid_wake_subscriber();
}

// HID_FIFO_SIZE lives in hid.h now, so sh.c's `ic` can say how deep
// the ring it is reporting on is.

// packed raw event: bit0 = pressed(1)/released(0), bits 8:1 = HID
// usage code, bits 16:9 = modifier byte at the time of the event.
// always < 0x80000000 as an int32_t (top 15 bits always zero), so it
// can never collide with k_hid_read_key()'s -1 "empty" sentinel. which
// port produced the event is deliberately not encoded -- see the file
// header comment, apps only ever want "a key event", not "a key event
// from port N".
#define HID_EVENT(usage, mods, pressed) \
	((((uint32_t)(mods) & 0xFF) << 9) | (((uint32_t)(usage) & 0xFF) << 1) | ((pressed) ? 1u : 0u))

static volatile uint32_t __attribute__((section(".bss"))) hid_fifo[HID_FIFO_SIZE];
static volatile uint8_t __attribute__((section(".bss"))) hid_head = 0, hid_tail = 0;

// per-port edge-detection state -- entirely independent, since either
// port might be a keyboard, a mouse, a gamepad, or nothing at all,
// with no relationship to what the other port currently is.
typedef struct {
	uint8_t modifiers;
	uint8_t keys[4];
} hid_port_t;

static hid_port_t port0, port1;

void z_hid_init(void) {
	port0.modifiers = 0;
	port0.keys[0] = port0.keys[1] = port0.keys[2] = port0.keys[3] = 0;
	port1.modifiers = 0;
	port1.keys[0] = port1.keys[1] = port1.keys[2] = port1.keys[3] = 0;
	hid_head = hid_tail = 0;
}

// The two halves of "did the ring lose anything?", for sh.c's `ic`
// command. Both wrap freely; what matters is whether dropped moves
// at all, and the gap between pushed and what the reader saw.
static volatile uint32_t hid_pushed, hid_dropped;

static void hid_push(uint32_t ev) {
	uint8_t next = (hid_head + 1) % HID_FIFO_SIZE;
	if (next == hid_tail) {
		hid_dropped++;
		return; // FIFO full -- drop the event (same
			// accepted tradeoff as uart.c's RX
			// overflow handling), now counted.
	}
	hid_fifo[hid_head] = ev;
	hid_head = next;
	hid_pushed++;
}

void k_hid_stats(uint32_t *pushed, uint32_t *dropped) {
	*pushed = hid_pushed;
	*dropped = hid_dropped;
}

// 0x00 = "no key in this slot", 0x01 = "phantom state/rollover error"
// per the USB HID boot protocol -- neither is a real keypress.
static inline bool hid_usage_valid(uint8_t usage) {
	return usage >= 0x04;
}

static inline bool key_in_set(const uint8_t *set, uint8_t usage) {
	return set[0] == usage || set[1] == usage || set[2] == usage || set[3] == usage;
}

// shared by both ports' ISRs -- st is that port's own hid_port_t,
// info/keys are that port's own current register values. runs in
// interrupt context (not itself preemptible by another IRQ), so
// unlike k_hid_read_key() below it doesn't need its own maskirq()
// around the hid_head/hid_tail update.
static void hid_irq_common(hid_port_t *st, uint32_t info, uint32_t keys) {

	uint8_t typ = (info >> 24) & 0x3;   // rtl/usb_hid.v: 1=keyboard, 2=mouse, 3=gamepad --
	                                     // bits[25:24] of the packed info register
	                                     // ({ report[31], 5'b0[30:26], typ[25:24],
	                                     // 16'b0[23:8], modifiers[7:0] }); NOT bits
	                                     // [23:22], which was this file's bug until
	                                     // now -- that range falls entirely inside
	                                     // the constant 16'b0 padding, so it always
	                                     // read 0 regardless of what was plugged in,
	                                     // silently discarding every keyboard event.

	// NOTE: deliberately not checking info's own report-pulse bit
	// (bit 31) here -- it's a single 12MHz-usbclk-cycle pulse, almost
	// certainly already deasserted again by the time this ISR runs.
	// The fact that this is running at all (the corresponding IRQ
	// fired, latched in hardware -- see the file header comment) is
	// already the evidence a report just arrived; typ/modifiers/keys
	// below are ordinary registered state that stays valid between
	// reports, not the pulse itself.

	// A report arrived. Wake whoever asked to be told.
	//
	// This is the whole reason wm can stop polling: the interrupt was
	// always firing for mouse reports -- rtl/usb_hid.v's `report`
	// pulse is wired to cpu_irq[5]/[6] for keyboard, mouse and
	// gamepad alike, see this file's header -- the ISR just dropped
	// them on the floor. Waking here costs a flag write.
	//
	// Before the typ != 1 flush below, deliberately: an unplug is
	// also a report, and a reader waiting on pointer activity should
	// be woken for that too rather than waiting out its timeout.
	//
	// Keyboard reports wake it as well, which is why this is not
	// conditional on typ. The subscriber is the process that reads
	// input for everyone, and it sleeps until input arrives rather
	// than waking on every tick to look -- so a keystroke that only
	// landed in the ring below, with nothing to wake the reader,
	// would sit there until some unrelated event happened to arrive.
	// A wake it did not need costs one pass around its loop.
	hid_wake_subscriber();

	if (typ != 1) {

		// NOT A KEYBOARD ON THIS PORT -- which now includes "not
		// anything on this port", because the device was unplugged.
		//
		// Returning early here (as this used to) leaks state. Held
		// keys live in st->keys until a later report shows them
		// absent, and after an unplug NO LATER REPORT EVER COMES:
		// usb_hid_host stops issuing them entirely. Yank a keyboard
		// mid-keypress and every app downstream believes that key is
		// still down, forever, with nothing able to correct it. Swap
		// a keyboard for a gamepad on the same port and the same
		// thing happens.
		//
		// So flush instead: synthesise the release events that the
		// departed device is no longer around to send, exactly as if
		// it had reported every key up before leaving. Consumers see
		// an ordinary release and need no knowledge of hotplug at
		// all. rtl/usb_hid.v raises the interrupt that gets us here
		// on a device type change specifically so this can run --
		// see its int_o comment.
		//
		// The modifier byte passed alongside is 0, not st->modifiers:
		// by the time these releases are delivered no modifier is
		// held either, and reporting a stale Shift on the release of
		// a key would be its own small lie.
		if (st->modifiers) {
			uint8_t changed = st->modifiers;
			for (int b = 0; b < 8; b++) {
				if (!(changed & (1 << b))) continue;
				hid_push(HID_EVENT(Z_HID_USAGE_LCTRL + b, 0, false));
			}
			st->modifiers = 0;
		}

		for (int i = 0; i < 4; i++) {
			uint8_t u = st->keys[i];
			if (hid_usage_valid(u)) hid_push(HID_EVENT(u, 0, false));
			st->keys[i] = 0;
		}

		return;

	}

	uint8_t modifiers = info & 0xFF;
	uint8_t cur_keys[4] = {
		(uint8_t)(keys >> 24), (uint8_t)(keys >> 16),
		(uint8_t)(keys >> 8),  (uint8_t)(keys)
	};

	// modifier edges -- one synthesized pseudo-usage (0xE0-0xE7, the
	// real USB HID usage IDs for these keys -- see zkbd.h) per changed bit
	if (modifiers != st->modifiers) {
		uint8_t changed = modifiers ^ st->modifiers;
		for (int b = 0; b < 8; b++) {
			if (!(changed & (1 << b))) continue;
			bool pressed = (modifiers & (1 << b)) != 0;
			hid_push(HID_EVENT(Z_HID_USAGE_LCTRL + b, modifiers, pressed));
		}
	}

	// releases: anything in st->keys no longer in cur_keys
	for (int i = 0; i < 4; i++) {
		uint8_t u = st->keys[i];
		if (!hid_usage_valid(u)) continue;
		if (!key_in_set(cur_keys, u))
			hid_push(HID_EVENT(u, modifiers, false));
	}

	// presses: anything in cur_keys not already in st->keys
	for (int i = 0; i < 4; i++) {
		uint8_t u = cur_keys[i];
		if (!hid_usage_valid(u)) continue;
		if (!key_in_set(st->keys, u))
			hid_push(HID_EVENT(u, modifiers, true));
	}

	st->modifiers = modifiers;
	st->keys[0] = cur_keys[0]; st->keys[1] = cur_keys[1];
	st->keys[2] = cur_keys[2]; st->keys[3] = cur_keys[3];

}

// called from z_kernel_entry() on Z_IRQ_HID (port 0)
void z_hid_irq0(void) {
	hid_irq_common(&port0, reg_usb0_info, reg_usb0_keys);
}

// called from z_kernel_entry() on Z_IRQ_HID1 (port 1)
void z_hid_irq1(void) {
	hid_irq_common(&port1, reg_usb1_info, reg_usb1_keys);
}

// Z_SYS_HID_PTR_SUBSCRIBE. Registers the caller as the process to
// wake on pointer reports. Returns the pid registered, so a caller
// can confirm it took.
z_obj_t *k_hid_ptr_subscribe(z_obj_t *obj) {

	hid_ptr_pid = z_pid;

	obj->type = Z_UINT32;
	obj->val.uint32 = hid_ptr_pid;

	return (&z_ok);

}

int32_t k_hid_read_key(void) {

	// protects hid_head/hid_tail against a concurrent z_hid_irq0()/
	// z_hid_irq1() -- this function can be called (via the syscall
	// trampoline) from any process's ordinary, interruptible code,
	// unlike those ISRs which only ever run from interrupt context.
	// same shape as k_uart_getc()'s protection against z_uart_irq()
	// (uart.c).
	uint32_t old_mask = maskirq(0xFFFFFFFF);

	if (hid_head == hid_tail) {
		maskirq(old_mask);
		return -1;
	}

	uint32_t ev = hid_fifo[hid_tail];
	hid_tail = (hid_tail + 1) % HID_FIFO_SIZE;

	maskirq(old_mask);
	return (int32_t)ev;

}

z_obj_t *z_hid_read_key(z_obj_t *obj) {
	obj->val.int32 = k_hid_read_key();
	return (&z_ok);
}

// Injects a raw packed event (HID_EVENT() layout) into the shared
// ring, under the same mask k_hid_read_key() uses so it cannot race
// the port ISRs. For network-sourced keystrokes.
z_obj_t *k_hid_inject(z_obj_t *obj) {
	uint32_t old_mask = maskirq(0xFFFFFFFF);
	hid_push((uint32_t)obj->val.int32);
	maskirq(old_mask);
	// Visor keystrokes have no HID IRQ of their own. Wake the HID
	// subscriber so it drains the ring instead of waiting for the
	// next USB report or timer.
	k_hid_wake_subscriber();
	return (&z_ok);
}
