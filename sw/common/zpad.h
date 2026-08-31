#ifndef ZPAD_H
#define ZPAD_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * USB HID gamepad.
 *
 * rtl/ext/usb_hid_host/src/usb_hid_host.v has decoded gamepads since
 * the day it was vendored in -- it detects typ==3 and copes with
 * several different pad report layouts. rtl/usb_hid.v simply never
 * connected those outputs to anything, so the information was being
 * produced and discarded. This header is the software half of that
 * gap being closed; there is no new protocol handling anywhere.
 *
 * Header-only, no separate .c, and polled directly from userspace --
 * same reasoning as sw/common/zsoc.h. These are plain MMIO reads at a
 * fixed physical address, exactly like reg_usb0_cursor, which
 * sw/apps/wm/wm.c already polls from userspace every main loop
 * iteration. There is nothing here the kernel needs to mediate:
 * unlike the keyboard, a pad has no event queue to own and no edge
 * detection that has to survive being missed -- the state is a level,
 * and a game reads it once per frame.
 *
 * -- two ports, and no fixed assignment --
 *
 * There are two USB host ports (Obst and Lakritz both wire both), and
 * WHICH DEVICE IS ON WHICH PORT IS NOT FIXED ANYWHERE. There is no
 * "the keyboard port". Either port may hold a keyboard, a mouse, a
 * gamepad or nothing, independently of the other, and that can change
 * while the machine is running.
 *
 * So this header does not talk about ports at all in its main API. It
 * talks about PAD INDICES: pad 0 is the lowest-numbered port that
 * currently reports a gamepad, pad 1 is the other one. A two-player
 * game asks for pad 0 and pad 1; if only one pad is plugged in, pad 1
 * simply is not present, whichever physical socket the one pad is in.
 *
 * Same approach sw/os/hid.c and wm.c already take for the keyboard
 * and mouse (see wm.c's mouse_port()), for the same reason.
 *
 * -- hot swapping --
 *
 * Supported, and it needed hardware work to be true rather than
 * nominally true. Unplugging is SILENT: reports simply stop arriving,
 * so a naive design freezes the pad state at whatever was last held.
 * Pull the cable mid-jump and the machine believes RIGHT is held down
 * forever, with no event ever coming to correct it.
 *
 * rtl/usb_hid.v therefore clears the pad state whenever its port is
 * not currently reporting a gamepad, and raises an interrupt on the
 * device type change so the OS is told at all. From up here the
 * effect is simply that an unplugged pad reads as all-buttons-up and
 * z_pad_present() goes false, which is what a caller would want to
 * happen and does not need to do anything to get.
 *
 * A game should still call z_pad_count() every frame rather than once
 * at startup. Cheap (two MMIO reads) and it means plugging a second
 * pad in mid-game just works.
 */

#include <stdint.h>
#include <stdbool.h>

#include "zeitlos.h"   /* reg_usb0_pad, reg_usb1_pad */

/* Gamepad state register, one per USB host port. Word offset 4 in
 * each port's own register block -- see rtl/usb_hid.v.
 *
 * Reads back { 20'b0, typ[1:0], buttons[9:0] }. The device type is in
 * the same word as the state on purpose: a pad with nothing pressed
 * reads as zero, which is indistinguishable from no pad at all, and
 * asking a second register would leave a window where a hotplug lands
 * between the two reads. One read, one clock domain, one answer. */
/* reg_usb0_pad / reg_usb1_pad themselves live in zeitlos.h, alongside
 * the rest of each port's register block, rather than being redefined
 * here -- one address, one place. */

/* Button bits -- KEEP IN SYNC with rtl/usb_hid.v's own game_state
 * concatenation. Bit position is the only thing that has to match;
 * there is no shared source between the Verilog and C halves, the
 * same hand-maintained split as zkbd.h's HID usage codes. */
#define Z_PAD_LEFT   (1u << 0)
#define Z_PAD_RIGHT  (1u << 1)
#define Z_PAD_UP     (1u << 2)
#define Z_PAD_DOWN   (1u << 3)
#define Z_PAD_A      (1u << 4)
#define Z_PAD_B      (1u << 5)
#define Z_PAD_X      (1u << 6)
#define Z_PAD_Y      (1u << 7)
#define Z_PAD_SELECT (1u << 8)
#define Z_PAD_START  (1u << 9)

#define Z_PAD_DPAD (Z_PAD_LEFT | Z_PAD_RIGHT | Z_PAD_UP | Z_PAD_DOWN)

#define Z_PAD_MAX_PORTS 2

/* device type, from the same word -- see rtl/usb_hid.v */
#define Z_PAD_TYP_SHIFT 10
#define Z_PAD_TYP_MASK  0x3u
#define Z_PAD_TYP_GAMEPAD 3u

/* Raw read of one PORT (0 or 1), not a pad index. Most callers want
 * z_pad_read() below instead; this is here for anything that genuinely
 * cares about physical sockets, such as a settings screen showing what
 * is plugged in where. */
static inline uint32_t z_pad_port_raw(int port) {
	return (port == 0) ? reg_usb0_pad : reg_usb1_pad;
}

static inline bool z_pad_port_is_gamepad(int port) {
	uint32_t r = z_pad_port_raw(port);
	return ((r >> Z_PAD_TYP_SHIFT) & Z_PAD_TYP_MASK) == Z_PAD_TYP_GAMEPAD;
}

/* Map a pad index to a physical port, or -1 if there is no such pad.
 *
 * Scans in port order every call rather than caching, so a pad
 * unplugged from port 0 while another sits in port 1 correctly
 * promotes the survivor to pad 0. Two register reads; not worth
 * caching, and a cache would be exactly the thing that got hot
 * swapping subtly wrong. */
static inline int z_pad_port(int pad) {
	int seen = 0;
	for (int p = 0; p < Z_PAD_MAX_PORTS; p++) {
		if (!z_pad_port_is_gamepad(p)) continue;
		if (seen == pad) return p;
		seen++;
	}
	return -1;
}

/* How many gamepads are attached right now, 0..2.
 *
 * Call this every frame, not once at startup -- it is two MMIO reads,
 * and doing so is what makes plugging a second pad in mid-game work
 * without any further effort. */
static inline int z_pad_count(void) {
	int n = 0;
	for (int p = 0; p < Z_PAD_MAX_PORTS; p++)
		if (z_pad_port_is_gamepad(p)) n++;
	return n;
}

static inline bool z_pad_present(int pad) {
	return z_pad_port(pad) >= 0;
}

/* Current button state for a pad index. Returns 0 for a pad that is
 * not present -- deliberately the same value as "present, nothing
 * pressed", because a caller that just wants to move a character
 * should not have to special-case an absent pad to get the right
 * behaviour, which is that the character stands still. Use
 * z_pad_present() when the difference matters (a "press start" screen,
 * a player-2 join prompt). */
static inline uint32_t z_pad_read(int pad) {
	int p = z_pad_port(pad);
	if (p < 0) return 0;
	return z_pad_port_raw(p) & 0x3ffu;
}

/* -- edge detection --
 *
 * The hardware reports a level: which buttons are down right now. A
 * game usually wants both that and the edges (jump on the press, not
 * every frame the button is held). Keep one of these per pad, call
 * z_pad_update() once per frame, and ask.
 *
 * Deliberately caller-owned state rather than a global: two players
 * need two of them, and a game with a pause menu may want a separate
 * tracker whose edges are not consumed by the gameplay code. */
typedef struct {
	uint32_t cur;
	uint32_t prev;
} z_pad_state_t;

static inline void z_pad_init(z_pad_state_t *s) {
	s->cur = 0;
	s->prev = 0;
}

static inline void z_pad_update(z_pad_state_t *s, int pad) {
	s->prev = s->cur;
	s->cur = z_pad_read(pad);
}

/* held down this frame */
static inline bool z_pad_down(const z_pad_state_t *s, uint32_t mask) {
	return (s->cur & mask) != 0;
}

/* went down between the last two updates */
static inline bool z_pad_pressed(const z_pad_state_t *s, uint32_t mask) {
	return ((s->cur & ~s->prev) & mask) != 0;
}

/* came up between the last two updates.
 *
 * Note this also fires when a pad is unplugged with a button held --
 * the state clears, so every held button reports a release. That is
 * the correct and useful behaviour: a character that was running does
 * not keep running because the cable came out. */
static inline bool z_pad_released(const z_pad_state_t *s, uint32_t mask) {
	return ((~s->cur & s->prev) & mask) != 0;
}

/* -- axis helpers --
 *
 * Resolve the d-pad to -1, 0 or +1. Opposite directions cancel to 0
 * rather than one winning: some pads genuinely report both for a
 * moment during a fast flick, and a character that briefly stands
 * still reads as a hesitation, whereas one that briefly reverses reads
 * as a bug. */
static inline int z_pad_axis_x(const z_pad_state_t *s) {
	bool l = (s->cur & Z_PAD_LEFT) != 0;
	bool r = (s->cur & Z_PAD_RIGHT) != 0;
	return (l == r) ? 0 : (r ? 1 : -1);
}

static inline int z_pad_axis_y(const z_pad_state_t *s) {
	bool u = (s->cur & Z_PAD_UP) != 0;
	bool d = (s->cur & Z_PAD_DOWN) != 0;
	return (u == d) ? 0 : (d ? 1 : -1);
}

#endif
